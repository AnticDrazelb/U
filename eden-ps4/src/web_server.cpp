// The app's web server (web_server.h): HTTP/1.1 over BSD sockets, one request per connection,
// each connection on its own thread. Uploads are PUT requests whose body is the file (the page
// sends them with XMLHttpRequest, for its progress bars), written through a ".part" name as they
// arrive: games of several GB never sit in memory.
//   GET  /                        the page (web_page.inc)
//   GET  /api/status              JSON: keys, firmware, games, logs
//   GET  /api/logs                every log as one text file, to download and send (code addresses
//                                 named, samples.log summed up: log_tools.h)
//   GET  /api/profile             samples.log summed up
//   GET  /files/<path>            one log (the data folder's .log and .txt files, and gpu/)
//   PUT  /upload/<kind>/<name>    kind keys (*.keys), firmware (*.nca, or a .zip of them: unpacked
//                                 here, replacing the firmware), games (.nsp .xci .nro .nso)
//                                 or updates (updates and DLC, .nsp: applied as their game boots)
//   POST /api/firmware/clear      removes the firmware (before a new one is uploaded)
//   DELETE /files/<name>          removes a game from the data folder
//   DELETE /updates/<name>        removes an update or DLC

#include "web_server.h"
#include "firmware_zip.h"
#include "log_tools.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace Launcher {

namespace {

namespace fs = std::filesystem;

#include "web_page.inc" // kWebPage

constexpr size_t kMaxHead = 16 * 1024;
constexpr int kMaxConnections = 6;
constexpr size_t kLogLimit = size_t(8) << 20; // per file in the log bundle (its end)
constexpr size_t kLogLimitWhileRunning = size_t(2) << 20; // (the program needs the memory)
constexpr size_t kMinFirmwareFiles = 10; // fewer .nca files than this in a zip: not a firmware

std::atomic<int> g_connections{0};

std::string Lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

bool EndsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && Lower(text.substr(text.size() - suffix.size())) == suffix;
}

bool IsGame(std::string_view name)
{
    return EndsWith(name, ".nsp") || EndsWith(name, ".xci") || EndsWith(name, ".nro") || EndsWith(name, ".nso");
}

bool IsLog(std::string_view name)
{
    return EndsWith(name, ".log") || EndsWith(name, ".txt");
}

std::string JsonString(std::string_view text)
{
    std::string out = "\"";
    for (const unsigned char c : text)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += char(c);
        }
        else if (c < 0x20)
        {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        }
        else
        {
            out += char(c);
        }
    }
    return out + "\"";
}

std::string UrlDecode(std::string_view text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit((unsigned char)text[i + 1]) &&
            std::isxdigit((unsigned char)text[i + 2]))
        {
            out += char(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        }
        else
        {
            out += text[i] == '+' ? ' ' : text[i];
        }
    }
    return out;
}

// A file name the browser sent: one path component, nothing that could leave its folder
bool SafeName(const std::string& name)
{
    if (name.empty() || name == "." || name == ".." || name.size() > 255)
        return false;
    return std::none_of(name.begin(), name.end(),
                        [](unsigned char c) { return c == '/' || c == '\\' || c < 0x20 || c == ':'; });
}

// The connection, with what was read past the request's head
class Connection
{
public:
    explicit Connection(int fd_) : fd{fd_} {}
    ~Connection() { close(fd); }

    bool SendAll(const void* data, size_t size)
    {
        const char* p = static_cast<const char*>(data);
        while (size > 0)
        {
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags |= MSG_NOSIGNAL;
#endif
            const ssize_t n = send(fd, p, size, flags);
            if (n <= 0)
            {
                if (n < 0 && errno == EINTR)
                    continue;
                return false;
            }
            p += n;
            size -= size_t(n);
        }
        return true;
    }
    bool Send(std::string_view text) { return SendAll(text.data(), text.size()); }

    // A response sent in parts (Send) as it's made, its end the connection's
    void Begin(int code, std::string_view reason, std::string_view type, std::string_view extra_headers = {})
    {
        Send("HTTP/1.1 " + std::to_string(code) + " " + std::string(reason) + "\r\nContent-Type: " +
             std::string(type) + "\r\nCache-Control: no-store\r\nConnection: close\r\n" +
             std::string(extra_headers) + "\r\n");
    }

    void Respond(int code, std::string_view reason, std::string_view type, std::string_view body,
                 std::string_view extra_headers = {})
    {
        std::string head = "HTTP/1.1 " + std::to_string(code) + " " + std::string(reason) +
                           "\r\nContent-Type: " + std::string(type) +
                           "\r\nContent-Length: " + std::to_string(body.size()) +
                           "\r\nCache-Control: no-store\r\nConnection: close\r\n" + std::string(extra_headers) + "\r\n";
        Send(head);
        Send(body);
    }
    void Error(int code, std::string_view reason, std::string_view message)
    {
        Respond(code, reason, "text/plain; charset=utf-8", std::string(message) + "\n");
    }

    // The request's head (up to its blank line); what follows it stays in `pending`
    bool ReadHead(std::string& head)
    {
        char buffer[4096];
        while (true)
        {
            const size_t end = pending.find("\r\n\r\n");
            if (end != std::string::npos)
            {
                head = pending.substr(0, end);
                pending.erase(0, end + 4);
                return true;
            }
            if (pending.size() > kMaxHead)
                return false;
            const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0)
            {
                if (n < 0 && errno == EINTR)
                    continue;
                return false;
            }
            pending.append(buffer, size_t(n));
        }
    }

    // Up to `size` bytes of the body; 0 at its end or on an error
    size_t ReadBody(char* out, size_t size)
    {
        if (!pending.empty())
        {
            const size_t n = std::min(size, pending.size());
            std::memcpy(out, pending.data(), n);
            pending.erase(0, n);
            return n;
        }
        while (true)
        {
            const ssize_t n = recv(fd, out, size, 0);
            if (n < 0 && errno == EINTR)
                continue;
            return n > 0 ? size_t(n) : 0;
        }
    }

private:
    int fd;
    std::string pending;
};

struct Request
{
    std::string method, path;
    long long content_length = -1;
    bool chunked = false;
    bool expect_continue = false; // (curl asks before sending a large body)
};

bool ParseHead(const std::string& head, Request& request)
{
    const size_t line_end = head.find("\r\n");
    const std::string first = head.substr(0, line_end);
    const size_t a = first.find(' '), b = first.rfind(' ');
    if (a == std::string::npos || b == a)
        return false;
    request.method = first.substr(0, a);
    request.path = first.substr(a + 1, b - a - 1);
    if (const size_t query = request.path.find('?'); query != std::string::npos)
        request.path.resize(query);
    size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
    while (pos < head.size())
    {
        size_t end = head.find("\r\n", pos);
        if (end == std::string::npos)
            end = head.size();
        const std::string line = head.substr(pos, end - pos);
        pos = end + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string name = Lower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        if (name == "content-length")
            request.content_length = std::atoll(value.c_str());
        else if (name == "transfer-encoding" && Lower(value).find("chunked") != std::string::npos)
            request.chunked = true;
        else if (name == "expect" && Lower(value).find("100-continue") != std::string::npos)
            request.expect_continue = true;
    }
    return true;
}

// The data folder's logs, and gpu/'s (relative paths), and their sizes
std::vector<std::pair<std::string, uintmax_t>> ListLogs(const fs::path& data)
{
    std::vector<std::pair<std::string, uintmax_t>> logs;
    for (const char* folder : {"", "gpu", "gpu/flight"})
    {
        std::error_code ec;
        for (fs::directory_iterator it(data / folder, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (it->is_regular_file(ec) && IsLog(name))
                logs.emplace_back((*folder ? std::string(folder) + "/" : std::string()) + name, it->file_size(ec));
        }
    }
    std::sort(logs.begin(), logs.end());
    return logs;
}

// The end of a file, at most `limit` bytes
std::string ReadEnd(const fs::path& path, size_t limit)
{
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> f{std::fopen(path.c_str(), "rb"), std::fclose};
    if (!f)
        return {};
    std::fseek(f.get(), 0, SEEK_END);
    const long size = std::ftell(f.get());
    const long start = size > long(limit) ? size - long(limit) : 0;
    std::fseek(f.get(), start, SEEK_SET);
    std::string text(size_t(size - start), '\0');
    text.resize(std::fread(text.data(), 1, text.size(), f.get()));
    return text;
}

// Every log, one after another, each read and handed to `write` on its own, with the app's function
// names for the code addresses in them (unless `symbols` is empty) and samples.log summed up
bool WriteBundle(const fs::path& data, const std::string& version, const Symbols& symbols, bool running,
                 const std::function<bool(std::string_view)>& write)
{
    const size_t limit = running ? kLogLimitWhileRunning : kLogLimit;
    std::string head = "Eden for PS4 " + version + ": every log in " + data.string() + "\n";
    if (running)
        head += "(a program is running: each log's last 2 MiB, code addresses without names; for names, "
                "download them again from the HOME menu)\n";
    else if (symbols.Empty())
        head += "(no symbols.zst in this build: code addresses without names)\n";
    if (!write(head))
        return false;
    for (const auto& [name, size] : ListLogs(data))
    {
        std::string text = ReadEnd(data / name, limit);
        std::string part = "\n===== " + name + " (" + std::to_string(size) + " bytes" +
                           (size > limit ? ", its last " + std::to_string(limit >> 20) + " MiB" : "") + ") =====\n";
        if (name == "samples.log")
            part += ProfileSummary(text, symbols) + "\n----- samples.log itself -----\n";
        part += symbols.Empty() ? std::move(text) : SymbolizeLog(text, symbols);
        if (!part.empty() && part.back() != '\n')
            part += '\n';
        if (!write(part))
            return false;
    }
    return true;
}

class Server
{
public:
    explicit Server(WebServerConfig config_) : config{std::move(config_)}, data{config.data_dir} {}

    void Handle(int fd)
    {
        Connection connection(fd);
        std::string head;
        Request request;
        if (!connection.ReadHead(head) || !ParseHead(head, request))
            return;
        const std::string& path = request.path;
        if (request.method == "GET" && (path == "/" || path == "/index.html"))
            connection.Respond(200, "OK", "text/html; charset=utf-8", kWebPage);
        else if (request.method == "GET" && path == "/api/status")
            connection.Respond(200, "OK", "application/json", Status());
        else if (request.method == "GET" && path == "/api/logs")
            SendLogBundle(connection);
        else if (request.method == "GET" && path == "/api/profile")
            SendProfile(connection);
        else if (request.method == "GET" && path.starts_with("/files/"))
            SendLog(connection, UrlDecode(path.substr(7)));
        else if (request.method == "PUT" && path.starts_with("/upload/"))
            Upload(connection, request, path.substr(8));
        else if (request.method == "POST" && path == "/api/firmware/clear")
            ClearFirmware(connection);
        else if (request.method == "DELETE" && path.starts_with("/files/"))
            DeleteGame(connection, UrlDecode(path.substr(7)));
        else if (request.method == "DELETE" && path.starts_with("/updates/"))
            DeleteUpdate(connection, UrlDecode(path.substr(9)));
        else
            connection.Error(404, "Not Found", "Nothing here.");
    }

private:
    fs::path Registered() const { return data / "user" / "nand" / "system" / "Contents" / "registered"; }

    void Log(const std::string& line)
    {
        if (config.log)
            config.log("web: " + line);
    }
    void Changed()
    {
        if (config.changed)
            config.changed();
    }

    // The data folder's logs, and gpu/'s (relative paths), and their sizes
    std::vector<std::pair<std::string, uintmax_t>> Logs() const { return ListLogs(data); }

    std::string Status() const
    {
        std::error_code ec;
        size_t firmware = 0;
        for (fs::directory_iterator it(Registered(), ec), end; !ec && it != end; it.increment(ec))
            firmware++;
        std::string games = "[";
        for (fs::directory_iterator it(data, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (it->is_regular_file(ec) && IsGame(name))
                games += (games.size() > 1 ? "," : "") + std::string("{\"name\":") + JsonString(name) +
                         ",\"size\":" + std::to_string(it->file_size(ec)) + "}";
        }
        games += "]";
        std::string updates = "[";
        for (fs::directory_iterator it(data / "updates", ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (it->is_regular_file(ec) && EndsWith(name, ".nsp"))
                updates += (updates.size() > 1 ? "," : "") + std::string("{\"name\":") + JsonString(name) +
                           ",\"size\":" + std::to_string(it->file_size(ec)) + "}";
        }
        ec.clear();
        updates += "]";
        std::string logs = "[";
        for (const auto& [name, size] : Logs())
            logs += (logs.size() > 1 ? "," : "") + std::string("{\"name\":") + JsonString(name) +
                    ",\"size\":" + std::to_string(size) + "}";
        logs += "]";
        std::string json = "{\"version\":" + JsonString(config.version) +
                           ",\"keys\":" + (fs::exists(data / "user/keys/prod.keys", ec) ? "true" : "false") +
                           ",\"title_keys\":" + (fs::exists(data / "user/keys/title.keys", ec) ? "true" : "false") +
                           ",\"firmware\":" + std::to_string(firmware) + ",\"games\":" + games +
                           ",\"updates\":" + updates + ",\"logs\":" + logs;
        if (config.status)
        {
            const std::string extra = config.status();
            if (!extra.empty())
                json += "," + extra;
        }
        return json + "}";
    }

    bool ProgramRunning() const { return config.program_running && config.program_running(); }

    // Every log, one after another, each read and sent on its own (a program may be running).
    // With the app's function names for the code addresses in them, unless a program runs.
    void SendLogBundle(Connection& connection)
    {
        connection.Begin(200, "OK", "text/plain; charset=utf-8",
                         "Content-Disposition: attachment; filename=\"eden-ps4-logs.txt\"\r\n");
        const bool running = ProgramRunning();
        std::unique_lock lock{symbols_mutex, std::defer_lock}; // (one bundle with names at a time)
        Symbols symbols;
        if (!running && !config.symbols_path.empty())
        {
            lock.lock();
            symbols.Load(config.symbols_path);
        }
        WriteBundle(data, config.version, symbols, running,
                    [&](std::string_view part) { return connection.Send(part); });
    }

    void SendProfile(Connection& connection)
    {
        const fs::path samples = data / "samples.log";
        std::error_code ec;
        if (!fs::exists(samples, ec))
        {
            connection.Error(404, "Not Found",
                             "No samples.log: turn on System Settings, CPU, Profiler, then play a game for a while.");
            return;
        }
        const bool running = ProgramRunning();
        std::unique_lock lock{symbols_mutex, std::defer_lock};
        Symbols symbols;
        if (!running && !config.symbols_path.empty())
        {
            lock.lock();
            symbols.Load(config.symbols_path);
        }
        connection.Respond(200, "OK", "text/plain; charset=utf-8",
                           ProfileSummary(ReadEnd(samples, running ? kLogLimitWhileRunning : kLogLimit), symbols));
    }

    void SendLog(Connection& connection, const std::string& relative)
    {
        // only the logs Status lists (never the keys)
        const auto logs = Logs();
        if (std::none_of(logs.begin(), logs.end(), [&](const auto& log) { return log.first == relative; }))
        {
            connection.Error(404, "Not Found", "No such log.");
            return;
        }
        connection.Respond(200, "OK", "text/plain; charset=utf-8", ReadEnd(data / relative, size_t(64) << 20));
    }

    void Upload(Connection& connection, const Request& request, const std::string& rest)
    {
        const size_t slash = rest.find('/');
        const std::string kind = rest.substr(0, slash);
        const std::string name = slash == std::string::npos ? std::string() : UrlDecode(rest.substr(slash + 1));
        fs::path folder;
        const bool firmware_zip = kind == "firmware" && EndsWith(name, ".zip");
        if (kind == "keys" && EndsWith(name, ".keys"))
            folder = data / "user" / "keys";
        else if (kind == "firmware" && EndsWith(name, ".nca"))
            folder = Registered();
        else if (firmware_zip)
            folder = data / "tmp";
        else if (kind == "games" && IsGame(name))
            folder = data;
        else if (kind == "updates" && EndsWith(name, ".nsp"))
            folder = data / "updates";
        if (folder.empty() || !SafeName(name))
        {
            connection.Error(400, "Bad Request", "Not a file that goes there: " + name);
            return;
        }
        if (request.chunked || request.content_length < 0)
        {
            connection.Error(411, "Length Required", "The upload needs its length.");
            return;
        }
        std::error_code ec;
        fs::create_directories(folder, ec);
        const fs::path target = folder / (firmware_zip ? std::string("firmware.zip") : name);
        const fs::path partial = target.string() + ".part";
        std::unique_ptr<std::FILE, int (*)(std::FILE*)> out{std::fopen(partial.c_str(), "wb"), std::fclose};
        if (!out)
        {
            connection.Error(500, "Internal Server Error", "Can't write " + partial.string());
            return;
        }
        if (request.expect_continue)
            connection.Send("HTTP/1.1 100 Continue\r\n\r\n");
        std::vector<char> buffer(size_t(1) << 20);
        unsigned long long left = (unsigned long long)request.content_length;
        bool ok = true;
        while (left > 0 && ok)
        {
            const size_t n = connection.ReadBody(buffer.data(), size_t(std::min<unsigned long long>(left, buffer.size())));
            if (n == 0)
                ok = false;
            else if (std::fwrite(buffer.data(), 1, n, out.get()) != n)
                ok = false;
            else
                left -= n;
        }
        if (std::fclose(out.release()) != 0)
            ok = false;
        if (ok)
            fs::rename(partial, target, ec);
        if (!ok || ec)
        {
            fs::remove(partial, ec);
            Log("upload of " + name + " failed");
            connection.Error(500, "Internal Server Error",
                             left > 0 ? "The upload was cut short." : "Writing it failed (is the storage full?).");
            return;
        }
        Log("uploaded " + target.string() + " (" + std::to_string(request.content_length) + " bytes)");
        if (firmware_zip)
        {
            InstallFirmwareZip(connection, target, name);
            return;
        }
        Changed();
        connection.Respond(200, "OK", "text/plain; charset=utf-8", "ok\n");
    }

    // A firmware's zip, just uploaded: its .nca files replace the firmware there, then it goes
    void InstallFirmwareZip(Connection& connection, const fs::path& zip, const std::string& name)
    {
        std::error_code ec;
        const size_t ncas = CountZipNcas(zip);
        if (ncas < kMinFirmwareFiles)
        {
            fs::remove(zip, ec);
            connection.Error(400, "Bad Request",
                             ncas ? name + " has only " + std::to_string(ncas) + " .nca files: that isn't a firmware."
                                  : name + " isn't a zip this can read, or has no .nca files in it.");
            return;
        }
        const ZipExtract result = Launcher::InstallFirmwareZip(zip, Registered());
        fs::remove(zip, ec);
        Changed();
        if (!result.error.empty())
        {
            Log("firmware from " + name + ": stopped after " + std::to_string(result.files) + " files: " + result.error);
            connection.Error(500, "Internal Server Error",
                             "Stopped after " + std::to_string(result.files) + " files: " + result.error);
            return;
        }
        Log("firmware from " + name + ": " + std::to_string(result.files) + " files");
        connection.Respond(200, "OK", "text/plain; charset=utf-8",
                           "ok\nThe firmware is installed: " + std::to_string(result.files) + " files.\n");
    }

    void ClearFirmware(Connection& connection)
    {
        std::error_code ec;
        const uintmax_t removed = RemoveFolder(Registered());
        fs::create_directories(Registered(), ec);
        Log("firmware cleared (" + std::to_string(removed) + " entries)");
        Changed();
        connection.Respond(200, "OK", "text/plain; charset=utf-8", "ok\n");
    }

    void DeleteUpdate(Connection& connection, const std::string& name)
    {
        std::error_code ec;
        const fs::path file = data / "updates" / name;
        if (!SafeName(name) || !EndsWith(name, ".nsp") || !fs::is_regular_file(file, ec))
        {
            connection.Error(404, "Not Found", "No such update or DLC.");
            return;
        }
        fs::remove(file, ec);
        Log("removed " + file.string());
        Changed();
        connection.Respond(200, "OK", "text/plain; charset=utf-8", "ok\n");
    }

    void DeleteGame(Connection& connection, const std::string& name)
    {
        std::error_code ec;
        if (!SafeName(name) || !IsGame(name) || !fs::is_regular_file(data / name, ec))
        {
            connection.Error(404, "Not Found", "No such game in the data folder.");
            return;
        }
        fs::remove(data / name, ec);
        Log("removed " + (data / name).string());
        Changed();
        connection.Respond(200, "OK", "text/plain; charset=utf-8", "ok\n");
    }

    WebServerConfig config;
    fs::path data;
    std::mutex symbols_mutex;
};

void SetTimeouts(int fd, int seconds)
{
    timeval timeout{};
    timeout.tv_sec = seconds;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

} // namespace

bool WriteLogBundle(const std::string& data_dir, const std::string& version, const std::string& symbols_path,
                   const std::filesystem::path& file)
{
    Symbols symbols; // (no program runs: the names are loaded, ~11 MiB, and go again)
    if (!symbols_path.empty())
        symbols.Load(symbols_path);
    const fs::path partial = file.string() + ".part";
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> out{std::fopen(partial.c_str(), "wb"), std::fclose};
    if (!out)
        return false;
    const bool written = WriteBundle(data_dir, version, symbols, false, [&](std::string_view part) {
        return std::fwrite(part.data(), 1, part.size(), out.get()) == part.size();
    });
    std::error_code ec;
    if (std::fclose(out.release()) != 0 || !written)
    {
        fs::remove(partial, ec);
        return false;
    }
    fs::rename(partial, file, ec);
    return !ec;
}

bool StartWebServer(WebServerConfig config)
{
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        return false;
    const int on = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 8) != 0)
    {
        close(listener);
        return false;
    }
    if (config.log)
        config.log("web: listening on port " + std::to_string(config.port));
    auto server = std::make_shared<Server>(std::move(config));
    std::thread([listener, server] {
        while (true)
        {
            const int fd = accept(listener, nullptr, nullptr);
            if (fd < 0)
            {
                if (errno == EINTR || errno == ECONNABORTED)
                    continue;
                usleep(100 * 1000);
                continue;
            }
            if (g_connections.load() >= kMaxConnections)
            {
                static constexpr char kBusy[] = "HTTP/1.1 503 Busy\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                send(fd, kBusy, sizeof(kBusy) - 1, 0);
                close(fd);
                continue;
            }
            SetTimeouts(fd, 120);
            g_connections++;
            std::thread([fd, server] {
                server->Handle(fd);
                g_connections--;
            }).detach();
        }
    }).detach();
    return true;
}

std::string LocalAddress()
{
    // A UDP socket "connected" to an outside address sends nothing, but picks the interface (and
    // so the address) the network reaches this machine at
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return {};
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    remote.sin_addr.s_addr = htonl(0x01010101); // 1.1.1.1
    std::string result;
    if (connect(fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0)
    {
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        char text[INET_ADDRSTRLEN] = {};
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) == 0 && local.sin_addr.s_addr != 0 &&
            inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)))
            result = text;
    }
    close(fd);
    return result;
}

} // namespace Launcher

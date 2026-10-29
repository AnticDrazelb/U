// The launcher on a desktop: eden-ps4 <program.nro>. Output on stdout; Eden's data folders in
// $EDEN_PS4_DATA (default: a new temporary folder). Exit code 0 if the program exited, 1 if it
// didn't load, 2 if it was still running after $EDEN_PS4_TIMEOUT seconds (default 60), 3 if it was
// stopped (EDEN_PS4_INPUT's Stop).
//
// For testing programs that draw and read the controller, without a screen or a controller:
//   EDEN_PS4_FRAMES=<dir>   saves frames there as PPM images: frame-0001.ppm (the first), then
//                           one every second, and last.ppm (the last one, at exit)
//   EDEN_PS4_LOG=<filter>   Eden's log filter, e.g. "*:Info Service.VI:Debug" (default: warnings)
//   EDEN_PS4_INPUT=<script> player 1's buttons over time: "<seconds>:<buttons>,..." with the
//                           buttons joined by '+' (A B X Y L R ZL ZR Plus Minus Left Up Right
//                           Down LStick RStick) or none, e.g. "3:A,3.2:,5:Right+B,6:"; "Stop"
//                           stops the program (as L3+R3 held on the PS4)
//   EDEN_PS4_AUDIO=<file>   records the sound to a WAV file, in real time (a second stream Eden
//                           opens goes to <file>.2.wav, and so on)
//   EDEN_PS4_BOOTS=<n>      boots it n times in one process (as the PS4's HOME menu runs one
//                           program after another), with the heap in use after each

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <malloc.h>

#include "launcher.h"

namespace {

void PrintLine(Launcher::LineKind kind, std::string_view text) {
    const char* prefix = "";
    switch (kind) {
    case Launcher::LineKind::Program:
        break;
    case Launcher::LineKind::Warning:
        prefix = "eden: ";
        break;
    case Launcher::LineKind::Error:
        prefix = "eden error: ";
        break;
    case Launcher::LineKind::Status:
        prefix = "eden-ps4: ";
        break;
    case Launcher::LineKind::Info:
        prefix = "eden: ";
        break;
    }
    std::printf("%s%.*s\n", prefix, static_cast<int>(text.size()), text.data());
    std::fflush(stdout);
}

std::string g_frames_dir;
std::mutex g_frame_mutex;
std::vector<uint32_t> g_last_frame;
uint32_t g_last_width = 0, g_last_height = 0;
unsigned g_frame_count = 0;
std::chrono::steady_clock::time_point g_next_save;

void WritePpm(const std::string& path, std::span<const uint32_t> pixels, uint32_t width, uint32_t height) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", width, height);
    std::vector<unsigned char> row(size_t(width) * 3);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t p = pixels[size_t(y) * width + x];
            row[x * 3] = p & 0xFF;
            row[x * 3 + 1] = (p >> 8) & 0xFF;
            row[x * 3 + 2] = (p >> 16) & 0xFF;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

void SaveFrame(std::span<const uint32_t> pixels, uint32_t width, uint32_t height) {
    std::scoped_lock lock{g_frame_mutex};
    g_last_frame.assign(pixels.begin(), pixels.end());
    g_last_width = width;
    g_last_height = height;
    const auto now = std::chrono::steady_clock::now();
    if (++g_frame_count == 1 || now >= g_next_save) {
        char name[64];
        std::snprintf(name, sizeof(name), "/frame-%04u.ppm", g_frame_count);
        WritePpm(g_frames_dir + name, pixels, width, height);
        g_next_save = now + std::chrono::seconds(1);
    }
}

uint32_t ParseButtons(const std::string& text) {
    static const std::pair<const char*, uint32_t> names[] = {
        {"A", Launcher::ButtonA}, {"B", Launcher::ButtonB}, {"X", Launcher::ButtonX},
        {"Y", Launcher::ButtonY}, {"L", Launcher::ButtonL}, {"R", Launcher::ButtonR},
        {"ZL", Launcher::ButtonZL}, {"ZR", Launcher::ButtonZR}, {"Plus", Launcher::ButtonPlus},
        {"Minus", Launcher::ButtonMinus}, {"Left", Launcher::ButtonLeft}, {"Up", Launcher::ButtonUp},
        {"Right", Launcher::ButtonRight}, {"Down", Launcher::ButtonDown},
        {"LStick", Launcher::ButtonLStick}, {"RStick", Launcher::ButtonRStick}};
    uint32_t buttons = 0;
    std::stringstream ss(text);
    std::string name;
    while (std::getline(ss, name, '+')) {
        for (const auto& [n, bit] : names) {
            if (name == n) {
                buttons |= bit;
            }
        }
    }
    return buttons;
}

// Plays EDEN_PS4_INPUT: each step sets player 1's buttons at its time.
void PlayInput(std::string script, std::atomic<bool>* done) {
    const auto start = std::chrono::steady_clock::now();
    std::stringstream ss(script);
    std::string step;
    while (std::getline(ss, step, ',') && !*done) {
        const size_t colon = step.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const auto at = start + std::chrono::milliseconds(long(std::atof(step.c_str()) * 1000));
        while (std::chrono::steady_clock::now() < at && !*done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (step.substr(colon + 1) == "Stop") {
            Launcher::Stop();
            continue;
        }
        Launcher::PadState state;
        state.buttons = ParseButtons(step.substr(colon + 1));
        Launcher::SetPad(state);
    }
}

// EDEN_PS4_AUDIO: a WAV file per output, written at the pace a sound card would take it.
std::string g_audio_path;
std::atomic<int> g_audio_outputs{0};

class WavOutput final : public Launcher::AudioOutput {
public:
    ~WavOutput() override {
        if (file) {
            WriteHeader();
            std::fclose(file);
        }
    }
    bool Open(uint32_t frames_, uint32_t channels_) override {
        const int n = ++g_audio_outputs;
        const std::string path = n == 1 ? g_audio_path : g_audio_path + "." + std::to_string(n) + ".wav";
        file = std::fopen(path.c_str(), "wb");
        if (!file) {
            return false;
        }
        frames = frames_;
        channels = channels_;
        WriteHeader();
        next = std::chrono::steady_clock::now();
        return true;
    }
    void Output(std::span<const int16_t> samples) override {
        std::fwrite(samples.data(), sizeof(int16_t), samples.size(), file);
        data_bytes += uint32_t(samples.size() * sizeof(int16_t));
        if (++buffers % 100 == 0) {
            WriteHeader(); // a file that's complete so far, should the run be cut short
        }
        next += std::chrono::microseconds(uint64_t(frames) * 1'000'000 / 48'000);
        std::this_thread::sleep_until(next);
    }

private:
    void WriteHeader() {
        const long at = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
        unsigned char h[44];
        const auto put32 = [&](int o, uint32_t v) { std::memcpy(h + o, &v, 4); };
        const auto put16 = [&](int o, uint16_t v) { std::memcpy(h + o, &v, 2); };
        std::memcpy(h, "RIFF", 4);
        put32(4, 36 + data_bytes);
        std::memcpy(h + 8, "WAVEfmt ", 8);
        put32(16, 16);
        put16(20, 1); // PCM
        put16(22, uint16_t(channels));
        put32(24, 48'000);
        put32(28, 48'000 * channels * 2);
        put16(32, uint16_t(channels * 2));
        put16(34, 16);
        std::memcpy(h + 36, "data", 4);
        put32(40, data_bytes);
        std::fwrite(h, 1, sizeof(h), file);
        std::fseek(file, at > 44 ? at : 44, SEEK_SET);
    }

    FILE* file = nullptr;
    uint32_t frames = 0, channels = 2, data_bytes = 0, buffers = 0;
    std::chrono::steady_clock::time_point next;
};

std::unique_ptr<Launcher::AudioOutput> MakeWavOutput() {
    return std::make_unique<WavOutput>();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <program.nro>\n", argv[0]);
        return 1;
    }
    const std::filesystem::path program = std::filesystem::absolute(argv[1]);

    std::string data_dir;
    if (const char* env = std::getenv("EDEN_PS4_DATA")) {
        data_dir = env;
    } else {
        char tmpl[] = "/tmp/eden-ps4-XXXXXX";
        if (!mkdtemp(tmpl)) {
            std::perror("mkdtemp");
            return 1;
        }
        data_dir = tmpl;
    }
    const char* timeout_env = std::getenv("EDEN_PS4_TIMEOUT");
    const auto timeout = std::chrono::seconds(timeout_env ? std::atoi(timeout_env) : 60);

    Launcher::FrameSink frames = nullptr;
    if (const char* env = std::getenv("EDEN_PS4_FRAMES")) {
        g_frames_dir = env;
        std::filesystem::create_directories(g_frames_dir);
        frames = SaveFrame;
    }
    if (const char* env = std::getenv("EDEN_PS4_AUDIO")) {
        g_audio_path = env;
        Launcher::SetAudio(MakeWavOutput);
    }
    std::atomic<bool> done{false};
    std::thread input;
    // Eden's disk shader cache and asynchronous shaders (off unless set to 1)
    const auto flag = [](const char* name) {
        const char* value = std::getenv(name);
        return value && std::string_view{value} == "1";
    };
    Launcher::SetShaderCache(flag("EDEN_PS4_SHADER_CACHE"));
    Launcher::SetAsyncShaders(flag("EDEN_PS4_ASYNC_SHADERS"));
    Launcher::SetGpuAccuracyHigh(flag("EDEN_PS4_GPU_HIGH"));
    if (const char* env = std::getenv("EDEN_PS4_INPUT")) {
        input = std::thread(PlayInput, std::string(env), &done);
    }

    // EDEN_PS4_BOOTS=N boots it N times in this process, as the PS4's HOME menu does one program
    // after another, and says how much heap each run leaves behind
    const char* boots_env = std::getenv("EDEN_PS4_BOOTS");
    const int boots = std::max(1, boots_env ? std::atoi(boots_env) : 1);
    const auto heap_in_use = [] {
        const struct mallinfo2 info = mallinfo2();
        return std::pair{info.uordblks >> 20, info.hblkhd >> 20};
    };
    const auto heap_before = heap_in_use();
    Launcher::BootResult result{};
    for (int boot = 0; boot < boots; boot++) {
        result = Launcher::Boot(program, data_dir, PrintLine, frames, timeout);
        if (boots > 1) {
            const auto heap = heap_in_use();
            std::printf("eden-ps4: boot %d of %d done, heap in use %zu MiB + %zu MiB in large blocks "
                        "(%zu + %zu before the first)\n",
                        boot + 1, boots, heap.first, heap.second, heap_before.first, heap_before.second);
        }
    }

    done = true;
    if (input.joinable()) {
        input.join();
    }
    if (frames && !g_last_frame.empty()) {
        WritePpm(g_frames_dir + "/last.ppm", g_last_frame, g_last_width, g_last_height);
        std::printf("eden-ps4: %u frames, the last in %s/last.ppm\n", g_frame_count, g_frames_dir.c_str());
    }
    switch (result.status) {
    case Launcher::BootResult::Status::Exited:
        return 0;
    case Launcher::BootResult::Status::LoadFailed:
        return 1;
    case Launcher::BootResult::Status::TimedOut:
        return 2;
    case Launcher::BootResult::Status::Stopped:
        return 3;
    }
    return 1;
}

// The launcher on a desktop: eden-ps4 <program.nro>. Output on stdout; Eden's data folders in
// $EDEN_PS4_DATA (default: a new temporary folder). Exit code 0 if the program exited, 1 if it
// didn't load, 2 if it was still running after $EDEN_PS4_TIMEOUT seconds (default 60).
//
// For testing programs that draw and read the controller, without a screen or a controller:
//   EDEN_PS4_FRAMES=<dir>   saves frames there as PPM images: frame-0001.ppm (the first), then
//                           one every second, and last.ppm (the last one, at exit)
//   EDEN_PS4_LOG=<filter>   Eden's log filter, e.g. "*:Info Service.VI:Debug" (default: warnings)
//   EDEN_PS4_INPUT=<script> player 1's buttons over time: "<seconds>:<buttons>,..." with the
//                           buttons joined by '+' (A B X Y L R ZL ZR Plus Minus Left Up Right
//                           Down LStick RStick) or none, e.g. "3:A,3.2:,5:Right+B,6:"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
        Launcher::PadState state;
        state.buttons = ParseButtons(step.substr(colon + 1));
        Launcher::SetPad(state);
    }
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
    std::atomic<bool> done{false};
    std::thread input;
    if (const char* env = std::getenv("EDEN_PS4_INPUT")) {
        input = std::thread(PlayInput, std::string(env), &done);
    }

    const auto result = Launcher::Boot(program, data_dir, PrintLine, frames, timeout);

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
    }
    return 1;
}

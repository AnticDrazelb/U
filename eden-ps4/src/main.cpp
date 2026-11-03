// The Eden PS4 launcher's core: boots a Switch program in Eden's core (kernel, scheduler, CPU
// cores, HLE services, loader) with the null renderer, whose frames (composed on the CPU) go to
// the platform; or, on the PS4 with Mesa's RADV linked, Eden's Vulkan renderer drawing straight to
// the screen. Sound goes to the platform's output through Eden's frontend sink.
// What the program prints, and Eden's warnings and errors, go to the platform's sink and to
// log.txt.

#include <cstdint>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include "audio_core/sink/frontend_sink.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/perf_stats.h"
#include "core/cpu_manager.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "launcher.h"
#include "network/network.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#ifdef __OPENORBIS__
#include "common/ps4_diagnostics.h"
#include "common/ps4_perf.h"
#endif

namespace Launcher {
namespace {

/// The FPS counter (System Settings, Graphics) on the null renderer's frames, as the Vulkan
/// renderer's present pass draws it (patch 0063): the frames of the last second, seven-segment
/// digits in a dark box at the top left, drawn into a copy of the frame. Each frame the null
/// renderer composes is one the game presented.
class FpsCounter {
public:
    std::span<const u32> Draw(std::span<const u32> pixels, u32 width, u32 height) {
        frames++;
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - start).count();
        if (seconds >= 1.0) {
            shown = static_cast<u32>(frames / seconds + 0.5);
            frames = 0;
            start = now;
        }
        if (pixels.size() < size_t(width) * height) {
            return pixels;
        }
        copy.assign(pixels.begin(), pixels.end());
        const auto fill = [&](s32 x, s32 y, s32 w, s32 h, u32 color) {
            const s32 left = std::max(x, 0), right = std::min<s32>(x + w, static_cast<s32>(width));
            const s32 bottom = std::min<s32>(y + h, static_cast<s32>(height));
            for (s32 row = std::max(y, 0); row < bottom && left < right; row++) {
                const auto line = copy.begin() + static_cast<ptrdiff_t>(row) * width;
                std::fill(line + left, line + right, color);
            }
        };
        // segments a b c d e f g, from the top clockwise, then the middle (pixels are R in the
        // low byte: black, and a light yellow)
        static constexpr u8 kSegments[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
        constexpr u32 kDark = 0xFF000000, kLight = 0xFF4DFFFF;
        const s32 digit_height = std::max<s32>(static_cast<s32>(height) / 24, 14);
        const s32 digit_width = digit_height / 2;
        const s32 thick = std::max(digit_width / 5, 2);
        const s32 gap = thick * 2, pad = thick * 2, half = digit_height / 2;
        const std::string text = std::to_string(std::min<u32>(shown, 999));
        const s32 count = static_cast<s32>(text.size());
        const s32 x0 = pad * 2, y0 = pad * 2;
        fill(x0 - pad, y0 - pad, count * digit_width + (count - 1) * gap + pad * 2, digit_height + pad * 2,
             kDark);
        for (s32 i = 0; i < count; i++) {
            const u8 on = kSegments[text[static_cast<size_t>(i)] - '0'];
            const s32 x = x0 + i * (digit_width + gap);
            const s32 segments[7][4] = {
                {x, y0, digit_width, thick},                                // a
                {x + digit_width - thick, y0, thick, half},                 // b
                {x + digit_width - thick, y0 + half, thick, half},          // c
                {x, y0 + digit_height - thick, digit_width, thick},         // d
                {x, y0 + half, thick, half},                                // e
                {x, y0, thick, half},                                       // f
                {x, y0 + half - thick / 2, digit_width, thick},             // g
            };
            for (int s = 0; s < 7; s++) {
                if (on & (1 << s)) {
                    fill(segments[s][0], segments[s][1], segments[s][2], segments[s][3], kLight);
                }
            }
        }
        return copy;
    }

private:
    std::vector<u32> copy;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    u32 frames = 0, shown = 0;
};

/// A window without a GPU behind it: the null renderer composes frames drawn by the CPU, which
/// go to the platform's frame sink.
class HeadlessWindow final : public Core::Frontend::EmuWindow {
public:
    explicit HeadlessWindow(FrameSink frames_) : frames{frames_} {
        window_info.type = Core::Frontend::WindowSystemType::Headless;
    }
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<Core::Frontend::GraphicsContext>();
    }
    bool IsShown() const override {
        return frames != nullptr;
    }
    bool WantsSoftwareFrames() const override {
        return frames != nullptr;
    }
    void OnSoftwareFrame(std::span<const u32> pixels, u32 width, u32 height) override {
        frames(Settings::values.show_fps_overlay.GetValue() ? fps.Draw(pixels, width, height) : pixels, width,
               height);
    }

private:
    FrameSink frames;
    FpsCounter fps;
};

/// The PS4's screen, for Eden's Vulkan renderer: its swapchain (vk_swapchain_ps4.cpp) opens the
/// console's video out and flips its own buffers, 1280x720.
class ConsoleWindow final : public Core::Frontend::EmuWindow {
public:
    ConsoleWindow() {
        window_info.type = Core::Frontend::WindowSystemType::Orbis;
        window_info.render_surface = nullptr;
        NotifyClientAreaSizeChanged({1280u, 720u});
        UpdateCurrentFramebufferLayout(1280, 720);
    }
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<Core::Frontend::GraphicsContext>();
    }
    bool IsShown() const override {
        return true;
    }
};

bool g_gpu_display = false;
bool g_docked = true;
int g_resolution = 100;
bool g_shader_cache = false, g_async_shaders = false, g_cpu_unsafe = false, g_gpu_high = false, g_fps_overlay = false;
bool g_unlocked_frames = false;
int g_speed_percent = 100;
std::string g_keys_dir, g_firmware_dir; // (empty: the user folder's)

// Player 1's controller: Eden's virtual gamepad (the engine Android's touch overlay drives),
// while a program runs.
std::mutex g_pad_mutex;
InputCommon::VirtualGamepad* g_gamepad = nullptr;
PadState g_pad_state;

std::mutex g_output_mutex;
LineSink g_sink = nullptr;
std::FILE* g_log = nullptr;
// log.txt stops at this size (a game that warns every frame would fill the PS4's storage over a
// long session); what's written so far
constexpr size_t kLogLimit = size_t(32) << 20;
size_t g_log_bytes = 0;

void Output(LineKind kind, std::string_view text) {
    std::scoped_lock lock{g_output_mutex};
    if (g_log && g_log_bytes < kLogLimit) {
        std::fwrite(text.data(), 1, text.size(), g_log);
        std::fputc('\n', g_log);
        g_log_bytes += text.size() + 1;
        if (g_log_bytes >= kLogLimit) {
            std::fputs("(log.txt stops here, at 32 MiB)\n", g_log);
        }
        std::fflush(g_log);
    }
    if (g_sink)
        g_sink(kind, text);
}

void OnLog(Common::Log::Class log_class, Common::Log::Level level, std::string_view message,
           std::string_view line) {
    if (log_class == Common::Log::Class::Debug_Emulated) {
        while (!message.empty() && (message.back() == '\n' || message.back() == '\0'))
            message.remove_suffix(1);
        Output(LineKind::Program, message);
    } else if (level >= Common::Log::Level::Error) {
        Output(LineKind::Error, line);
    } else if (level == Common::Log::Level::Warning) {
        Output(LineKind::Warning, line);
    } else {
        // what a log filter lets through below Warning (the GPU display's own, or EDEN_PS4_LOG)
        Output(LineKind::Info, line);
    }
}

void ApplyLauncherSettings() {
    // Eden's log filter (e.g. "*:Info Service.VI:Debug"); by default warnings and errors only
    // (with the GPU display, the Vulkan renderer's and GPU memory's progress too: what the console
    // shows of them is only in log.txt)
    const char* filter = std::getenv("EDEN_PS4_LOG");
    Settings::values.log_filter = filter         ? filter
                                  : g_gpu_display ? "*:Warning Debug.Emulated:Info Render.Vulkan:Info HW.GPU:Info HW.Memory:Info"
                                                  : "*:Warning Debug.Emulated:Info";
    // The null renderer composes frames on the CPU and drops GPU work; EDEN_PS4_RENDERER=vulkan
    // runs Eden's Vulkan renderer without a window (its frames read back), for GPU-drawn programs.
    Settings::values.use_docked_mode.SetValue(g_docked ? Settings::ConsoleMode::Docked
                                                       : Settings::ConsoleMode::Handheld);
    Settings::values.resolution_setup.SetValue(g_resolution <= 50   ? Settings::ResolutionSetup::Res1_2X
                                               : g_resolution <= 75 ? Settings::ResolutionSetup::Res3_4X
                                                                    : Settings::ResolutionSetup::Res1X);
    const char* renderer = std::getenv("EDEN_PS4_RENDERER");
    const bool vulkan = g_gpu_display || (renderer && std::string_view{renderer} == "vulkan");
    Settings::values.renderer_backend.SetValue(vulkan ? Settings::RendererBackend::Vulkan
                                                      : Settings::RendererBackend::Null);
    Settings::values.sink_id.SetValue(AudioCore::Sink::HasFrontendAudio() ? Settings::AudioEngine::Frontend
                                                                           : Settings::AudioEngine::Null);
    Settings::values.use_disk_shader_cache.SetValue(g_shader_cache);
    Settings::values.use_asynchronous_shaders.SetValue(g_async_shaders);
    Settings::values.cpu_accuracy.SetValue(g_cpu_unsafe ? Settings::CpuAccuracy::Unsafe : Settings::CpuAccuracy::Auto);
    // EDEN_PS4_FASTMEM=0: no fastmem on the desktop either, so that guest memory goes through the
    // recompiler's page table walk, as on the PS4 (which can't map memory twice): Debugging
    // accuracy, with the optimisations Auto has otherwise
#ifdef __OPENORBIS__
    // Fastmem (common/host_memory.cpp: an arena when the Fastmem setting is on) with Unsafe
    // accuracy too: Eden drops it there unless "host MMU" is on, which defaults to off on platforms
    // it doesn't list (03.85's arena went unused with CPU Unsafe)
    Settings::values.cpuopt_unsafe_host_mmu.SetValue(true);
#else
    // (on the PS4 the variable is the Fastmem setting itself, read by common/host_memory.cpp)
    if (const char* fastmem = std::getenv("EDEN_PS4_FASTMEM"); fastmem && std::string_view{fastmem} == "0") {
        if (g_cpu_unsafe) {
            Settings::values.cpuopt_unsafe_host_mmu.SetValue(false);
        } else {
            Settings::values.cpu_accuracy.SetValue(Settings::CpuAccuracy::Debugging);
            Settings::values.cpuopt_fastmem.SetValue(false);
            Settings::values.cpuopt_fastmem_exclusives.SetValue(false);
        }
    }
#endif
    // Low GPU accuracy and no reactive flushing unless asked: High reads GPU-written memory back for
    // the CPU and marks it so that every guest access to it takes the slow path (no fastmem here)
    Settings::values.gpu_accuracy.SetValue(g_gpu_high ? Settings::GpuAccuracy::High : Settings::GpuAccuracy::Low);
    Settings::values.use_reactive_flushing.SetValue(g_gpu_high);
    Settings::values.show_fps_overlay.SetValue(g_fps_overlay);
    // Frame pacing: Eden's composer shows a frame at the next 60 Hz vsync (the speed limit at 100%:
    // a game that misses one waits for the next, 30, 20, 15 fps); unlocked, its vsyncs come 100 times
    // as often, so each frame is shown as soon as it's done (and a game that could go above 60 does)
    Settings::values.use_speed_limit.SetValue(!g_unlocked_frames);
    // Game speed: the speed limit scales how often Eden's vsyncs come (vi/conductor.cpp), so a game
    // with room to spare runs that much faster. A composite waits for the one before it to be shown,
    // and the PS4's display flips at the TV's 60 Hz vsync: above 100% they flip at the next
    // scanline instead (Immediate, vk_swapchain_ps4.cpp), or the TV's 60 Hz would cap it again.
    Settings::values.speed_limit.SetValue(static_cast<u16>(g_speed_percent));
    Settings::values.vsync_mode.SetValue(g_speed_percent > 100 ? Settings::VSyncMode::Immediate : Settings::VSyncMode::Fifo);
    // GPU macros (MME: games' draws and state changes go through them, on the GPU thread) compiled
    // to x86 code (on the PS4 into memory mapped executable from the start, patch 0051), not
    // interpreted; EDEN_PS4_MACRO_JIT=0 interprets them
    const char* macro_jit = std::getenv("EDEN_PS4_MACRO_JIT");
    Settings::values.disable_macro_jit.SetValue(macro_jit && std::string_view{macro_jit} == "0");
    // No Cemuhook (UDP) controller servers: their client would open a host socket at start,
    // for controllers this launcher doesn't use.
    Settings::values.udp_input_servers.SetValue("");
    // No ring controller (it maps to the Joy-Con driver, which isn't in this build).
    Settings::values.ringcon_analogs = "";

    // Player 1: a Pro Controller on the virtual gamepad (SetPad); no handheld console.
    auto& players = Settings::values.players.GetValue();
    for (auto& player : players) {
        player.connected = false;
    }
    auto& player = players[0];
    player.connected = true;
    player.controller_type = Settings::ControllerType::ProController;
    for (int i = 0; i < Settings::NativeButton::NumButtons; i++) {
        player.buttons[i] = i < 20 ? fmt::format("engine:virtual_gamepad,port:0,button:{}", i) : "";
    }
    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; i++) {
        player.analogs[i] = fmt::format(
            "engine:virtual_gamepad,port:0,axis_x:{},axis_y:{},deadzone:0.0,range:1.0", i * 2, i * 2 + 1);
    }
    for (auto& motion : player.motions) {
        motion = "";
    }
}

// Why a game didn't load, in words that say what to do about it: the keys and the firmware
// first (a commercial game's first boot fails there most), else the loader's own message
std::string LoadError(Core::SystemResultStatus status) {
    switch (status) {
    case Core::SystemResultStatus::ErrorGetLoader:
        return "no loader for this file";
    case Core::SystemResultStatus::ErrorNotInitialized:
        return "the CPU cores are not initialized";
    case Core::SystemResultStatus::ErrorVideoCore:
        return "the video core did not start";
    default:
        break;
    }
    if (u32(status) < u32(Core::SystemResultStatus::ErrorLoader))
        return "the system failed to load it";
    using Loader::ResultStatus;
    const auto loader = ResultStatus(u32(status) - u32(Core::SystemResultStatus::ErrorLoader));
    const char* advice = nullptr;
    switch (loader) {
    case ResultStatus::ErrorMissingProductionKeyFile:
        advice = "prod.keys is missing: put it on a USB drive (at its top, or in a folder there such as switch), "
                 "or send it from the web page (the address at the top of the HOME menu)";
        break;
    case ResultStatus::ErrorMissingHeaderKey:
    case ResultStatus::ErrorIncorrectHeaderKey:
        advice = "prod.keys has no right header_key: dump the keys again from your Switch";
        break;
    case ResultStatus::ErrorMissingTitlekey:
        advice = "this game's title key isn't in title.keys: a downloaded game needs title.keys dumped from "
                 "the Switch it's installed on";
        break;
    case ResultStatus::ErrorMissingTitlekek:
    case ResultStatus::ErrorMissingKeyAreaKey:
        advice = "your prod.keys are older than this game: dump them again from a Switch updated to at least "
                 "the firmware the game needs, and send that firmware too";
        break;
    case ResultStatus::ErrorIncorrectKeyAreaKey:
    case ResultStatus::ErrorIncorrectTitlekeyOrTitlekek:
        advice = "the keys don't decrypt this game: prod.keys and title.keys must come from the Switch it was "
                 "dumped from (or the file is damaged)";
        break;
    case ResultStatus::ErrorNSPMissingProgramNCA:
    case ResultStatus::ErrorXCIMissingProgramNCA:
        advice = "no program found in it: usually the keys can't decrypt it (prod.keys missing, or older than "
                 "the game), else it's an update or DLC, which goes with the base game";
        break;
    case ResultStatus::ErrorNoExeFS:
    case ResultStatus::ErrorMissingNPDM:
        advice = "this file has no program: it may be an update or DLC, which goes with the base game";
        break;
    case ResultStatus::ErrorIntegrityVerificationFailed:
    case ResultStatus::ErrorBadNCAHeader:
    case ResultStatus::ErrorBadPFSHeader:
    case ResultStatus::ErrorBadXCIHeader:
        advice = "the file looks damaged: copy or dump it again";
        break;
    default:
        break;
    }
    const std::string message = Loader::GetResultStatusString(loader);
    return advice ? fmt::format("{} ({})", advice, message) : message;
}

} // namespace

void SetDocked(bool docked) {
    g_docked = docked;
}

void SetResolution(int percent) {
    g_resolution = percent;
}

void SetShaderCache(bool on) {
    g_shader_cache = on;
}

void SetAsyncShaders(bool on) {
    g_async_shaders = on;
}

void SetCpuUnsafe(bool on) {
    g_cpu_unsafe = on;
}

void SetGpuAccuracyHigh(bool on) {
    g_gpu_high = on;
}

void SetFpsOverlay(bool on) {
    g_fps_overlay = on;
}

void SetUnlockedFrames(bool on) {
    g_unlocked_frames = on;
}

void SetSpeedPercent(int percent) {
    g_speed_percent = percent == 150 || percent == 200 ? percent : 100;
}

void SetKeysAndFirmware(std::string keys_dir, std::string firmware_dir) {
    g_keys_dir = std::move(keys_dir);
    g_firmware_dir = std::move(firmware_dir);
}

std::filesystem::path TitleCachePath(const std::filesystem::path& data_dir, std::string_view program,
                                     std::string_view extension) {
    uint64_t hash = 1469598103934665603ull; // FNV-1a
    for (const char c : program)
        hash = (hash ^ uint8_t(c)) * 1099511628211ull;
    return data_dir / "cache" / "titles" / fmt::format("{:016x}.{}", hash, extension);
}

void SetGpuDisplay(bool on) {
    g_gpu_display = on;
}

void SetPad(const PadState& state) {
    std::scoped_lock lock{g_pad_mutex};
    if (!g_gamepad) {
        return;
    }
    const u32 changed = state.buttons ^ g_pad_state.buttons;
    for (int i = 0; i < 20; i++) {
        if (changed & (1u << i)) {
            g_gamepad->SetButtonState(0, i, (state.buttons >> i) & 1);
        }
    }
    using Stick = InputCommon::VirtualGamepad::VirtualStick;
    if (state.lx != g_pad_state.lx || state.ly != g_pad_state.ly) {
        g_gamepad->SetStickPosition(0, Stick::Left, state.lx, state.ly);
    }
    if (state.rx != g_pad_state.rx || state.ry != g_pad_state.ry) {
        g_gamepad->SetStickPosition(0, Stick::Right, state.rx, state.ry);
    }
    g_pad_state = state;
}

namespace {
class AudioAdapter final : public AudioCore::Sink::FrontendAudioOutput {
public:
    explicit AudioAdapter(std::unique_ptr<AudioOutput> output_) : output{std::move(output_)} {}
    bool Open(u32 frames, u32 channels) override {
        return output->Open(frames, channels);
    }
    void Output(std::span<const s16> samples) override {
        output->Output(samples);
    }

private:
    std::unique_ptr<AudioOutput> output;
};
} // namespace

void SetAudio(AudioFactory factory) {
    if (!factory) {
        AudioCore::Sink::SetFrontendAudio(nullptr);
        return;
    }
    AudioCore::Sink::SetFrontendAudio([factory]() -> std::unique_ptr<AudioCore::Sink::FrontendAudioOutput> {
        std::unique_ptr<AudioOutput> output = factory();
        if (!output) {
            return nullptr;
        }
        return std::make_unique<AudioAdapter>(std::move(output));
    });
}

// The running program's end: it exited, or Stop was called.
std::mutex g_run_mutex;
std::condition_variable g_run_cv;
bool g_running = false;
bool g_stop = false;

void Stop() {
    std::scoped_lock lock{g_run_mutex};
    if (g_running) {
        g_stop = true;
        g_run_cv.notify_all();
    }
}

// What a program that loaded is called, who made it, and its icon, for the HOME menu: a game's
// can't be read without the keys that loading it just used.
void CacheTitle(Core::System& system, const std::filesystem::path& data_dir, const std::string& program) {
    Loader::AppLoader& loader = system.GetAppLoader();
    std::string title, publisher;
    void(loader.ReadTitle(title));
    FileSys::NACP control;
    if (loader.ReadControlData(control) == Loader::ResultStatus::Success)
        publisher = control.GetDeveloperName();
    std::vector<u8> icon;
    void(loader.ReadIcon(icon));
    std::error_code ec;
    std::filesystem::create_directories(TitleCachePath(data_dir, program, "txt").parent_path(), ec);
    if (std::FILE* f = std::fopen(TitleCachePath(data_dir, program, "txt").c_str(), "w")) {
        std::fprintf(f, "%s\n%s\n", title.c_str(), publisher.c_str());
        std::fclose(f);
    }
    if (!icon.empty()) {
        if (std::FILE* f = std::fopen(TitleCachePath(data_dir, program, "jpg").c_str(), "wb")) {
            std::fwrite(icon.data(), 1, icon.size(), f);
            std::fclose(f);
        }
    }
}

BootResult Boot(const std::filesystem::path& program, const std::filesystem::path& data_dir,
                LineSink sink, FrameSink frames, std::chrono::milliseconds timeout) {
    // Eden's data folders (NAND, SD card, keys...) in data_dir/user.
    std::error_code ec;
    std::filesystem::create_directories(data_dir / "user", ec);
    if (ec)
        return {BootResult::Status::LoadFailed, fmt::format("can't use {}: {}", data_dir.string(), ec.message())};
    Common::FS::SetAppDirectory((data_dir / "user").string());
    Common::FS::CreateEdenPaths();

    g_sink = sink;
    g_log = std::fopen((data_dir / "log.txt").c_str(), "w");
    g_log_bytes = 0;

    ApplyLauncherSettings();
    // Updates and DLC (.nsp) in data_dir/updates, applied to their game as it boots, without
    // installing them (Eden's external content)
    std::filesystem::create_directories(data_dir / "updates", ec);
    Settings::values.external_content_dirs = {(data_dir / "updates").string()};
    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(false);
    Common::Log::SetCallback(OnLog);
    Common::Log::Start();
    // The keys where SetKeysAndFirmware said (a USB drive), else the user folder's, read again at
    // each boot (once the log has started: it writes to it): keys added or a drive plugged in
    // since the last boot count
    if (!g_keys_dir.empty())
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, g_keys_dir);
    Core::Crypto::KeyManager::Instance().ReloadKeys();
    Settings::values.system_contents_dir = g_firmware_dir;
    // ENet and the multiplayer room, which the BSD sockets service expects
    Network::Init();

    BootResult result{BootResult::Status::TimedOut, {}};
    {
        // The input subsystem outlives the system: the emulated controllers use its devices
        // until the system is gone.
        InputCommon::InputSubsystem input;
        input.Initialize();
        {
            std::scoped_lock lock{g_pad_mutex};
            g_gamepad = input.GetVirtualGamepad();
            g_pad_state = {};
        }
        {
            Core::System system;
            system.Initialize();
            system.ApplySettings();
            HeadlessWindow headless{frames};
            ConsoleWindow console;
            Core::Frontend::EmuWindow& window = g_gpu_display ? static_cast<Core::Frontend::EmuWindow&>(console)
                                                              : headless;

            system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
            system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
            system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
            system.GetUserChannel().clear();

            bool exited = false;
            system.RegisterExitCallback([&] {
                std::scoped_lock lock{g_run_mutex};
                exited = true;
                g_run_cv.notify_all();
            });

            Output(LineKind::Status, fmt::format("booting {}", program.filename().string()));
            Service::AM::FrontendAppletParameters load_parameters{
                .applet_id = Service::AM::AppletId::Application,
            };
            const auto status = system.Load(window, program.string(), load_parameters);
            if (status != Core::SystemResultStatus::Success) {
                result = {BootResult::Status::LoadFailed, LoadError(status)};
            } else {
                CacheTitle(system, data_dir, program.string());
                system.GPU().Start();
                system.GetCpuManager().OnGpuReady();
                // the shaders this game used before, built now (as yuzu-cmd does): a longer start,
                // no stutter for them later (title id 0, as homebrew without one: no cache)
                if (Settings::values.use_disk_shader_cache.GetValue() &&
                    Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan) {
                    u64 title_id = 0;
                    if (system.GetAppLoader().ReadProgramId(title_id) != Loader::ResultStatus::Success || !title_id)
                        title_id = system.GetApplicationProcessProgramID();
                    Output(LineKind::Status, fmt::format("loading the shader cache of {:016x}", title_id));
                    system.Renderer().ReadRasterizer()->LoadDiskResources(
                        title_id, std::stop_token{}, [](VideoCore::LoadCallbackStage, size_t, size_t) {});
                }
                std::unique_lock lock{g_run_mutex};
                g_running = true;
                g_stop = false;
                lock.unlock();
                void(system.Run());
                lock.lock();
                // every 10 s while it runs, its speed as Eden measures it (the PS4's boot trace, log.txt)
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (!exited && !g_stop) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline)
                        break;
                    const auto slice = std::min<std::chrono::steady_clock::duration>(std::chrono::seconds(10),
                                                                                     deadline - now);
                    if (g_run_cv.wait_for(lock, slice, [&] { return exited || g_stop; }))
                        break;
                    lock.unlock();
                    const Core::PerfStatsResults stats = system.GetAndResetPerfStats();
                    std::string extra;
#ifdef __OPENORBIS__
                    {
                        // (04.09) what the GPU thread met new in these 10 s (a race's start)
                        namespace P = Common::PS4Perf;
                        static uint64_t last[5] = {};
                        const uint64_t now[5] = {P::gpu_pipelines_new.load(), P::gpu_draws_skipped.load(),
                                                 P::gpu_images_created.load(), P::gpu_buffers_created.load(),
                                                 P::gpu_thread.draws.load()};
                        extra = fmt::format("; {} draws, new: {} pipelines ({} draws skipped), {} images, {} buffers",
                                            now[4] - last[4], now[0] - last[0], now[1] - last[1], now[2] - last[2],
                                            now[3] - last[3]);
                        std::copy(std::begin(now), std::end(now), std::begin(last));
                        // (04.12) uniform buffers copied to stream buffers, and the constant buffer
                        // data written
                        {
                            static uint64_t last_ubo[2] = {};
                            const uint64_t ubo[2] = {P::gpu_thread.ubo_copied_bytes.load(), P::gpu_thread.cb_bytes.load()};
                            extra += fmt::format("; uniform buffers copied {:.1f} MiB, constant data written {:.1f} MiB",
                                                 double(ubo[0] - last_ubo[0]) / 1048576.0,
                                                 double(ubo[1] - last_ubo[1]) / 1048576.0);
                            std::copy(std::begin(ubo), std::end(ubo), std::begin(last_ubo));
                        }
                        // (04.11) core 0's (the game's main thread's) two longest waits on objects
                        const P::WaitTable& table = P::object_waits[0];
                        static uint64_t last_count[P::WaitTable::Max] = {}, last_ns[P::WaitTable::Max] = {};
                        const size_t used = std::min(table.used.load(std::memory_order_acquire), P::WaitTable::Max);
                        size_t best[2] = {SIZE_MAX, SIZE_MAX};
                        uint64_t best_ns[2] = {0, 0}, counts[P::WaitTable::Max] = {}, nss[P::WaitTable::Max] = {};
                        for (size_t i = 0; i < used; i++) {
                            const uint64_t c = table.entries[i].count.load(std::memory_order_relaxed);
                            const uint64_t n = table.entries[i].total_ns.load(std::memory_order_relaxed);
                            counts[i] = c - last_count[i];
                            nss[i] = n - last_ns[i];
                            last_count[i] = c;
                            last_ns[i] = n;
                            if (nss[i] > best_ns[0]) {
                                best[1] = best[0], best_ns[1] = best_ns[0];
                                best[0] = i, best_ns[0] = nss[i];
                            } else if (nss[i] > best_ns[1]) {
                                best[1] = i, best_ns[1] = nss[i];
                            }
                        }
                        for (size_t k = 0; k < 2; k++)
                            if (best[k] != SIZE_MAX && counts[best[k]] != 0)
                                extra += fmt::format("{} {} x{} {:.0f} ms", k ? "," : "; core 0 waited on", table.entries[best[k]].label,
                                                     counts[best[k]], double(nss[best[k]]) / 1e6);
                    }
#endif
                    Output(LineKind::Status,
                           fmt::format("speed: the game {:.1f} fps, emulation at {:.0f}% of full speed, {:.1f} ms "
                                       "of work a frame{}",
                                       stats.average_game_fps, stats.emulation_speed * 100.0,
                                       stats.frametime * 1000.0, extra));
                    lock.lock();
                }
                if (exited)
                    result = {BootResult::Status::Exited, {}};
                else if (g_stop)
                    result = {BootResult::Status::Stopped, {}};
                g_running = false;
                lock.unlock();
#ifdef __OPENORBIS__
                // (04.09: a shutdown stuck for 30 s restarts the app, its step in the hang report)
                Common::PS4Diagnostics::shutdown_stage = "pausing the program";
                Common::PS4Diagnostics::shutting_down = true;
#endif
                void(system.Pause());
                if (!exited) {
                    // Where the program's threads were: addresses relative to its start (the
                    // NRO's ELF addresses), to symbolize
                    if (auto* process = system.ApplicationProcess()) {
                        const u64 base = GetInteger(process->GetEntryPoint());
                        for (auto& thread : process->GetThreadList()) {
                            const auto& ctx = thread.GetContext();
                            Output(LineKind::Status,
                                   fmt::format("thread {}: pc {:#x} lr {:#x} sp {:#x} (image at {:#x})",
                                               thread.GetThreadId(), ctx.pc - base, ctx.lr - base,
                                               ctx.sp, base));
                        }
                    }
                }
                {
                    // no input into a system that is shutting down (it reached objects on its
                    // services' stacks through the controllers' callbacks)
                    std::scoped_lock pad_lock{g_pad_mutex};
                    g_gamepad = nullptr;
                }
                system.ShutdownMainProcess();
#ifdef __OPENORBIS__
                Common::PS4Diagnostics::shutting_down = false;
#endif
            }
        }
        {
            std::scoped_lock lock{g_pad_mutex};
            g_gamepad = nullptr;
        }
        input.Shutdown();
    }
    Network::Shutdown();

    switch (result.status) {
    case BootResult::Status::Exited:
        Output(LineKind::Status, "the program exited");
        break;
    case BootResult::Status::LoadFailed:
        Output(LineKind::Error, fmt::format("could not load {}: {}", program.string(), result.detail));
        break;
    case BootResult::Status::TimedOut:
        Output(LineKind::Error, fmt::format("the program was still running after {} s",
                                            timeout.count() / 1000));
        break;
    case BootResult::Status::Stopped:
        Output(LineKind::Status, "the program was stopped");
        break;
    }

    Common::Log::SetCallback(nullptr);
    Common::Log::Stop();
    std::scoped_lock lock{g_output_mutex};
    if (g_log)
        std::fclose(g_log);
    g_log = nullptr;
    g_sink = nullptr;
    return result;
}

} // namespace Launcher

// Each frontend compiles the Vulkan memory allocator once (video_core links it).
#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"

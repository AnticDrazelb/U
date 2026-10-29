// The Eden PS4 launcher's core: boots a Switch program in Eden's core (kernel, scheduler, CPU
// cores, HLE services, loader) with the null renderer, whose frames (composed on the CPU) go to
// the platform; or, on the PS4 with Mesa's RADV linked, Eden's Vulkan renderer drawing straight to
// the screen. Sound goes to the platform's output through Eden's frontend sink.
// What the program prints, and Eden's warnings and errors, go to the platform's sink and to
// log.txt.

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <system_error>

#include <fmt/format.h>

#include "audio_core/sink/frontend_sink.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "launcher.h"
#include "network/network.h"
#include "video_core/gpu.h"

namespace Launcher {
namespace {

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
        frames(pixels, width, height);
    }

private:
    FrameSink frames;
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

// Player 1's controller: Eden's virtual gamepad (the engine Android's touch overlay drives),
// while a program runs.
std::mutex g_pad_mutex;
InputCommon::VirtualGamepad* g_gamepad = nullptr;
PadState g_pad_state;

std::mutex g_output_mutex;
LineSink g_sink = nullptr;
std::FILE* g_log = nullptr;

void Output(LineKind kind, std::string_view text) {
    std::scoped_lock lock{g_output_mutex};
    if (g_log) {
        std::fwrite(text.data(), 1, text.size(), g_log);
        std::fputc('\n', g_log);
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
    const char* renderer = std::getenv("EDEN_PS4_RENDERER");
    const bool vulkan = g_gpu_display || (renderer && std::string_view{renderer} == "vulkan");
    Settings::values.renderer_backend.SetValue(vulkan ? Settings::RendererBackend::Vulkan
                                                      : Settings::RendererBackend::Null);
    Settings::values.sink_id.SetValue(AudioCore::Sink::HasFrontendAudio() ? Settings::AudioEngine::Frontend
                                                                           : Settings::AudioEngine::Null);
    Settings::values.use_disk_shader_cache.SetValue(false);
    // GPU macros (MME: deko3d's and Mesa's draws use them) interpreted: the macro JIT writes x86 code
    // into memory it makes executable itself, which a PS4 app can't (EDEN_PS4_MACRO_JIT=1 for it)
    const char* macro_jit = std::getenv("EDEN_PS4_MACRO_JIT");
    Settings::values.disable_macro_jit.SetValue(!(macro_jit && std::string_view{macro_jit} == "1"));
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

const char* LoadError(Core::SystemResultStatus status) {
    switch (status) {
    case Core::SystemResultStatus::ErrorGetLoader:
        return "no loader for this file";
    case Core::SystemResultStatus::ErrorNotInitialized:
        return "the CPU cores are not initialized";
    case Core::SystemResultStatus::ErrorVideoCore:
        return "the video core did not start";
    default:
        return "the loader failed";
    }
}

} // namespace

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

    ApplyLauncherSettings();
    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(false);
    Common::Log::SetCallback(OnLog);
    Common::Log::Start();
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
                system.GPU().Start();
                system.GetCpuManager().OnGpuReady();
                std::unique_lock lock{g_run_mutex};
                g_running = true;
                g_stop = false;
                lock.unlock();
                void(system.Run());
                lock.lock();
                g_run_cv.wait_for(lock, timeout, [&] { return exited || g_stop; });
                if (exited)
                    result = {BootResult::Status::Exited, {}};
                else if (g_stop)
                    result = {BootResult::Status::Stopped, {}};
                g_running = false;
                lock.unlock();
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
                system.ShutdownMainProcess();
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

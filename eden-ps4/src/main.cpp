// The Eden PS4 launcher's core: boots a Switch program in Eden's core (kernel, scheduler, CPU
// cores, HLE services, loader) with no GPU or audio: the null renderer and the null audio sink.
// What the program prints, and Eden's warnings and errors, go to the platform's sink and to
// log.txt.

#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <system_error>

#include <fmt/format.h>
#include <unistd.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "input_common/main.h"
#include "launcher.h"
#include "video_core/gpu.h"

namespace Launcher {
namespace {

/// A window that is never drawn to: the null renderer only needs the interface.
class HeadlessWindow final : public Core::Frontend::EmuWindow {
public:
    HeadlessWindow() {
        window_info.type = Core::Frontend::WindowSystemType::Headless;
    }
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<Core::Frontend::GraphicsContext>();
    }
    bool IsShown() const override {
        return false;
    }
};

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
    }
}

void ApplyLauncherSettings() {
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Null);
    Settings::values.sink_id.SetValue(Settings::AudioEngine::Null);
    Settings::values.use_disk_shader_cache.SetValue(false);
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

BootResult Boot(const std::filesystem::path& program, const std::filesystem::path& data_dir,
                LineSink sink, std::chrono::milliseconds timeout) {
    // Eden's portable mode: its data folders go in ./user.
    std::error_code ec;
    std::filesystem::create_directories(data_dir / "user", ec);
    if (chdir(data_dir.c_str()) != 0)
        return {BootResult::Status::LoadFailed, fmt::format("can't use {}", data_dir.string())};
    Common::FS::SetAppDirectory("");

    g_sink = sink;
    g_log = std::fopen((data_dir / "log.txt").c_str(), "w");

    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(false);
    Common::Log::SetCallback(OnLog);
    Common::Log::Start();
    ApplyLauncherSettings();

    BootResult result{BootResult::Status::TimedOut, {}};
    {
        // The input subsystem outlives the system: the emulated controllers use its devices
        // until the system is gone.
        InputCommon::InputSubsystem input;
        input.Initialize();
        {
            Core::System system;
            system.Initialize();
            system.ApplySettings();
            HeadlessWindow window;

            system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
            system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
            system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
            system.GetUserChannel().clear();

            std::mutex exit_mutex;
            std::condition_variable exit_cv;
            bool exited = false;
            system.RegisterExitCallback([&] {
                std::scoped_lock lock{exit_mutex};
                exited = true;
                exit_cv.notify_all();
            });

            Output(LineKind::Status, fmt::format("booting {}", program.filename().string()));
            const auto status = system.Load(window, program.string(),
                                            {.applet_id = Service::AM::AppletId::Application});
            if (status != Core::SystemResultStatus::Success) {
                result = {BootResult::Status::LoadFailed, LoadError(status)};
            } else {
                system.GPU().Start();
                system.GetCpuManager().OnGpuReady();
                void(system.Run());
                std::unique_lock lock{exit_mutex};
                if (exit_cv.wait_for(lock, timeout, [&] { return exited; }))
                    result = {BootResult::Status::Exited, {}};
                lock.unlock();
                void(system.Pause());
                system.ShutdownMainProcess();
            }
        }
        input.Shutdown();
    }

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

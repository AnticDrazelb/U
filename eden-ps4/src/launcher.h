#pragma once
// The launcher's core (main.cpp): boots a Switch program in Eden, headless. The platform files
// (platform_host.cpp, platform_ps4.cpp) pick the program and show what it prints.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace Launcher {

enum class LineKind {
    Program, // the program's own output (svcOutputDebugString)
    Warning, // Eden's warnings
    Error,   // Eden's errors
    Status,  // the launcher's own lines
    Info,    // Eden's other messages, when a log filter lets them through (EDEN_PS4_LOG)
};

/// Shows one line of output (no newline). Called from Eden's threads.
using LineSink = void (*)(LineKind kind, std::string_view text);

/// Shows a frame the program drew: `width` x `height` pixels, each R, G, B, A bytes (opaque).
/// Called from Eden's GPU thread, once per frame the program presents.
using FrameSink = void (*)(std::span<const uint32_t> pixels, uint32_t width, uint32_t height);

/// Player 1's controller, a Pro Controller. Buttons in Eden's order (Settings::NativeButton).
enum Button : uint32_t {
    ButtonA = 1u << 0,
    ButtonB = 1u << 1,
    ButtonX = 1u << 2,
    ButtonY = 1u << 3,
    ButtonLStick = 1u << 4,
    ButtonRStick = 1u << 5,
    ButtonL = 1u << 6,
    ButtonR = 1u << 7,
    ButtonZL = 1u << 8,
    ButtonZR = 1u << 9,
    ButtonPlus = 1u << 10,
    ButtonMinus = 1u << 11,
    ButtonLeft = 1u << 12,
    ButtonUp = 1u << 13,
    ButtonRight = 1u << 14,
    ButtonDown = 1u << 15,
    ButtonHome = 1u << 18,
    ButtonCapture = 1u << 19,
};
struct PadState {
    uint32_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0; // -1..1, right and up positive
};
/// Sets player 1's controller state. Any thread; ignored while no program runs.
void SetPad(const PadState& state);

/// The PS4's own display for programs: Eden's Vulkan renderer draws on the console's GPU (Mesa's
/// RADV) straight to the screen, which the platform gives up while a program runs, and nothing
/// goes to the FrameSink. Off (the default): the null renderer composes frames for the FrameSink.
/// Set it before Boot.
void SetGpuDisplay(bool on);

/// The Switch's console mode: docked (games render for a TV, up to 1920x1080) or handheld (up to
/// 1280x720). Docked unless set. Set it before Boot.
void SetDocked(bool docked);

/// What games render at, in percent of the console mode's resolution: 100 (the default), 75 or
/// 50 (Eden's resolution scaler, then scaled up for the screen). Set it before Boot.
void SetResolution(int percent);

/// Eden's disk shader cache (shaders kept in data_dir/user/shader and built while a game loads the
/// next time), its asynchronous shaders (built in the background, a draw waiting for none), and
/// its Unsafe CPU accuracy (instead of Auto). All off unless set. Set them before Boot.
void SetShaderCache(bool on);
void SetAsyncShaders(bool on);
void SetCpuUnsafe(bool on);
/// Eden's High GPU accuracy, with reactive flushing (GPU-written memory read back when the CPU
/// touches it); off: Low, without, as Eden's phone builds default to. Set it before Boot.
void SetGpuAccuracyHigh(bool on);

/// Where Boot keeps what it learned of a program that loaded (for a menu to show next time):
/// data_dir/cache/titles/<hash of its path>.<extension>, "jpg" its icon, "txt" its title and
/// publisher (a line each).
std::filesystem::path TitleCachePath(const std::filesystem::path& data_dir, std::string_view program,
                                     std::string_view extension);

/// A sound output: 48 kHz, signed 16-bit samples, channels interleaved.
class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    /// Opens the output for buffers of `frames` frames of `channels` channels. False if it can't.
    virtual bool Open(uint32_t frames, uint32_t channels) = 0;
    /// Plays one buffer, blocking until the output takes it. Always called from the same thread.
    virtual void Output(std::span<const int16_t> samples) = 0;
};
using AudioFactory = std::unique_ptr<AudioOutput> (*)();
/// Where programs' sound goes: one output for each stream Eden opens (null: no sound). Set it
/// before Boot.
void SetAudio(AudioFactory factory);

struct BootResult {
    enum class Status {
        Exited,     // the program ended itself (svcExitProcess)
        LoadFailed, // Eden could not load it
        TimedOut,   // still running after the timeout
        Stopped,    // stopped with Stop()
    } status;
    std::string detail;
};

/// Stops the program running in Boot, which returns Stopped. Any thread; ignored while none runs.
void Stop();

/// Boots `program` and runs it until it exits, Stop is called or `timeout` passes. Eden's data folders (NAND, SD
/// card, keys) are in `data_dir`/user, and everything the sink is given also goes to
/// `data_dir`/log.txt.
/// `frames` may be null: then nothing is composed (the null renderer's frames are dropped).
BootResult Boot(const std::filesystem::path& program, const std::filesystem::path& data_dir,
                LineSink sink, FrameSink frames, std::chrono::milliseconds timeout);

} // namespace Launcher

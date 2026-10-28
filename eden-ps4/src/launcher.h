#pragma once
// The launcher's core (main.cpp): boots a Switch program in Eden, headless. The platform files
// (platform_host.cpp, platform_ps4.cpp) pick the program and show what it prints.

#include <chrono>
#include <cstdint>
#include <filesystem>
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

struct BootResult {
    enum class Status {
        Exited,     // the program ended itself (svcExitProcess)
        LoadFailed, // Eden could not load it
        TimedOut,   // still running after the timeout
    } status;
    std::string detail;
};

/// Boots `program` and runs it until it exits or `timeout` passes. Eden's data folders (NAND, SD
/// card, keys) are in `data_dir`/user, and everything the sink is given also goes to
/// `data_dir`/log.txt.
/// `frames` may be null: then nothing is composed (the null renderer's frames are dropped).
BootResult Boot(const std::filesystem::path& program, const std::filesystem::path& data_dir,
                LineSink sink, FrameSink frames, std::chrono::milliseconds timeout);

} // namespace Launcher

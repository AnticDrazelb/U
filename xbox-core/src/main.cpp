// xbox-core: runs an original Xbox program (XBE) on this machine's CPU.
//
//   xbox-core <default.xbe | game.iso> [--seconds S] [--png out.png] [--wav out.wav] [--pad SPEC | --pad button@seconds,...] [-v 0|1|2]
//
// --seconds stops it after S seconds (Xbox programs rarely end on their own); --png saves the
// picture on the screen at the end; --wav records the sound. --pad holds controller input from the
// first second on: a comma list of buttons (up down left right start back ls rs a b x y black white
// lt rt) and axes (lx=N ly=N rx=N ry=N, -32768..32767; an analog button takes =0..255).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "core.h"
#include "diagnostics.h"
#include "kernel.h"

namespace {
using namespace xb;

// --pad's SPEC as a controller state
bool ParsePad(const std::string& spec, xb::PadState& pad)
{
    static const char* digital[] = {"up", "down", "left", "right", "start", "back", "ls", "rs"};
    static const char* analog[] = {"a", "b", "x", "y", "black", "white", "lt", "rt"};
    size_t at = 0;
    while (at < spec.size())
    {
        size_t end = spec.find(',', at);
        if (end == std::string::npos)
            end = spec.size();
        std::string item = spec.substr(at, end - at), value;
        at = end + 1;
        if (const size_t eq = item.find('='); eq != std::string::npos)
            value = item.substr(eq + 1), item = item.substr(0, eq);
        bool known = false;
        for (int i = 0; i < 8; i++)
            if (item == digital[i])
                pad.buttons |= u16(1u << i), known = true;
        for (int i = 0; i < 8; i++)
            if (item == analog[i])
                pad.analog[i] = u8(value.empty() ? 255 : std::atoi(value.c_str())), known = true;
        const int v = std::atoi(value.c_str());
        if (item == "lx") pad.lx = s16(v), known = true;
        if (item == "ly") pad.ly = s16(v), known = true;
        if (item == "rx") pad.rx = s16(v), known = true;
        if (item == "ry") pad.ry = s16(v), known = true;
        if (!known)
            return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    using namespace xb;
    std::string xbe, png, wav, pad;
    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc)
            g_time_limit = std::atof(argv[++i]);
        else if (a == "--png" && i + 1 < argc)
            png = argv[++i];
        else if (a == "--wav" && i + 1 < argc)
            wav = argv[++i];
        else if (a == "--pad" && i + 1 < argc)
            pad = argv[++i];
        else if (a == "-v" && i + 1 < argc)
            g_verbose = std::atoi(argv[++i]);
        else
            xbe = a;
    }
    if (xbe.empty())
    {
        Log("usage: xbox-core <default.xbe | game.iso> [--seconds S] [--png out.png] [--wav out.wav] [--pad SPEC | --pad button@seconds,...] [-v 0|1|2]");
        return 1;
    }
    // --wav: what the program plays, recorded (48 kHz, 16-bit stereo)
    static FILE* wav_file = nullptr;
    static u32 wav_frames = 0;
    if (!wav.empty() && (wav_file = std::fopen(wav.c_str(), "wb")))
    {
        std::fseek(wav_file, 44, SEEK_SET);
        g_audio_sink = [](const s16* samples, size_t frames) {
            std::fwrite(samples, 4, frames, wav_file);
            wav_frames += u32(frames);
        };
    }
    InitMemory();
    InitCpu();
    InitDevices();
    InitKernel();
    std::string error;
    if (!LoadGame(xbe, error))
        Fatal("%s: %s", xbe.c_str(), error.c_str());
    SetDataDirectory("xbox-data");
    diag::StartMonitor("hang.log", nullptr, nullptr);
    ResolveThunks();
    StartVblank();
    if (!pad.empty() && pad.find('@') != std::string::npos)
    {
        // a script: "a@3,down@5,a+x@7": each press (buttons joined by +) at its second, for 0.2 s
        std::vector<std::pair<double, PadState>> presses;
        size_t at = 0;
        while (at < pad.size())
        {
            size_t end = pad.find(',', at);
            if (end == std::string::npos)
                end = pad.size();
            const std::string item = pad.substr(at, end - at);
            at = end + 1;
            const size_t sign = item.find('@');
            std::string buttons = item.substr(0, sign);
            for (char& c : buttons)
                if (c == '+')
                    c = ',';
            PadState state;
            if (sign == std::string::npos || !ParsePad(buttons, state))
                Fatal("--pad %s: unknown input", item.c_str());
            presses.push_back({std::atof(item.c_str() + sign + 1), state});
        }
        std::thread([presses] {
            const auto start = std::chrono::steady_clock::now();
            for (const auto& [when, state] : presses)
            {
                std::this_thread::sleep_until(start + std::chrono::duration<double>(when));
                SetPad(state);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                SetPad(PadState{});
            }
        }).detach();
    }
    else if (!pad.empty())
    {
        PadState state;
        if (!ParsePad(pad, state))
            Fatal("--pad %s: unknown input", pad.c_str());
        std::thread([state] {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            SetPad(state);
        }).detach();
    }
    RunXbe();
    if (!png.empty())
        SaveFramebuffer(png);
    if (wav_file)
    {
        // the RIFF header, now that the length is known
        const u32 data = wav_frames * 4;
        const u32 header[11] = {0x46464952, 36 + data, 0x45564157, 0x20746d66, 16, 0x00020001, 48000, 48000 * 4,
                                0x00100004, 0x61746164, data};
        std::fseek(wav_file, 0, SEEK_SET);
        std::fwrite(header, 4, 11, wav_file);
        std::fclose(wav_file);
        Log("audio: %u frames (%.1f s) -> %s", wav_frames, wav_frames / 48000.0, wav.c_str());
    }
    std::fflush(stdout);
    _exit(0); // guest threads may still be in kernel waits
}

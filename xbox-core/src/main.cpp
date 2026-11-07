// xbox-core: runs an original Xbox program (XBE) on this machine's CPU.
//
//   xbox-core <default.xbe> [--seconds S] [--png out.png] [-v 0|1|2]
//
// --seconds stops it after S seconds (Xbox programs rarely end on their own); --png saves the
// picture on the screen at the end.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "core.h"
#include "kernel.h"

int main(int argc, char** argv)
{
    using namespace xb;
    std::string xbe, png, wav;
    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc)
            g_time_limit = std::atof(argv[++i]);
        else if (a == "--png" && i + 1 < argc)
            png = argv[++i];
        else if (a == "--wav" && i + 1 < argc)
            wav = argv[++i];
        else if (a == "-v" && i + 1 < argc)
            g_verbose = std::atoi(argv[++i]);
        else
            xbe = a;
    }
    if (xbe.empty())
    {
        Log("usage: xbox-core <default.xbe> [--seconds S] [--png out.png] [-v 0|1|2]");
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
    if (!LoadXbe(xbe, g_xbe, error))
        Fatal("%s: %s", xbe.c_str(), error.c_str());
    const size_t slash = xbe.find_last_of('/');
    SetGameDirectory(slash == std::string::npos ? "." : xbe.substr(0, slash));
    SetDataDirectory("xbox-data");
    ResolveThunks();
    StartVblank();
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

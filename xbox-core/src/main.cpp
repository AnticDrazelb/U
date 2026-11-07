// xbox-core: runs an original Xbox program (XBE) on this machine's CPU.
//
//   xbox-core <default.xbe> [--seconds S] [--png out.png] [-v 0|1|2]
//
// --seconds stops it after S seconds (Xbox programs rarely end on their own); --png saves the
// picture on the screen at the end.
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "core.h"
#include "kernel.h"

int main(int argc, char** argv)
{
    using namespace xb;
    std::string xbe, png;
    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc)
            g_time_limit = std::atof(argv[++i]);
        else if (a == "--png" && i + 1 < argc)
            png = argv[++i];
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
    std::fflush(stdout);
    _exit(0); // guest threads may still be in kernel waits
}

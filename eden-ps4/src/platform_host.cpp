// The launcher on a desktop: eden-ps4 <program.nro>. Output on stdout; Eden's data folders in
// $EDEN_PS4_DATA (default: a new temporary folder). Exit code 0 if the program exited, 1 if it
// didn't load, 2 if it was still running after $EDEN_PS4_TIMEOUT seconds (default 60).

#include <cstdio>
#include <cstdlib>
#include <string>

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
    }
    std::printf("%s%.*s\n", prefix, static_cast<int>(text.size()), text.data());
    std::fflush(stdout);
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

    const auto result = Launcher::Boot(program, data_dir, PrintLine, timeout);
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

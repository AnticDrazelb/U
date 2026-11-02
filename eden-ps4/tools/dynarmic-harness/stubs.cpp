// Eden's common/ functions dynarmic refers to, for the host harness
#include <cstdio>
#include <cstdlib>
#include "common/logging.h"
void AssertFailSoftImpl() { std::fprintf(stderr, "assert (soft) failed\n"); }
[[noreturn]] void AssertFatalImpl() { std::fprintf(stderr, "assert failed\n"); std::abort(); }
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char* filename, unsigned int line, const char*, fmt::string_view format, const fmt::format_args& args) {
    std::fprintf(stderr, "%s:%u: %s\n", filename, line, fmt::vformat(format, args).c_str());
}
}

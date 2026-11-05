// Eden's common library as its Dynarmic uses it, for a desktop test: asserts and logging print
#include <cstdio>
#include <cstdlib>
#include "common/assert.h"
#include "common/logging.h"
void AssertFailSoftImpl() { std::fprintf(stderr, "dynarmic: assertion failed (soft)\n"); }
void AssertFatalImpl() { std::fprintf(stderr, "dynarmic: assertion failed\n"); std::abort(); }
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char* file, unsigned int line, const char* function,
                       fmt::string_view format, const fmt::format_args& args) {
    std::fprintf(stderr, "%s:%u %s: %s\n", file, line, function, fmt::vformat(format, args).c_str());
}
} // namespace Common::Log

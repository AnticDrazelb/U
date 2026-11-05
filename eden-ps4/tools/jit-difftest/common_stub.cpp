// Eden's common library as its Dynarmic uses it, for a desktop test: asserts and logging print
#include <cstdio>
#include <cstdlib>
#include <csetjmp>
#include "common/assert.h"
#include "common/logging.h"
// (the test's translation of one instruction to vet it: an assertion is an instruction to skip)
// (soft ones noted; a fatal one jumps back to the vetting, the translator's frames being noexcept)
bool g_assert_throws = false, g_assert_hit = false;
std::jmp_buf* g_assert_jump = nullptr;
void AssertFailSoftImpl() {
    if (g_assert_throws) {
        g_assert_hit = true;
        return;
    }
    std::fprintf(stderr, "dynarmic: assertion failed (soft)\n");
}
void AssertFatalImpl() {
    if (g_assert_throws && g_assert_jump)
        std::longjmp(*g_assert_jump, 1);
    std::fprintf(stderr, "dynarmic: assertion failed\n");
    std::abort();
}
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char* file, unsigned int line, const char* function,
                       fmt::string_view format, const fmt::format_args& args) {
    if (!g_assert_throws)
        std::fprintf(stderr, "%s:%u %s: %s\n", file, line, function, fmt::vformat(format, args).c_str());
}
} // namespace Common::Log

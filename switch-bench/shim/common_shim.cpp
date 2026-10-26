// The few parts of Eden's src/common that dynarmic links against: the log sink behind LOG_* and
// the assert handlers. Messages go to the benchmark's own log (platform.h).
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging.h"
#include "platform.h"

namespace Common::Log {
void FmtLogMessageImpl(Class log_class, Level log_level, const char* filename, unsigned int line_num,
                       const char* function, fmt::string_view format, const fmt::format_args& args)
{
    (void)log_class;
    if (log_level < Level::Warning)
        return;
    const std::string text = fmt::vformat(format, args);
    Platform::Log(fmt::format("[dynarmic {}] {}:{} {}: {}", static_cast<int>(log_level), filename, line_num,
                              function, text));
}
} // namespace Common::Log

void AssertFailSoftImpl()
{
    Platform::Log("[dynarmic] assertion failed (continuing)");
}

void AssertFatalImpl()
{
    Platform::Log("[dynarmic] fatal assertion");
    std::abort();
}

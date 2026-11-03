#include "common/assert.h"
#include "common/logging.h"
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char*, fmt::string_view, const fmt::format_args&) {}
}
void AssertFailSoftImpl() {}

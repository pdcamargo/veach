#include <veach/core/assert.hpp>
#include <veach/core/log.hpp>

#include <cstdlib>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

namespace veach::detail {

void panic(std::string_view message, std::source_location where) {
    log::write(log::Level::error, "core", std::format("{} ({}:{})", message, where.file_name(), where.line()),
               slug(ErrorCode::internal));
    std::abort();
}

}  // namespace veach::detail

namespace veach {

void report_crashes_on_stderr() {
#if defined(_WIN32)
#if defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

}  // namespace veach

/* The Matter SDK's log lines go to the app (matter.log sets how much of them shows). */
#include <cstdarg>
#include <cstdio>

#include <lib/support/logging/Constants.h>
#include <platform/logging/LogV.h>

#include "ucxm_host.h"

namespace chip {
namespace Logging {
namespace Platform {

void LogV(const char * module, uint8_t category, const char * msg, va_list v)
{
    const ucxapp_matter_host_t * host = gUcxmHost;
    char line[256];
    int n;

    if (host == nullptr)
    {
        return;
    }
    n = snprintf(line, sizeof(line), "[%s] ", module);
    if (n > 0 && static_cast<size_t>(n) < sizeof(line))
    {
        vsnprintf(line + n, sizeof(line) - static_cast<size_t>(n), msg, v);
    }
    host->log(category == kLogCategory_Error ? UCXAPP_MATTER_LOG_ERROR
                  : category == kLogCategory_Progress ? UCXAPP_MATTER_LOG_INFO : UCXAPP_MATTER_LOG_DETAIL,
              line);
}

} // namespace Platform
} // namespace Logging
} // namespace chip

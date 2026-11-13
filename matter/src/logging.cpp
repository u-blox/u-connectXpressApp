/* The Matter SDK's log lines go to the app (matter.log sets how much of them shows). */
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <lib/support/logging/Constants.h>
#include <platform/logging/LogV.h>

#include "node.h"
#include "ucxm_host.h"

namespace {
int gErrorsExpected;
}

ucxm::SdkErrorsExpected::SdkErrorsExpected()
{
    gErrorsExpected++;
}

ucxm::SdkErrorsExpected::~SdkErrorsExpected()
{
    gErrorsExpected--;
}

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
    // The SDK's progress lines are many and written for its developers: detail. The node's own milestones are
    // the info lines (plugin.cpp). The data model's errors (DMG) are about what a controller asked for, such as an
    // attribute the node does not have; the controller gets the status back and goes on, so they are detail too.
    const bool error = category == kLogCategory_Error && strcmp(module, "DMG") != 0 && gErrorsExpected == 0;
    host->log(error ? UCXAPP_MATTER_LOG_ERROR : UCXAPP_MATTER_LOG_DETAIL, line);
}

} // namespace Platform
} // namespace Logging
} // namespace chip

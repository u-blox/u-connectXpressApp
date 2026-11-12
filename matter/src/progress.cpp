#include "progress.h"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "ucxm_host.h"

namespace ucxm {
namespace {

constexpr int kSteps = kStepComplete;

const char * const kDone[kSteps + 1] = {
    "",
    "paired with the passcode",
    "fabric installed",
    "network credentials received",
    "joining the network",
    "on the network",
    "complete, the node is ready",
};

const char * const kDoing[kSteps + 1] = {
    "",
    "pairing with the passcode",
    "attestation and certificates",
    "waiting for the network credentials",
    "joining the network",
    "getting on the network",
    "finishing",
};

std::atomic<int> gLast{ 0 };
std::mutex gFailedMutex;
char gFailed[120]; // the last failure, until the next attempt

void SetFailed(const char * text)
{
    std::lock_guard<std::mutex> lock(gFailedMutex);

    snprintf(gFailed, sizeof(gFailed), "%s", text);
}

} // namespace

void ProgressReset()
{
    gLast = 0;
    SetFailed("");
}

bool ProgressLine(char * out, size_t size)
{
    const int last = gLast;
    std::lock_guard<std::mutex> lock(gFailedMutex);

    if (gFailed[0] != '\0')
    {
        snprintf(out, size, "%s", gFailed);
        return true;
    }
    if (last < 1 || last >= kSteps)
    {
        return false;
    }
    snprintf(out, size, "%d/%d %s", last, kSteps, kDone[last]);
    return true;
}

void ProgressDone(ProgressStep step)
{
    char bar[kSteps + 1];
    char line[96];
    int last = gLast;

    while (step > last && !gLast.compare_exchange_weak(last, step))
    {
    }
    if (step <= last || gUcxmHost == nullptr)
    {
        return;
    }
    SetFailed("");
    for (int i = 0; i < kSteps; i++)
    {
        bar[i] = i < step ? '#' : '-';
    }
    bar[kSteps] = '\0';
    snprintf(line, sizeof(line), "commissioning [%s] %d/%d %s", bar, static_cast<int>(step), kSteps, kDone[step]);
    gUcxmHost->log(UCXAPP_MATTER_LOG_INFO, line);
    snprintf(line, sizeof(line), "matter: commissioning %d/%d, %s", static_cast<int>(step), kSteps, kDone[step]);
    gUcxmHost->event(line);
}

void ProgressFailed(int step, const char * reason)
{
    char line[160];

    if (step < 1 || step > kSteps)
    {
        step = gLast < kSteps ? gLast + 1 : kSteps;
    }
    if (gUcxmHost == nullptr)
    {
        return;
    }
    snprintf(line, sizeof(line), "commissioning failed at %d/%d (%s): %s", step, kSteps, kDoing[step], reason);
    gUcxmHost->log(UCXAPP_MATTER_LOG_ERROR, line);
    snprintf(line, sizeof(line), "matter: commissioning failed at %d/%d, %s", step, kSteps, reason);
    gUcxmHost->event(line);
    snprintf(line, sizeof(line), "failed at %d/%d: %s", step, kSteps, reason);
    SetFailed(line);
}

} // namespace ucxm

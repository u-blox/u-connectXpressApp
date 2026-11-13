#include "progress.h"

#include <atomic>
#include <cstdint>
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
std::mutex gMutex;   // what follows, between the Matter thread, the AT client's thread and the card's
char gFailed[120];   // the last failure, until the next attempt

// The commissioning clock. It starts when the commissioner connects over Bluetooth LE (or, over the network, with
// its pairing request) and each step keeps the time it was done at, so the last one is the whole commissioning
constexpr uint32_t kLinkToPairingMs = 30000; // a link older than this is not the commissioner that now pairs
bool gTimed;
uint32_t gStartMs;
bool gLinkSeen;
uint32_t gLinkMs;
uint32_t gStepMs[kSteps + 1];
bool gComplete;

// "18.4" for 18449 ms
size_t Seconds(uint32_t ms, char * out, size_t size)
{
    const int n = snprintf(out, size, "%u.%u", static_cast<unsigned>(ms / 1000), static_cast<unsigned>(ms % 1000 / 100));

    return n < 0 ? 0 : static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1;
}

} // namespace

void ProgressReset()
{
    std::lock_guard<std::mutex> lock(gMutex);

    gLast      = 0;
    gFailed[0] = '\0';
    gTimed     = false;
    gComplete  = false;
}

void ProgressLinkUp()
{
    if (gUcxmHost == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(gMutex);

    gLinkMs   = gUcxmHost->tickMs();
    gLinkSeen = true;
}

void ProgressBegin()
{
    if (gUcxmHost == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(gMutex);
    const uint32_t now = gUcxmHost->tickMs();

    if (gTimed)
    {
        return; // the attempt under way keeps its start: a clock that starts again would flatter it
    }
    gTimed    = true;
    gStartMs  = gLinkSeen && now - gLinkMs < kLinkToPairingMs ? gLinkMs : now;
    gLinkSeen = false;
}

bool ProgressLine(char * out, size_t size)
{
    const int last = gLast;
    std::lock_guard<std::mutex> lock(gMutex);

    if (gFailed[0] != '\0')
    {
        snprintf(out, size, "%s", gFailed);
        return true;
    }
    if (last < 1 || last > kSteps)
    {
        return false;
    }
    snprintf(out, size, "%d/%d %s", last, kSteps, kDone[last]);
    return true;
}

bool ProgressTimes(char * out, size_t size)
{
    const int last = gLast;
    std::lock_guard<std::mutex> lock(gMutex);
    size_t n;

    if (!gTimed || last < 1 || last > kSteps || size == 0)
    {
        return false;
    }
    n = Seconds(gStepMs[last], out, size);
    n += static_cast<size_t>(snprintf(out + n, size - n, " s%s (steps at", gComplete ? "" : " so far"));
    n = n < size ? n : size - 1;
    for (int i = 1; i <= last && n + 8 < size; i++)
    {
        out[n++] = ' ';
        n += Seconds(gStepMs[i], out + n, size - n);
    }
    snprintf(out + n, size - n, " s)");
    return true;
}

bool ProgressTotal(char * out, size_t size)
{
    std::lock_guard<std::mutex> lock(gMutex);

    if (!gComplete)
    {
        return false;
    }
    (void) Seconds(gStepMs[kSteps], out, size);
    return true;
}

void ProgressDone(ProgressStep step)
{
    char bar[kSteps + 1];
    char seconds[16];
    char line[120];
    int last = gLast;

    while (step > last && !gLast.compare_exchange_weak(last, step))
    {
    }
    if (step <= last || gUcxmHost == nullptr)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(gMutex);
        const uint32_t now = gUcxmHost->tickMs();

        gFailed[0] = '\0';
        if (!gTimed)
        {
            gTimed   = true; // its start was not seen: the clock runs from the first step
            gStartMs = now;
        }
        for (int i = last + 1; i <= step; i++)
        {
            gStepMs[i] = now - gStartMs;
        }
        gComplete = step == kStepComplete;
        (void) Seconds(now - gStartMs, seconds, sizeof(seconds));
    }
    for (int i = 0; i < kSteps; i++)
    {
        bar[i] = i < step ? '#' : '-';
    }
    bar[kSteps] = '\0';
    snprintf(line, sizeof(line), "commissioning [%s] %d/%d %s (%s s)", bar, static_cast<int>(step), kSteps, kDone[step],
             seconds);
    gUcxmHost->log(UCXAPP_MATTER_LOG_INFO, line);
    snprintf(line, sizeof(line), "matter: commissioning %d/%d, %s (%s s)", static_cast<int>(step), kSteps, kDone[step],
             seconds);
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
    std::lock_guard<std::mutex> lock(gMutex);

    snprintf(gFailed, sizeof(gFailed), "failed at %d/%d: %s", step, kSteps, reason);
    gTimed = false; // the next attempt has a clock of its own
}

} // namespace ucxm

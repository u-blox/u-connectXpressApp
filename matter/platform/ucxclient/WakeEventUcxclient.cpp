/*
 * Platform-agnostic WakeEvent implementation for ucxclient platform
 * 
 * This implementation does NOT use any host OS sockets or network interfaces.
 * All Matter network I/O goes through NORA-W36 via ucxclient AT commands.
 * 
 * This WakeEvent uses a simple mutex + condition variable approach that works
 * on any platform (Windows, Linux, FreeRTOS, etc.) without requiring sockets.
 * 
 * The "file descriptor" returned is a fake FD that will be ignored by select()
 * but allows the event loop to function for timer processing.
 */

#include <system/SystemConfig.h>

// Only compile this when ucxclient WakeEvent is enabled
#if defined(CHIP_SYSTEM_CONFIG_USE_UCXCLIENT_WAKE_EVENT) && CHIP_SYSTEM_CONFIG_USE_UCXCLIENT_WAKE_EVENT

#if CHIP_SYSTEM_CONFIG_USE_SOCKETS && !CHIP_SYSTEM_CONFIG_USE_LIBEV

#include <system/WakeEvent.h>
#include <system/SystemLayerImplSelect.h>  // For LayerSockets
#include <lib/support/CodeUtils.h>
#include <system/SystemError.h>
#include <system/SystemLayer.h>
#include <lib/support/logging/CHIPLogging.h>

#include <mutex>
#include <chrono>
#include <condition_variable>
#include <atomic>

namespace chip {
namespace System {

void UcxWake();
bool UcxWakeWait(uint32_t timeoutMs);

namespace {

// Global wake state - used to signal the event loop
// This is platform-agnostic (works on Windows, Linux, FreeRTOS via std::mutex)
struct WakeState {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> signaled{false};
    bool initialized{false};
};

static WakeState sWakeState;

// Fake file descriptor - not a real FD, just a marker
// select() on Windows/Linux will ignore invalid FDs, and we set timeout to 0
// so it immediately returns, allowing timer processing to work
constexpr int kFakeWakeFD = 9999;

} // anonymous namespace

// 1.6: WakeEvent::Open/Close no longer take a LayerSockets& argument.
CHIP_ERROR WakeEvent::Open()
{
    std::lock_guard<std::mutex> lock(sWakeState.mutex);
    
    if (sWakeState.initialized)
    {
        ChipLogProgress(chipSystemLayer, "[WakeEvent] Already initialized");
        return CHIP_NO_ERROR;
    }
    
    sWakeState.signaled.store(false);
    sWakeState.initialized = true;
    
    // Use a fake FD - we don't actually use select() for socket I/O
    // All networking goes through NORA-W36 via ucxclient
    mReadFD = kFakeWakeFD;
    
    ChipLogProgress(chipSystemLayer, "[WakeEvent] Initialized (platform-agnostic, no host sockets)");
    
    return CHIP_NO_ERROR;
}

void WakeEvent::Close()
{
    std::lock_guard<std::mutex> lock(sWakeState.mutex);
    
    if (!sWakeState.initialized)
    {
        return;
    }
    
    sWakeState.initialized = false;
    sWakeState.signaled.store(false);
    mReadFD = -1;
    
    // Wake any waiting threads so they can exit
    sWakeState.cv.notify_all();
    
    ChipLogProgress(chipSystemLayer, "[WakeEvent] Closed");
}

void WakeEvent::Confirm() const
{
    // Clear the signal - another thread called Notify() and we're acknowledging it
    sWakeState.signaled.store(false);
}

CHIP_ERROR WakeEvent::Notify() const
{
    UcxWake();
    return CHIP_NO_ERROR;
}

// u-connectXpressApp: what the event loop waits on (LayerImplSelect::WaitForEvents) instead of select()
void UcxWake()
{
    {
        std::lock_guard<std::mutex> lock(sWakeState.mutex);

        sWakeState.signaled.store(true);
    }
    sWakeState.cv.notify_one();
}

bool UcxWakeWait(uint32_t timeoutMs)
{
    std::unique_lock<std::mutex> lock(sWakeState.mutex);
    const bool signaled = sWakeState.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                                 [] { return sWakeState.signaled.load(); });

    sWakeState.signaled.store(false);
    return signaled;
}

} // namespace System
} // namespace chip

#endif // CHIP_SYSTEM_CONFIG_USE_SOCKETS && !CHIP_SYSTEM_CONFIG_USE_LIBEV

#endif // CHIP_SYSTEM_CONFIG_USE_UCXCLIENT_WAKE_EVENT

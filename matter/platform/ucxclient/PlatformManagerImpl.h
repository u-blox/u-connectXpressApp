/*
 * Copyright (c) 2024 Project CHIP Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once

#include <platform/internal/GenericPlatformManagerImpl.h>

#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

namespace chip {
namespace DeviceLayer {

/**
 * ucxclient platform implementation of PlatformManager
 * 
 * This is a portable implementation that works on Windows/POSIX/FreeRTOS
 * using standard C++ primitives (std::queue, std::mutex, std::condition_variable)
 * instead of platform-specific APIs like pthread or poll().
 */
class PlatformManagerImpl final : public PlatformManager,
                                   public Internal::GenericPlatformManagerImpl<PlatformManagerImpl>
{
    // Allow access from base class and PlatformManager
    friend PlatformManager;
    friend Internal::GenericPlatformManagerImpl<PlatformManagerImpl>;
    
public:
    // Platform-specific initialization
    CHIP_ERROR _InitChipStack();
    void _Shutdown();
    
    // Event handling
    CHIP_ERROR _AddEventHandler(EventHandlerFunct handler, intptr_t arg);
    void _RemoveEventHandler(EventHandlerFunct handler, intptr_t arg);
    
    // Server lifecycle
    void _HandleServerStarted();
    void _HandleServerShuttingDown();
    
    // Work scheduling
    CHIP_ERROR _ScheduleWork(AsyncWorkFunct workFunct, intptr_t arg);
    
    // Event posting and dispatch - CRITICAL for BLE events!
    CHIP_ERROR _PostEvent(const ChipDeviceEvent * event);
    void _DispatchEvent(const ChipDeviceEvent * event);
    
    // Stack locking (using portable std::mutex)
    void _LockChipStack();
    void _UnlockChipStack();
    bool _TryLockChipStack();
    
    // Event loop management
    void _RunEventLoop();
    CHIP_ERROR _StartEventLoopTask();
    CHIP_ERROR _StopEventLoopTask();
    
    // Background work and events
    CHIP_ERROR _ScheduleBackgroundWork(AsyncWorkFunct workFunct, intptr_t arg);
    CHIP_ERROR _PostBackgroundEvent(const ChipDeviceEvent * event);
    void _RunBackgroundEventLoop();
    CHIP_ERROR _StartBackgroundEventLoopTask();
    CHIP_ERROR _StopBackgroundEventLoopTask();
    
    // Timer support
    CHIP_ERROR _StartChipTimer(System::Clock::Timeout duration);
    
    // Process device events from internal queue (called by event loop)
    void ProcessDeviceEvents();
    
    // Per-iteration application callback (called from _RunEventLoop after each iteration).
    // Use this to process app-level concerns (web server, AP provisioning, LED updates, etc.)
    // without duplicating the event loop on every platform. Follows ESP32's SetAppCallback pattern.
    using AppLoopCallback = void (*)();
    void SetAppLoopCallback(AppLoopCallback callback) { mAppLoopCallback = callback; }
    
    static PlatformManagerImpl & GetDefaultInstance();

private:
    // Event queue using portable C++ primitives
    std::queue<ChipDeviceEvent> mEventQueue;
    std::mutex mEventQueueMutex;
    std::condition_variable mEventQueueCondition;
    
    // Stack lock (portable) — MUST be recursive because dispatched events
    // may call ProcessDeviceEvents() re-entrantly (e.g., ConnectNetwork →
    // WaitForNetworkReady → ProcessDeviceEvents). std::mutex deadlocks here.
    std::recursive_mutex mChipStackLock;
    
    // Event loop thread
    std::thread mEventLoopThread;
    std::atomic<bool> mEventLoopRunning{ false };
    std::atomic<bool> mShouldRunEventLoop{ false };
    
    // Per-iteration app callback (set via SetAppLoopCallback)
    AppLoopCallback mAppLoopCallback = nullptr;
};

/**
 * Returns the platform-specific implementation of PlatformManager
 */
inline PlatformManagerImpl & PlatformMgrImpl()
{
    return PlatformManagerImpl::GetDefaultInstance();
}

} // namespace DeviceLayer
} // namespace chip

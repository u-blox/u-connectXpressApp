/*
 * Copyright (c) 2024 Project CHIP Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <platform/internal/CHIPDeviceLayerInternal.h>
#include <platform/PlatformManager.h>
#include "PlatformManagerImpl.h"

// AddressPolicy is only needed for minimal mDNS (SDK handles mDNS in software)
// With platform mDNS, NORA-W36 firmware handles mDNS via AT+UWMDNS* commands
#if defined(UCX_MDNS_MINIMAL) && UCX_MDNS_MINIMAL
#include "AddressPolicy_Ucxclient.h"
#endif

// Include the base template implementation (portable, no POSIX-specific headers)
#include <platform/internal/GenericPlatformManagerImpl.ipp>

// For SystemLayerImplSelect event loop interface
#include <system/SystemLayerImplSelect.h>

// For ProcessUrcs() in the event loop - reads NORA-W36 UART and delivers UDP packets
#include "UcxClientManager.h"

// u-connectXpressApp: no Winsock, the node has no socket on the PC (the wake event is WakeEventUcxclient.cpp)

namespace chip {
namespace DeviceLayer {

static PlatformManagerImpl sInstance;

PlatformManagerImpl & PlatformManagerImpl::GetDefaultInstance()
{
    return sInstance;
}

CHIP_ERROR PlatformManagerImpl::_InitChipStack()
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    ChipLogProgress(DeviceLayer, "[PlatformMgr] Initializing System Layer...");
    err = DeviceLayer::SystemLayer().Init();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "[PlatformMgr] ERROR: SystemLayer Init failed");
        return err;
    }
    ChipLogProgress(DeviceLayer, "[PlatformMgr] System Layer initialized");

#if defined(UCX_MDNS_MINIMAL) && UCX_MDNS_MINIMAL
    // Initialize custom AddressPolicy for minimal mDNS (BEFORE mDNS server starts)
    // Only needed when SDK handles mDNS in software - not with platform mDNS
    ChipLogProgress(DeviceLayer, "[PlatformMgr] Initializing custom mDNS AddressPolicy for NORA-W36...");
    mdns::Minimal::InitializeUcxclientAddressPolicy();
#else
    // Platform mDNS: NORA-W36 firmware handles mDNS via AT+UWMDNS* commands
    ChipLogProgress(DeviceLayer, "[PlatformMgr] Using platform mDNS (NORA-W36 firmware handles mDNS)");
#endif

    // Initialize Connectivity Manager (includes ucxclient)
    ChipLogProgress(DeviceLayer, "[PlatformMgr] Initializing Connectivity Manager...");
    err = ConnectivityMgr().Init();
    SuccessOrExit(err);
    ChipLogProgress(DeviceLayer, "[PlatformMgr] Connectivity Manager initialized");

exit:
    return err;
}

void PlatformManagerImpl::_Shutdown()
{
    ChipLogProgress(DeviceLayer, "[PlatformMgr] Shutting down");
    mShouldRunEventLoop.store(false);
    {
        // u-connectXpressApp: the node can start again in the same process; work queued during this
        // shutdown (the BLE layer's deferred disconnect) must not run in the next node and close its link
        std::lock_guard<std::mutex> lock(mEventQueueMutex);
        std::queue<ChipDeviceEvent>().swap(mEventQueue);
    }
    mEventQueueCondition.notify_all();

    // Delegate to base class to release event handlers / internal state
    Internal::GenericPlatformManagerImpl<PlatformManagerImpl>::_Shutdown();
}

CHIP_ERROR PlatformManagerImpl::_AddEventHandler(EventHandlerFunct handler, intptr_t arg)
{
    // Use parent class implementation for event handlers
    return Internal::GenericPlatformManagerImpl<PlatformManagerImpl>::_AddEventHandler(handler, arg);
}

void PlatformManagerImpl::_RemoveEventHandler(EventHandlerFunct handler, intptr_t arg)
{
    Internal::GenericPlatformManagerImpl<PlatformManagerImpl>::_RemoveEventHandler(handler, arg);
}

void PlatformManagerImpl::_HandleServerStarted()
{
    ChipLogProgress(DeviceLayer, "[PlatformMgr] Server started");
}

void PlatformManagerImpl::_HandleServerShuttingDown()
{
    ChipLogProgress(DeviceLayer, "[PlatformMgr] Server shutting down");
}

CHIP_ERROR PlatformManagerImpl::_ScheduleWork(AsyncWorkFunct workFunct, intptr_t arg)
{
    // Create an event to schedule the work
    ChipDeviceEvent event;
    event.Type = DeviceEventType::kCallWorkFunct;
    event.CallWorkFunct.WorkFunct = workFunct;
    event.CallWorkFunct.Arg = arg;
    
    return _PostEvent(&event);
}

CHIP_ERROR PlatformManagerImpl::_PostEvent(const ChipDeviceEvent * event)
{
    if (event == nullptr)
    {
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
    
    {
        std::lock_guard<std::mutex> lock(mEventQueueMutex);
        mEventQueue.push(*event);
    }
    
    // Wake up the event loop by notifying the condition variable
    // This is CRITICAL for BLE - the event loop must process kCHIPoBLEIndicateConfirm
    // immediately so the BTP layer can send the next fragment without delay
    mEventQueueCondition.notify_one();
    
    // Also wake up the System Layer (for select()-based loops)
    // This triggers the WakeEvent to interrupt select()
    static_cast<System::LayerImplSelect &>(DeviceLayer::SystemLayer()).Signal();
    
    return CHIP_NO_ERROR;
}

void PlatformManagerImpl::_DispatchEvent(const ChipDeviceEvent * event)
{
    // Handle special event types first
    if (event->Type == DeviceEventType::kCallWorkFunct)
    {
        event->CallWorkFunct.WorkFunct(event->CallWorkFunct.Arg);
        return;
    }
    
    // Log platform events that are important for commissioning diagnostics
    if (event->Type == DeviceEventType::kDnssdRestartNeeded)
    {
        ChipLogProgress(DeviceLayer, "[EventDispatch] Dispatching kDnssdRestartNeeded");
    }
    else if (event->Type == DeviceEventType::kInterfaceIpAddressChanged)
    {
        // Quieted to Detail (Vegas 2026): fires ~6× per Wi-Fi reconnect cycle —
        // the meaningful state change is already logged in UcxClientManager.
        ChipLogDetail(DeviceLayer, "[EventDispatch] Dispatching kInterfaceIpAddressChanged");
    }
    else if (event->Type == DeviceEventType::kCHIPoBLEConnectionError)
    {
        ChipLogProgress(DeviceLayer, "[EventDispatch] Dispatching kCHIPoBLEConnectionError");
    }
    
    // Dispatch to device layer components (BLEManager, ConnectivityManager, etc.)
    // This is what GenericPlatformManagerImpl::DispatchEventToDeviceLayer does
#if CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE
    Internal::BLEMgr().OnPlatformEvent(event);
#endif
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    ThreadStackMgr().OnPlatformEvent(event);
#endif
    ConnectivityMgr().OnPlatformEvent(event);
    
    // Also dispatch to application event handlers
    Internal::GenericPlatformManagerImpl<PlatformManagerImpl>::DispatchEventToApplication(event);
}

void PlatformManagerImpl::_LockChipStack()
{
    mChipStackLock.lock();
}

void PlatformManagerImpl::_UnlockChipStack()
{
    mChipStackLock.unlock();
}

bool PlatformManagerImpl::_TryLockChipStack()
{
    return mChipStackLock.try_lock();
}

void PlatformManagerImpl::_RunEventLoop()
{
    ChipLogProgress(DeviceLayer, "[PlatformMgr] Starting event loop");
    
    mShouldRunEventLoop.store(true);
    mEventLoopRunning.store(true);
    
    // Get reference to System Layer for event loop processing
    // SystemLayerImplSelect needs PrepareEvents/WaitForEvents/HandleEvents cycle
    // to process timers and scheduled work (including reporting engine!)
    auto & systemLayer = static_cast<System::LayerImplSelect &>(DeviceLayer::SystemLayer());
    
    ChipLogProgress(DeviceLayer, "[PlatformMgr] Event loop using SystemLayerImplSelect");
    systemLayer.EventLoopBegins();
    
    while (mShouldRunEventLoop.load())
    {
        // CRITICAL: Process NORA-W36 serial data and deliver queued UDP packets.
        // On desktop, main.cpp calls ProcessUrcs() explicitly in its while loop.
        // On STM32/FreeRTOS, _RunEventLoop() IS the while loop, so we call it here.
        // This reads UART URCs (+UESODBF with inline UDP data), queues packets,
        // then ProcessQueuedPackets() delivers them to the Matter SDK.
        // Runs OUTSIDE chip stack lock (matching desktop behavior).
        UcxClientManager::GetInstance().ProcessUrcs();

        _LockChipStack();
        
        // Prepare events - sets up select() timeout based on pending timers
        systemLayer.PrepareEvents();
        
        _UnlockChipStack();
        
        // Wait for events with a timeout (allows periodic processing)
        // This calls select() which may block until timeout or socket activity
        systemLayer.WaitForEvents();
        
        _LockChipStack();
        
        // Handle events - processes expired timers (including ScheduleWork callbacks!)
        // This is where the reporting engine's Run() callback gets invoked!
        systemLayer.HandleEvents();
        
        // Process device events from our queue
        ProcessDeviceEvents();
        
        // CRITICAL: Process any newly scheduled work IMMEDIATELY!
        // When HandleEvents() or ProcessDeviceEvents() calls ScheduleWork(),
        // a timer with NOW timestamp is added. We must process it before
        // the next WaitForEvents() to avoid race conditions with BLE disconnects.
        // This ensures responses (like ReadResponse) are sent before connection closes.
        //
        // Run a second pass with zero timeout to catch immediate work
        systemLayer.PrepareEvents();  // Sets timeout to 0 if there's immediate work
        
        _UnlockChipStack();
        
        systemLayer.WaitForEvents();  // Returns immediately if timeout is 0
        
        _LockChipStack();
        
        systemLayer.HandleEvents();   // Process immediate timers
        
        _UnlockChipStack();

        // Deferred mDNS restart: OnWiFiStationNetworkUp sets a flag instead of
        // posting kDnssdRestartNeeded directly, because during commissioning the URC
        // fires inside WaitForNetworkReady's nested ProcessDeviceEvents which hangs
        // on mDNS AT commands. Posting here runs in a clean main-loop context.
        {
            auto & ucxMgr = UcxClientManager::GetInstance();
            if (ucxMgr.IsDnssdRestartPending())
            {
                ucxMgr.ClearDnssdRestartPending();
                ChipLogProgress(DeviceLayer, "[PlatformMgr] Posting deferred kDnssdRestartNeeded...");
                ChipDeviceEvent dnsEvent = {};
                dnsEvent.Type = DeviceEventType::kDnssdRestartNeeded;
                PlatformMgr().PostEventOrDie(&dnsEvent);
            }
        }

        // Per-iteration app callback: process web server, AP provisioning,
        // VirtualHardware updates, etc. Runs OUTSIDE chip stack lock.
        if (mAppLoopCallback)
        {
            mAppLoopCallback();
        }
    }
    
    systemLayer.EventLoopEnds();
    mEventLoopRunning.store(false);
    ChipLogProgress(DeviceLayer, "[PlatformMgr] Event loop exited");
}

void PlatformManagerImpl::ProcessDeviceEvents()
{
    // CRITICAL: This function is called from the main loop in main.cpp
    // It must process BOTH device events AND system layer timers!
    //
    // The system layer timers include ScheduleWork() callbacks like the
    // reporting engine's Run() which sends responses to ReadRequest.
    //
    // Without processing timers here, BLE commissioning fails because
    // responses are never sent!
    //
    // NOTE: This function MUST BE NON-BLOCKING! The main loop in main.cpp
    // needs to continue running to display QR codes, process keyboard input,
    // and handle URCs from NORA-W36.
    //
    // We only call HandleEvents (not WaitForEvents) to process any already-pending
    // timers without blocking. The main loop's 1ms sleep provides the timing.
    
    auto & systemLayer = static_cast<System::LayerImplSelect &>(DeviceLayer::SystemLayer());
    
    _LockChipStack();
    
    // PrepareEvents sets up the fd_sets and calculates the next timer timeout
    // This also marks expired timers for processing
    systemLayer.PrepareEvents();
    
    // HandleEvents processes expired timers (including ScheduleWork callbacks!)
    // This is where the reporting engine's Run() gets invoked!
    // NOTE: We skip WaitForEvents() to make this non-blocking!
    // The main loop has its own 1ms sleep, so we don't need to block here.
    systemLayer.HandleEvents();
    
    // Process device events from our queue
    std::unique_lock<std::mutex> lock(mEventQueueMutex);
    
    while (!mEventQueue.empty())
    {
        ChipDeviceEvent event = mEventQueue.front();
        mEventQueue.pop();
        
        // Unlock while dispatching to avoid deadlock
        lock.unlock();
        _DispatchEvent(&event);
        lock.lock();
    }
    
    lock.unlock();
    _UnlockChipStack();
}

CHIP_ERROR PlatformManagerImpl::_StartEventLoopTask()
{
    ChipLogProgress(DeviceLayer, "[PlatformMgr] Starting event loop thread");
    
    if (mEventLoopRunning.load())
    {
        return CHIP_ERROR_INCORRECT_STATE;
    }
    
    mShouldRunEventLoop.store(true);
    mEventLoopThread = std::thread([this]() {
        this->_RunEventLoop();
    });
    
    return CHIP_NO_ERROR;
}

CHIP_ERROR PlatformManagerImpl::_StopEventLoopTask()
{
    ChipLogProgress(DeviceLayer, "[PlatformMgr] Stopping event loop thread");
    
    mShouldRunEventLoop.store(false);
    mEventQueueCondition.notify_all();
    
    if (mEventLoopThread.joinable())
    {
        mEventLoopThread.join();
    }
    
    return CHIP_NO_ERROR;
}

CHIP_ERROR PlatformManagerImpl::_ScheduleBackgroundWork(AsyncWorkFunct workFunct, intptr_t arg)
{
    // For simplicity, schedule background work same as regular work
    return _ScheduleWork(workFunct, arg);
}

CHIP_ERROR PlatformManagerImpl::_PostBackgroundEvent(const ChipDeviceEvent * event)
{
    // For simplicity, post background events to main queue
    return _PostEvent(event);
}

void PlatformManagerImpl::_RunBackgroundEventLoop()
{
    // Not used in this implementation
}

CHIP_ERROR PlatformManagerImpl::_StartBackgroundEventLoopTask()
{
    // Not needed - we use single event loop
    return CHIP_NO_ERROR;
}

CHIP_ERROR PlatformManagerImpl::_StopBackgroundEventLoopTask()
{
    // Not needed
    return CHIP_NO_ERROR;
}

CHIP_ERROR PlatformManagerImpl::_StartChipTimer(System::Clock::Timeout duration)
{
    // Timers are handled by System Layer - just need to wake the event loop
    // The SystemLayerImplSelect will handle the actual timer via select() timeout
    return CHIP_NO_ERROR;
}

// Provide non-inline definition of PlatformMgr() for linking
// This returns reference to the singleton instance
PlatformManager & PlatformMgr()
{
    return PlatformManagerImpl::GetDefaultInstance();
}

} // namespace DeviceLayer
} // namespace chip

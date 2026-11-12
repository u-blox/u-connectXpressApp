/*
 *    Copyright (c) 2025 u-blox
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 * @file
 *   Platform abstraction layer for cross-platform code.
 *
 *   Provides unified APIs for:
 *   - Sleep/delay functions
 *   - Mutexes (thread synchronization)
 *   - Time measurement
 *
 *   Supported platforms:
 *   - Windows, Linux and WebAssembly (the C++ standard library)
 *   - STM32 on FreeRTOS (U_PORT_FREERTOS)
 *   - STM32 on Zephyr (__ZEPHYR__)
 */

#pragma once

#include <cstdint>

#if defined(U_PORT_FREERTOS)
// FreeRTOS platform
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#elif defined(__ZEPHYR__)
// Zephyr RTOS platform
#include <zephyr/kernel.h>
#else
// Windows, Linux, WebAssembly
#include <chrono>
#include <thread>
#include <mutex>
#endif

namespace chip {
namespace Platform {

// ============================================================================
// Sleep/Delay Functions
// ============================================================================

/**
 * @brief Sleep for the specified number of milliseconds.
 *
 * On FreeRTOS, this yields the task to the scheduler.
 * On desktop platforms, this uses std::this_thread::sleep_for().
 */
inline void SleepMs(uint32_t milliseconds)
{
#if defined(U_PORT_FREERTOS)
    // Convert milliseconds to FreeRTOS ticks
    // pdMS_TO_TICKS handles the conversion based on configTICK_RATE_HZ
    TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    if (ticks == 0 && milliseconds > 0)
    {
        ticks = 1;  // At least 1 tick for non-zero sleep
    }
    vTaskDelay(ticks);
#elif defined(__ZEPHYR__)
    k_sleep(K_MSEC(milliseconds));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
#endif
}

/**
 * @brief Sleep for the specified number of seconds.
 */
inline void SleepSeconds(uint32_t seconds)
{
    SleepMs(seconds * 1000);
}

// ============================================================================
// Time Measurement
// ============================================================================

/**
 * @brief Get current time in milliseconds (monotonic clock).
 *
 * Used for measuring elapsed time, not wall-clock time.
 */
inline uint32_t GetTickMs()
{
#if defined(U_PORT_FREERTOS)
    return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
#elif defined(__ZEPHYR__)
    return static_cast<uint32_t>(k_uptime_get());
#else
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return static_cast<uint32_t>(ms.count());
#endif
}

/**
 * @brief Calculate elapsed time in milliseconds.
 *
 * @param startTickMs The tick value from GetTickMs() at start.
 * @return Elapsed milliseconds since startTickMs.
 */
inline uint32_t ElapsedMs(uint32_t startTickMs)
{
    uint32_t now = GetTickMs();
    // Handle wrap-around (works for 32-bit unsigned)
    return now - startTickMs;
}

// ============================================================================
// Mutex (Thread Synchronization)
// ============================================================================

/**
 * @brief Platform-independent mutex class.
 *
 * Provides RAII-style locking via Lock() and Unlock(), or use ScopedLock.
 */
// Lock and Unlock are split on purpose, which clang's analysis of libc++'s annotated std::mutex cannot follow
#if defined(__clang__)
#define UCXM_NO_THREAD_SAFETY_ANALYSIS __attribute__((no_thread_safety_analysis))
#else
#define UCXM_NO_THREAD_SAFETY_ANALYSIS
#endif
class Mutex
{
public:
    Mutex()
    {
#if defined(U_PORT_FREERTOS)
        mMutex = xSemaphoreCreateMutex();
#elif defined(__ZEPHYR__)
        k_mutex_init(&mMutex);
#endif
    }

    ~Mutex()
    {
#if defined(U_PORT_FREERTOS)
        if (mMutex != nullptr)
        {
            vSemaphoreDelete(mMutex);
        }
#endif
        // Zephyr k_mutex doesn't need explicit destruction
    }

    // Non-copyable
    Mutex(const Mutex &) = delete;
    Mutex & operator=(const Mutex &) = delete;

    void Lock() UCXM_NO_THREAD_SAFETY_ANALYSIS
    {
#if defined(U_PORT_FREERTOS)
        if (mMutex != nullptr)
        {
            xSemaphoreTake(mMutex, portMAX_DELAY);
        }
#elif defined(__ZEPHYR__)
        k_mutex_lock(&mMutex, K_FOREVER);
#else
        mMutex.lock();
#endif
    }

    void Unlock() UCXM_NO_THREAD_SAFETY_ANALYSIS
    {
#if defined(U_PORT_FREERTOS)
        if (mMutex != nullptr)
        {
            xSemaphoreGive(mMutex);
        }
#elif defined(__ZEPHYR__)
        k_mutex_unlock(&mMutex);
#else
        mMutex.unlock();
#endif
    }

    /**
     * @brief Try to acquire the mutex without blocking.
     * @return true if acquired, false if already held by another thread.
     */
    bool TryLock()
    {
#if defined(U_PORT_FREERTOS)
        if (mMutex != nullptr)
        {
            return xSemaphoreTake(mMutex, 0) == pdTRUE;
        }
        return false;
#elif defined(__ZEPHYR__)
        return k_mutex_lock(&mMutex, K_NO_WAIT) == 0;
#else
        return mMutex.try_lock();
#endif
    }

private:
#if defined(U_PORT_FREERTOS)
    SemaphoreHandle_t mMutex = nullptr;
#elif defined(__ZEPHYR__)
    struct k_mutex mMutex;
#else
    std::mutex mMutex;
#endif
};

/**
 * @brief RAII-style scoped lock for Mutex.
 *
 * Locks the mutex on construction, unlocks on destruction.
 * Similar to std::lock_guard.
 */
class ScopedLock
{
public:
    explicit ScopedLock(Mutex & mutex) : mMutex(mutex)
    {
        mMutex.Lock();
    }

    ~ScopedLock()
    {
        mMutex.Unlock();
    }

    // Non-copyable
    ScopedLock(const ScopedLock &) = delete;
    ScopedLock & operator=(const ScopedLock &) = delete;

private:
    Mutex & mMutex;
};

// ============================================================================
// Static Container Helpers (for FreeRTOS memory constraints)
// ============================================================================

/**
 * @brief Fixed-capacity array with dynamic size tracking.
 *
 * On desktop, you might prefer std::vector, but this works everywhere
 * without dynamic memory allocation.
 *
 * @tparam T Element type
 * @tparam N Maximum capacity
 */
template <typename T, size_t N>
class StaticArray
{
public:
    StaticArray() : mSize(0) {}

    bool Push(const T & item)
    {
        if (mSize >= N)
        {
            return false;  // Full
        }
        mData[mSize++] = item;
        return true;
    }

    bool Pop(T & item)
    {
        if (mSize == 0)
        {
            return false;  // Empty
        }
        item = mData[--mSize];
        return true;
    }

    void Clear()
    {
        mSize = 0;
    }

    size_t Size() const { return mSize; }
    size_t Capacity() const { return N; }
    bool IsEmpty() const { return mSize == 0; }
    bool IsFull() const { return mSize >= N; }

    T & operator[](size_t index) { return mData[index]; }
    const T & operator[](size_t index) const { return mData[index]; }

    // Iterator support
    T * begin() { return mData; }
    T * end() { return mData + mSize; }
    const T * begin() const { return mData; }
    const T * end() const { return mData + mSize; }

private:
    T mData[N];
    size_t mSize;
};

/**
 * @brief Simple key-value map with fixed capacity.
 *
 * Uses linear search - suitable for small maps.
 *
 * @tparam K Key type
 * @tparam V Value type
 * @tparam N Maximum capacity
 */
template <typename K, typename V, size_t N>
class StaticMap
{
public:
    StaticMap() : mSize(0) {}

    /**
     * @brief Insert or update a key-value pair.
     * @return true on success, false if full and key not found.
     */
    bool Insert(const K & key, const V & value)
    {
        // Check if key exists
        for (size_t i = 0; i < mSize; i++)
        {
            if (mKeys[i] == key)
            {
                mValues[i] = value;
                return true;
            }
        }
        // Key not found, insert new
        if (mSize >= N)
        {
            return false;  // Full
        }
        mKeys[mSize] = key;
        mValues[mSize] = value;
        mSize++;
        return true;
    }

    /**
     * @brief Find value by key.
     * @return Pointer to value if found, nullptr if not found.
     */
    V * Find(const K & key)
    {
        for (size_t i = 0; i < mSize; i++)
        {
            if (mKeys[i] == key)
            {
                return &mValues[i];
            }
        }
        return nullptr;
    }

    const V * Find(const K & key) const
    {
        for (size_t i = 0; i < mSize; i++)
        {
            if (mKeys[i] == key)
            {
                return &mValues[i];
            }
        }
        return nullptr;
    }

    /**
     * @brief Remove a key-value pair.
     * @return true if found and removed, false if not found.
     */
    bool Remove(const K & key)
    {
        for (size_t i = 0; i < mSize; i++)
        {
            if (mKeys[i] == key)
            {
                // Move last element to this position
                if (i < mSize - 1)
                {
                    mKeys[i] = mKeys[mSize - 1];
                    mValues[i] = mValues[mSize - 1];
                }
                mSize--;
                return true;
            }
        }
        return false;
    }

    void Clear()
    {
        mSize = 0;
    }

    size_t Size() const { return mSize; }
    size_t Capacity() const { return N; }
    bool IsEmpty() const { return mSize == 0; }
    bool IsFull() const { return mSize >= N; }

    // Access by index (for iteration)
    const K & KeyAt(size_t index) const { return mKeys[index]; }
    V & ValueAt(size_t index) { return mValues[index]; }
    const V & ValueAt(size_t index) const { return mValues[index]; }

private:
    K mKeys[N];
    V mValues[N];
    size_t mSize;
};

}  // namespace Platform
}  // namespace chip

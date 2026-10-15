// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include <3ds.h>
#include <functional>
#include <memory>

namespace tz
{

// A LightLock, usable with std::lock_guard and std::unique_lock.
class Mutex
{
public:
    Mutex() { LightLock_Init(&lock_); }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() { LightLock_Lock(&lock_); }
    bool try_lock() { return LightLock_TryLock(&lock_) == 0; }
    void unlock() { LightLock_Unlock(&lock_); }
    LightLock* native() { return &lock_; }

private:
    LightLock lock_;
};

class ConditionVariable
{
public:
    ConditionVariable() { CondVar_Init(&cv_); }
    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    // The mutex must be locked; it is again when this returns.
    void wait(Mutex& mutex) { CondVar_Wait(&cv_, mutex.native()); }
    // Returns false on timeout.
    bool waitFor(Mutex& mutex, s64 nanoseconds) { return CondVar_WaitTimeout(&cv_, mutex.native(), nanoseconds) == 0; }
    void notifyOne() { CondVar_Signal(&cv_); }
    void notifyAll() { CondVar_Broadcast(&cv_); }

private:
    CondVar cv_;
};

// Thread priorities: lower numbers run first. The main thread runs at kMain.
namespace priority
{
constexpr int kAudio = 0x18;   // above the UI, so the music never skips
constexpr int kMain = 0x30;
constexpr int kWorker = 0x38;  // below the UI: work happens while it waits for the next frame
constexpr int kNetwork = 0x38;
constexpr int kIdle = 0x3F;
} // namespace priority

// A joinable thread running a function; joined when destroyed.
class Thread
{
public:
    Thread() = default;
    ~Thread() { join(); }
    Thread(Thread&& other) noexcept : handle_(other.handle_), state_(std::move(other.state_)) { other.handle_ = nullptr; }
    Thread& operator=(Thread&& other) noexcept;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    // core -2 is the app's default core (the one the main thread runs on), 1 the system core.
    static Thread start(std::function<void()> function, int priority, int core = -2, size_t stackSize = 64 * 1024);

    bool running() const { return handle_ != nullptr; }
    void join();

private:
    struct State
    {
        std::function<void()> function;
    };
    static void entry(void* arg);

    ::Thread handle_ = nullptr;
    std::unique_ptr<State> state_;
};

void sleepMs(u32 milliseconds);

} // namespace tz

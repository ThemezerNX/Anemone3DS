// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "sys/thread.hpp"

namespace tz
{

Thread& Thread::operator=(Thread&& other) noexcept
{
    if (this != &other)
    {
        join();
        handle_ = other.handle_;
        state_ = std::move(other.state_);
        other.handle_ = nullptr;
    }
    return *this;
}

void Thread::entry(void* arg)
{
    static_cast<State*>(arg)->function();
}

Thread Thread::start(std::function<void()> function, int priority, int core, size_t stackSize)
{
    Thread thread;
    thread.state_ = std::make_unique<State>(State{std::move(function)});
    thread.handle_ = threadCreate(entry, thread.state_.get(), stackSize, priority, core, false);
    if (!thread.handle_ && core != -2)
    {
        // the system core isn't available to every app: fall back to ours
        thread.handle_ = threadCreate(entry, thread.state_.get(), stackSize, priority, -2, false);
    }
    if (!thread.handle_)
        thread.state_.reset();
    return thread;
}

void Thread::join()
{
    if (!handle_)
        return;
    threadJoin(handle_, U64_MAX);
    threadFree(handle_);
    handle_ = nullptr;
    state_.reset();
}

void sleepMs(u32 milliseconds)
{
    svcSleepThread(static_cast<s64>(milliseconds) * 1000000LL);
}

} // namespace tz

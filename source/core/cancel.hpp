// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include <atomic>
#include <memory>

namespace tz
{

// Shared between a job and whoever started it. Cancelling doesn't stop a job by force: the job checks it between
// steps, and results of a cancelled job are dropped instead of delivered.
class CancelToken
{
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    bool cancelled() const { return flag_->load(std::memory_order_relaxed); }
    void cancel() const { flag_->store(true, std::memory_order_relaxed); }
    // A pointer for APIs that poll a flag (curl transfers).
    const std::atomic<bool>* flag() const { return flag_.get(); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

} // namespace tz

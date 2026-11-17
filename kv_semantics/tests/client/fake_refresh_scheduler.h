/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>
#include "view/refresh_scheduler.h"

namespace kv::test {

// Runs the refresh callback synchronously on every trigger, coalescing concurrent
// triggers like the resident scheduler. Tests inject it through the client factory so
// they control when a refresh happens.
class AutoRefreshScheduler final : public RefreshScheduler {
public:
    Status Start(std::chrono::milliseconds interval, RefreshCallback callback) override
    {
        interval_ = interval;
        callback_ = std::move(callback);
        ++startCount_;
        return Status::OK();
    }

    void Stop() override { ++stopCount_; }
    void TriggerByTimer() override { (void)RefreshOnce(); }

    void TriggerByIoFailure(const Status&) override { (void)RefreshOnce(); }

    Status RefreshNow() override { return RefreshOnce(); }

    // Coalesces triggers like the real scheduler: while one refresh runs, further
    // triggers are dropped instead of starting a concurrent refresh.
    Status RefreshOnce()
    {
        std::unique_lock<std::mutex> lock{mutex_, std::try_to_lock};
        if (!lock.owns_lock()) { return Status::OK(); }
        return callback_ ? callback_() : Status::OK();
    }

    std::chrono::milliseconds interval_{0};
    std::size_t startCount_{0};
    std::size_t stopCount_{0};
    RefreshCallback callback_;
    std::mutex mutex_;
};

inline RefreshSchedulerFactory MakeAutoRefreshSchedulerFactory()
{
    return [] {
        return std::static_pointer_cast<RefreshScheduler>(std::make_shared<AutoRefreshScheduler>());
    };
}

}  // namespace kv::test

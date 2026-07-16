#pragma once

#include "libuv_allocator.h"

#include <uv.h>

#include <cstddef>
#include <deque>
#include <type_traits>
#include <utility>

template <typename T>
class TqUvRelayEventQueue final {
public:
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    using MutexInitFn = int (*)(uv_mutex_t*);
    using BeforePopFn = void (*)();
#endif
    struct PushResult {
        bool Accepted{false};
        bool ShouldWake{false};
    };

    explicit TqUvRelayEventQueue(
        std::size_t capacity
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
        , MutexInitFn mutexInit = nullptr,
        BeforePopFn beforePop = nullptr
#endif
    )
        : Capacity_(capacity == 0 ? 1 : capacity)
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
        , BeforePop_(beforePop)
#endif
    {
        if (!TqUvInstallAllocator()) {
            return;
        }
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
        const auto initialize = mutexInit != nullptr ? mutexInit : &uv_mutex_init;
#else
        const auto initialize = &uv_mutex_init;
#endif
        const int status = initialize(&Mutex_);
        Initialized_ = status == 0;
    }

    ~TqUvRelayEventQueue() {
        if (Initialized_) {
            uv_mutex_destroy(&Mutex_);
        }
    }

    TqUvRelayEventQueue(const TqUvRelayEventQueue&) = delete;
    TqUvRelayEventQueue& operator=(const TqUvRelayEventQueue&) = delete;

    PushResult TryPush(T value) {
        if (!Initialized_) {
            return {};
        }
        uv_mutex_lock(&Mutex_);
        if (Queue_.size() >= Capacity_) {
            uv_mutex_unlock(&Mutex_);
            return {};
        }
        const bool shouldWake = Queue_.empty();
        try {
            Queue_.push_back(std::move(value));
        } catch (...) {
            uv_mutex_unlock(&Mutex_);
            return {};
        }
        uv_mutex_unlock(&Mutex_);
        return {true, shouldWake};
    }

    // Removes one item without allocating. A fault before the move leaves the
    // item in the queue so a later async/safety-timer pass can retry it.
    bool TryPop(T& output) noexcept {
        static_assert(std::is_nothrow_move_assignable_v<T>);
        if (!Initialized_) {
            return false;
        }
        uv_mutex_lock(&Mutex_);
        if (Queue_.empty()) {
            uv_mutex_unlock(&Mutex_);
            return false;
        }
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
        try {
            if (BeforePop_ != nullptr) {
                BeforePop_();
            }
        } catch (...) {
            uv_mutex_unlock(&Mutex_);
            return false;
        }
#endif
        output = std::move(Queue_.front());
        Queue_.pop_front();
        uv_mutex_unlock(&Mutex_);
        return true;
    }

    std::size_t Size() const {
        if (!Initialized_) {
            return 0;
        }
        uv_mutex_lock(&Mutex_);
        const auto size = Queue_.size();
        uv_mutex_unlock(&Mutex_);
        return size;
    }

    bool Empty() const {
        return Size() == 0;
    }

    std::size_t Capacity() const noexcept {
        return Capacity_;
    }

    bool Initialized() const noexcept {
        return Initialized_;
    }

private:
    mutable uv_mutex_t Mutex_{};
    std::deque<T> Queue_;
    std::size_t Capacity_{1};
    bool Initialized_{false};
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    BeforePopFn BeforePop_{nullptr};
#endif
};

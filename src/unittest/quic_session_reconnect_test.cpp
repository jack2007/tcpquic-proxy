#include "quic_session.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

static int TestFixedDelayRetrySchedulesAndRestartsSlot() {
    QuicClientSession session;
    std::atomic<int> scheduled{0};
    std::atomic<int> startCalls{0};
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(1);
    session.SetReconnectTestHooks(QuicClientSession::ReconnectTestHooks{
        [&](size_t index) {
            if (index != 0) {
                return false;
            }
            startCalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }});
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds delay, std::function<void()> task) {
            if (delay != std::chrono::milliseconds(100)) {
                return false;
            }
            scheduled.fetch_add(1, std::memory_order_relaxed);
            retryTask = std::move(task);
            return true;
        });

    session.ScheduleStartRetryForTest(0);
    if (scheduled.load(std::memory_order_relaxed) != 1 || !retryTask) {
        return 10;
    }
    retryTask();
    if (startCalls.load(std::memory_order_relaxed) != 1) {
        return 11;
    }
    session.Stop();
    return 0;
}

static int TestDelayedRetryDropsAfterStop() {
    QuicClientSession session;
    std::atomic<int> startCalls{0};
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(1);
    session.SetReconnectTestHooks(QuicClientSession::ReconnectTestHooks{
        [&](size_t) {
            startCalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }});
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds delay, std::function<void()> task) {
            if (delay != std::chrono::milliseconds(100)) {
                return false;
            }
            retryTask = std::move(task);
            return true;
        });

    session.ScheduleStartRetryForTest(0);
    if (!retryTask) {
        return 20;
    }
    session.Stop();
    retryTask();
    return startCalls.load(std::memory_order_relaxed) == 0 ? 0 : 21;
}

static int TestFixedDelayRetryCoalescesDuplicates() {
    QuicClientSession session;
    std::atomic<int> scheduled{0};
    std::atomic<int> startCalls{0};
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(1);
    session.SetReconnectTestHooks(QuicClientSession::ReconnectTestHooks{
        [&](size_t) {
            startCalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }});
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds, std::function<void()> task) {
            scheduled.fetch_add(1, std::memory_order_relaxed);
            retryTask = std::move(task);
            return true;
        });

    session.ScheduleStartRetryForTest(0);
    session.ScheduleStartRetryForTest(0);
    if (scheduled.load(std::memory_order_relaxed) != 1 || !retryTask) {
        return 30;
    }
    retryTask();
    if (startCalls.load(std::memory_order_relaxed) != 1) {
        return 31;
    }
    session.Stop();
    return 0;
}

static int TestRejectedSchedulerAllowsLaterRetry() {
    QuicClientSession session;
    std::atomic<int> rejectedCalls{0};
    std::atomic<int> acceptedCalls{0};
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(1);
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds, std::function<void()>) {
            rejectedCalls.fetch_add(1, std::memory_order_relaxed);
            return false;
        });
    session.ScheduleStartRetryForTest(0);
    if (rejectedCalls.load(std::memory_order_relaxed) != 1) {
        return 40;
    }

    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds, std::function<void()> task) {
            acceptedCalls.fetch_add(1, std::memory_order_relaxed);
            retryTask = std::move(task);
            return true;
        });
    session.ScheduleStartRetryForTest(0);
    if (acceptedCalls.load(std::memory_order_relaxed) != 1 || !retryTask) {
        return 41;
    }
    session.Stop();
    return 0;
}

int main() {
    if (int rc = TestFixedDelayRetrySchedulesAndRestartsSlot()) return rc;
    if (int rc = TestDelayedRetryDropsAfterStop()) return rc;
    if (int rc = TestFixedDelayRetryCoalescesDuplicates()) return rc;
    if (int rc = TestRejectedSchedulerAllowsLaterRetry()) return rc;
    return 0;
}

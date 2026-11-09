#pragma once

#include <atomic>
#include <mutex>

namespace wiiu::fsa {
enum class RouteKind { Unknown, Checking, Direct, System };

struct RouteStatus {
    RouteKind Route;
    const char *Reason;
};

class RouteController {
public:
    bool TryBeginProbe() {
        RouteKind expected = RouteKind::Unknown;
        return Current.compare_exchange_strong(expected, RouteKind::Checking, std::memory_order_acq_rel);
    }

    void CompleteProbe(const char *reason) {
        std::lock_guard<std::mutex> lock(Mutex);
        if (Current.load(std::memory_order_relaxed) != RouteKind::Checking) return;
        FailureReason = reason;
        Current.store(reason ? RouteKind::System : RouteKind::Direct, std::memory_order_release);
    }

    void CancelProbe(const char *reason) {
        std::lock_guard<std::mutex> lock(Mutex);
        if (Current.load(std::memory_order_relaxed) != RouteKind::Checking) return;
        FailureReason = reason;
        Current.store(RouteKind::System, std::memory_order_release);
    }

    void DisableDirect(const char *reason) {
        std::lock_guard<std::mutex> lock(Mutex);
        FailureReason = reason;
        Current.store(RouteKind::System, std::memory_order_release);
    }

    RouteKind GetRoute() const { return Current.load(std::memory_order_acquire); }

    bool IsDirect() const { return GetRoute() == RouteKind::Direct; }

    RouteStatus GetStatus() const {
        std::lock_guard<std::mutex> lock(Mutex);
        return {Current.load(std::memory_order_acquire), FailureReason};
    }

private:
    mutable std::mutex Mutex;
    std::atomic<RouteKind> Current{RouteKind::Unknown};
    const char *FailureReason = "not tried";
};
} // namespace wiiu::fsa

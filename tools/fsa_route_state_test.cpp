#include <atomic>
#include <cassert>
#include <cstring>
#include <thread>
#include <vector>

#include "../port/wiiu/fsa_route_state.hpp"

int main() {
    using namespace wiiu::fsa;

    RouteController passed;
    assert(passed.GetRoute() == RouteKind::Unknown);
    assert(passed.TryBeginProbe());
    assert(!passed.TryBeginProbe());
    passed.CompleteProbe(nullptr);
    assert(passed.IsDirect());
    passed.DisableDirect("write verification failed");
    assert(!passed.IsDirect());
    assert(std::strcmp(passed.GetStatus().Reason, "write verification failed") == 0);
    passed.CompleteProbe(nullptr);
    assert(!passed.IsDirect());

    RouteController failed;
    assert(failed.TryBeginProbe());
    failed.CompleteProbe("stat differs");
    assert(failed.GetRoute() == RouteKind::System);
    assert(std::strcmp(failed.GetStatus().Reason, "stat differs") == 0);
    assert(!failed.TryBeginProbe());

    RouteController cancelled;
    assert(cancelled.TryBeginProbe());
    cancelled.CancelProbe("route probe timed out");
    cancelled.CompleteProbe(nullptr);
    assert(cancelled.GetRoute() == RouteKind::System);
    assert(std::strcmp(cancelled.GetStatus().Reason, "route probe timed out") == 0);

    RouteController completed;
    assert(completed.TryBeginProbe());
    completed.CompleteProbe(nullptr);
    completed.CancelProbe("route probe timed out");
    assert(completed.IsDirect());

    for (int round = 0; round < 100; ++round) {
        RouteController competing;
        assert(competing.TryBeginProbe());
        std::atomic<bool> go{false};
        std::thread completer([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            competing.CompleteProbe(nullptr);
        });
        std::thread canceller([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            competing.CancelProbe("route probe timed out");
        });
        go.store(true, std::memory_order_release);
        completer.join();
        canceller.join();
        const RouteStatus status = competing.GetStatus();
        assert(status.Route == RouteKind::Direct || status.Route == RouteKind::System);
        assert(status.Route == RouteKind::Direct ? status.Reason == nullptr : std::strcmp(status.Reason, "route probe timed out") == 0);
        competing.DisableDirect("write verification failed");
        assert(competing.GetRoute() == RouteKind::System);
        assert(std::strcmp(competing.GetStatus().Reason, "write verification failed") == 0);
    }

    for (int round = 0; round < 50; ++round) {
        RouteController concurrent;
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::atomic<int> owners{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 16; ++t) {
            threads.emplace_back([&] {
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                if (concurrent.TryBeginProbe()) owners.fetch_add(1, std::memory_order_relaxed);
            });
        }
        while (ready.load(std::memory_order_acquire) != 16) std::this_thread::yield();
        go.store(true, std::memory_order_release);
        for (auto &thread : threads) thread.join();
        assert(owners.load() == 1);
        assert(concurrent.GetRoute() == RouteKind::Checking);
        concurrent.CompleteProbe(nullptr);
        assert(concurrent.IsDirect());
    }
}

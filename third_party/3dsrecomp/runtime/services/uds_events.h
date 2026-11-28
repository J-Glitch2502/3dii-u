// Bridge the standalone UDS engine to guest events without taking the kernel
// mutex from a networking callback. Publish() is called with that mutex held.
#pragma once
#include "../kernel.h"
#include "azl/uds_engine.hpp"
#include <condition_variable>
#include <chrono>
#include <thread>

namespace kernel {
class UdsEvents final : public azl::KernelOps {
    struct Slot {
        std::shared_ptr<Event> guest;
        bool signaled = false;
        bool closed = false;
        bool guest_signal = false;
        u64 generation = 0;
        u64 published = 0;
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::map<u32, std::shared_ptr<Slot>> events;
    u32 next = 1;

public:
    u32 CreateEvent() override {
        auto slot = std::make_shared<Slot>();
        // Construct directly: network callbacks must not touch kernel globals.
        slot->guest = std::make_shared<Event>();
        slot->guest->name = "UDS event";
        std::lock_guard<std::mutex> lock(mutex);
        if (!next) return 0;
        const u32 id = next++;
        events.emplace(id, std::move(slot));
        return id;
    }
    u32 DuplicateForGame(u32 id) override {
        std::lock_guard<std::mutex> lock(mutex);
        return events.count(id) ? id : 0;
    }
    void SignalEvent(u32 id) override {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = events.find(id);
        if (it == events.end()) return;
        it->second->signaled = true;
        it->second->guest_signal = true;
        ++it->second->generation;
        changed.notify_all();
    }
    void ClearEvent(u32 id) override {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = events.find(id);
        if (it != events.end()) {
            it->second->signaled = false;
            it->second->guest_signal = false;
            ++it->second->generation;
        }
    }
    bool WaitEvent(u32 id, s64 timeout) override {
        std::unique_lock<std::mutex> lock(mutex);
        const auto it = events.find(id);
        if (it == events.end()) return false;
        const auto slot = it->second;
        const auto ready = [&] { return slot->signaled || slot->closed; };
        if (timeout < 0) changed.wait(lock, ready);
        else if (!ready()) changed.wait_for(lock, std::chrono::nanoseconds(timeout), ready);
        if (slot->closed || !slot->signaled) return false;
        slot->signaled = false;
        return true;
    }
    void CloseEvent(u32 id) override {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = events.find(id);
        if (it == events.end()) return;
        it->second->closed = true;
        events.erase(it);
        changed.notify_all();
    }
    void SleepNs(s64 ns) override {
        if (ns > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }
    std::shared_ptr<Event> GuestEvent(u32 id) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = events.find(id);
        return it == events.end() ? nullptr : it->second->guest;
    }
    void Publish() { // kmutex held
        // A guest waking from signal() cannot run until kmutex is released.
        std::lock_guard<std::mutex> lock(mutex);
        for (auto &[id, slot] : events) {
            if (slot->published != slot->generation) {
                slot->published = slot->generation;
                if (slot->guest_signal) slot->guest->signal();
                else slot->guest->clear();
            }
        }
    }
};
}

// Experimental, same-process checkpoints. Host pointers never go into a
// portable state file. Normal builds compile out the coordination entirely.
#pragma once
#ifdef RECOMP_RESUMABLE
#include <atomic>
#include <functional>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <mutex>
#include <set>
#include <vector>
#include "cpu.h"
#include "state_archive.h"
#ifdef __WIIU__
#include <coreinit/cache.h>
#endif
namespace checkpoint {
// Normal work enters concurrently. Only capture/restore freezes all readers.
class Gate {
    std::atomic<u32> readers{0};
    std::atomic<bool> paused{false};
    std::mutex writer;
public:
    void lock();
    void unlock();
    void freeze();
    void thaw();
};
Gate &gate();
using Scope = std::lock_guard<Gate>;
struct ExclusiveScope {
    Gate &value;
    explicit ExclusiveScope(Gate &g) : value(g) { value.freeze(); }
    ~ExclusiveScope() { value.thaw(); }
    ExclusiveScope(const ExclusiveScope &) = delete;
    ExclusiveScope &operator=(const ExclusiveScope &) = delete;
};
struct Image {
    struct Bytes {
        u8 *data = nullptr;
        size_t size = 0;
        static inline std::atomic<size_t> live{0}, peak{0};
        ~Bytes() { if (data) { std::free(data); live.fetch_sub(size); } }
    };
    std::shared_ptr<void> archive_graph;
    Archive *archive = nullptr; // null retains the existing fast session-only path
    std::vector<std::function<void()>> restore;
    std::vector<std::function<bool()>> validate;
    std::vector<std::pair<size_t, const char *>> boundaries;
    size_t bytes = 0, max_bytes = 192u << 20;
    bool ok = true;
    std::set<const void *> objects;
    Image() { restore.reserve(4096); validate.reserve(512); }
    template<class T> void take(T &value) {
        if (!ok) return;
        if (archive) {
            auto saved = std::make_shared<T>(value);
            archive->io(*saved);
            if (!archive->ok) { ok = false; return; }
            if (archive->reading) {
                restore.push_back([&value, saved] { value = *saved; });
                return;
            }
        }
        restore.push_back([&value, saved = value] { value = saved; });
    }
    template<class T, size_t N> void take(T (&value)[N]) { for (auto &x : value) take(x); }
    template<class... T> void fields(T &...value) { (take(value), ...); }
    std::shared_ptr<Bytes> copy_bytes(const void *source, size_t size) {
        if (!ok || bytes > max_bytes || size > max_bytes - bytes) { ok = false; return {}; }
        auto saved = std::make_shared<Bytes>();
        saved->data = (u8 *)std::malloc(size);
        if (!saved->data) { ok = false; return {}; }
        saved->size = size;
        const size_t current = Bytes::live.fetch_add(size) + size;
        size_t peak = Bytes::peak.load();
        while (current > peak && !Bytes::peak.compare_exchange_weak(peak, current)) {}
        if (source) memcpy(saved->data, source, size);
        bytes += size;
        return saved;
    }
    void memory(void *ptr, size_t size) {
        if (archive && archive->buffering()) {
            if (bytes > max_bytes || size > max_bytes-bytes) { ok=false;return; }
            for (size_t at=0; ok && at<size;) {
                size_t part=std::min<size_t>(size-at,64u<<10);
                archive->raw((u8*)ptr+at,part);
                if (!archive->ok) { ok=false; return; }
                auto saved=archive->staged.back();bytes+=part;
                restore.push_back([target=(u8*)ptr+at,saved,part] {
                    memcpy(target,saved->data(),part);
#ifdef __WIIU__
                    DCFlushRange(target,part);
#endif
                });
                at+=part;
            }
            return;
        }
        if (archive && !archive->reading) {
            archive->raw(ptr, size);
            if (!archive->ok) { ok = false; return; }
        }
        // Small bounded allocations avoid requiring a single large free heap
        // extent after gameplay has fragmented MEM2.
        for (size_t at = 0; ok && at < size;) {
            const size_t part = std::min<size_t>(size - at, 64u << 10);
            auto saved = copy_bytes(archive && archive->reading ? nullptr : (u8 *)ptr + at, part);
            if (!saved) return;
            if (archive && archive->reading) {
                archive->raw(saved->data, part);
                if (!archive->ok) { ok = false; return; }
            }
            restore.push_back([target = (u8 *)ptr + at, saved, part] {
                memcpy(target, saved->data, part);
#ifdef __WIIU__
                DCFlushRange(target, part); // publish rewound guest data to the GPU/DMA consumers
#endif
            });
            at += part;
        }
    }
};

bool kernel_capture(Image &image);
bool kernel_persist(Image &image);
bool services_capture(Image &image);
bool gpu_capture(Image &image);
void audio_capture(Image &image);
void request(unsigned command);  // 1 RAM save, 2 automatic load, 3 RAM discard, 4 persistent save, 5 file load
bool request_memory32(u32 address, bool write, u32 expected = 0, u32 replacement = 0);
void pump();                     // GPU thread only, outside native guest execution
bool has_state();
bool saving_to_disk();
void finish_pending_write();
const char *result_message();
void set_heap_checks(bool enabled);
void initialize_persistent();
}
#else
namespace checkpoint { struct Scope { template<class T> explicit Scope(T) {} }; inline int gate() { return 0; } }
#endif

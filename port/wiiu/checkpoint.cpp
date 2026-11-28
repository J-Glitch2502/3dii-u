#include "checkpoint.h"
#ifdef RECOMP_RESUMABLE
#include <atomic>
#include <chrono>
#include <cerrno>
#include <thread>
#include <coreinit/debug.h>
#include <gx2/event.h>
#ifdef __WIIU__
#include <coreinit/memexpheap.h>
#include <coreinit/thread.h>
#endif
#include "kernel.h"
#include "loader.h"
#include "gpu/pica.h"

namespace wiiu { const std::string &data_dir(); void save_flush(bool); int flush_checkpoint_storage(); void show_state_loading(bool); bool ensure_save_capacity(const std::string &,u64); }

namespace kernel {
void checkpoint_services(checkpoint::Image &);
void checkpoint_hid(checkpoint::Image &);
void checkpoint_dsp(checkpoint::Image &);
void checkpoint_gsp(checkpoint::Image &);
void checkpoint_fs(checkpoint::Image &);
bool checkpoint_apt(checkpoint::Image &);
}
namespace audio { void checkpoint_audio(checkpoint::Image &); void pause(bool); void checkpoint_clear_output(); }
namespace mem { void checkpoint_allocator(checkpoint::Image &); }
extern "C" {
volatile u32 g_3dii_state_command;
volatile u32 g_3dii_state_status;
volatile u32 g_3dii_state_bytes;
}
namespace checkpoint {
static std::unique_ptr<Image> slot;
static std::atomic<bool> slot_available{false};
Gate &gate() { static auto *value = new Gate; return *value; }
void Gate::lock() {
    for (;;) {
        while (paused.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::microseconds(100));
        readers.fetch_add(1, std::memory_order_seq_cst);
        if (!paused.load(std::memory_order_seq_cst)) return;
        readers.fetch_sub(1, std::memory_order_release);
    }
}
void Gate::unlock() { readers.fetch_sub(1, std::memory_order_release); }
void Gate::freeze() {
    writer.lock();
    paused.store(true, std::memory_order_seq_cst);
    rt_checkpoint_requested = 1;
    while (readers.load(std::memory_order_seq_cst)) std::this_thread::sleep_for(std::chrono::microseconds(100));
}
void Gate::thaw() {
    rt_checkpoint_requested = 0;
    paused.store(false, std::memory_order_release);
    writer.unlock();
}

bool services_capture(Image &im) {
    kernel::checkpoint_services(im);
    kernel::checkpoint_hid(im); kernel::checkpoint_dsp(im);
    kernel::checkpoint_gsp(im); kernel::checkpoint_fs(im);
    mem::checkpoint_allocator(im);
    return kernel::checkpoint_apt(im);
}
void audio_capture(Image &im) { audio::checkpoint_audio(im); }
static bool heap_checks_enabled;
void set_heap_checks(bool enabled) { heap_checks_enabled = enabled; }
static bool check_shared_heap(const char *phase) {
    if (!heap_checks_enabled) return true;
#ifdef __WIIU__
    auto *heap = reinterpret_cast<MEMExpHeap *>(0x80000000u);
    if (heap->header.tag != MEM_EXPANDED_HEAP_TAG) {
        OSReport("[state-heap] %s: mapped root unavailable\n", phase);
        return true;
    }
    // Aroma's MEMCheckExpHeap hook takes the same external mutex as every
    // mapped-root allocation/free. No racing manual walk of allocator links.
    OSReport("[state-heap] checking %s\n", phase);
    const bool ok = MEMCheckExpHeap(&heap->header, MEM_EXP_HEAP_CHECK_FLAGS_LOG_ERRORS);
    OSReport("[state-heap] %s: %s\n", phase, ok ? "OK" : "DAMAGED");
    return ok;
#else
    (void)phase; return true;
#endif
}
static std::atomic<bool> disk_available{false};
static u64 initial_code_hash;
extern "C" const char g_build_stamp[] __attribute__((weak));
static u64 state_hash(const void *data, size_t size, u64 hash = 14695981039346656037ull) {
    const auto *p = static_cast<const u8 *>(data);
    for (size_t i=0;i<size;++i) hash = (hash ^ p[i]) * 1099511628211ull;
    return hash;
}
static std::string state_path() {
#ifdef __WIIU__
    char name[64]; snprintf(name,sizeof name,"/checkpoint-%016llx.bin",(unsigned long long)g_game.program_id);
    return wiiu::data_dir()+name;
#else
    return g_save_base+"/checkpoint.bin";
#endif
}
struct DiskHeader {
    char magic[8];
    u32 version, endian;
    u64 title, build, code, size, checksum;
};
static DiskHeader state_header() {
    DiskHeader h{}; memcpy(h.magic,"3DSTATE",7);h.version=1;h.endian=0x12345678;
    h.title=g_game.program_id;h.code=initial_code_hash;
    const char *build=g_build_stamp ? g_build_stamp : "host fixture";
    h.build=state_hash(build,strlen(build));
    return h;
}
void initialize_persistent() {
    initial_code_hash=state_hash(gptr(g_game.text_addr),g_game.text_size);
    initial_code_hash=state_hash(gptr(g_game.ro_addr),g_game.ro_size,initial_code_hash);
    initial_code_hash=state_hash(gptr(g_game.data_addr),g_game.data_size,initial_code_hash);
    FILE *f=fopen(state_path().c_str(),"rb");
    disk_available.store(f!=nullptr,std::memory_order_release);
    if(f)fclose(f);
}
static bool persistent_capture(Image &im) {
    auto phase = [&](const char *name, bool ok) {
        if (!ok || !im.ok || !im.archive->ok) {
            LOG("[state-file] %s failed at payload byte %llu: %s\n", name, (unsigned long long)im.archive->bytes,
                im.archive->error ? im.archive->error : "capture rejected");
            return false;
        }
        return true;
    };
    if (!phase("kernel",kernel_persist(im))) return false;
    if (!phase("services",services_capture(im))) return false;
    audio_capture(im);if (!phase("audio",im.ok)) return false;
    return phase("GPU",gpu_capture(im));
}
static void detach_archive(Image &im) {
    im.archive_graph.reset(); // adapters refer to the stack-owned stream
    im.archive=nullptr;
}
static std::atomic<bool> disk_writing{false};
static std::atomic<int> disk_result{0};
static std::atomic<bool> disk_cancel{false};
bool saving_to_disk() { return disk_writing.load(std::memory_order_acquire); }
static std::thread disk_worker;
struct JoinDiskWorker { ~JoinDiskWorker() { if(disk_worker.joinable())disk_worker.join(); } };
static JoinDiskWorker join_disk_worker;
void finish_pending_write() {
    // Called explicitly before filesystem/log shutdown; _exit skips destructors.
    if(disk_worker.joinable())disk_worker.join();
}
static std::unique_ptr<Image> save_persistent() {
    if (disk_worker.joinable()) disk_worker.join();
#ifdef __WIIU__
    wiiu::save_flush(true);
#endif
    auto im=std::make_unique<Image>();
    Archive stream(nullptr,false);im->archive=&stream;
    const bool captured=persistent_capture(*im);
    detach_archive(*im);
    if (!captured || !stream.ok || !im->ok) return {};
    auto h=state_header();h.size=stream.bytes;
    auto path=state_path();
    disk_cancel=false;disk_result=0;disk_writing.store(true,std::memory_order_release);
    disk_worker=std::thread([chunks=std::move(stream.staged),path,h]() mutable {
#ifdef CHECKPOINT_TEST_WORKER_GATE
        extern void test_writer_gate();test_writer_gate();
#endif
#ifdef __WIIU__
        OSSetThreadPriority(OSGetCurrentThread(),16); // background I/O must not inherit the GPU worker's priority
#endif
        const auto started=std::chrono::steady_clock::now();
        const auto temporary=path+".new",backup=path+".bak";
        const char *phase="open";u64 written=0,flushed=0;int io_error=0;
#ifdef __WIIU__
        if(!wiiu::ensure_save_capacity(temporary,sizeof h+h.size)) {
            LOG("[state-file] save storage could not grow for %llu bytes; current state retained\n",(unsigned long long)(sizeof h+h.size));
            disk_result.store(-1,std::memory_order_release);disk_writing.store(false,std::memory_order_release);return;
        }
#endif
        FILE *f=fopen(temporary.c_str(),"wb");bool ok=f!=nullptr;
        if(!ok)io_error=errno;
        if (f) {
            setvbuf(f,nullptr,_IOFBF,256u<<10);
            phase="header";ok=fwrite(&h,1,sizeof h,f)==sizeof h;
            if(!ok)io_error=errno;
            h.checksum=14695981039346656037ull;
            for (auto &chunk:chunks) {
                if (disk_cancel.load(std::memory_order_acquire)) {ok=false;break;}
                if (!ok) break;
                h.checksum=state_hash(chunk->data(),chunk->size(),h.checksum);
                phase="payload";const size_t count=fwrite(chunk->data(),1,chunk->size(),f);written+=count;
                ok=count==chunk->size();if(!ok)io_error=errno;
#ifdef __WIIU__
                if(ok && written-flushed >= (8u<<20)) {
                    phase="journal flush";ok=fflush(f)==0;if(!ok)io_error=errno;
                    if(ok){const int result=wiiu::flush_checkpoint_storage();ok=result>=0;if(!ok)io_error=result;}
                    flushed=written;
                }
#endif
            }
            if(ok){phase="final header";ok=!fseek(f,0,SEEK_SET)&&fwrite(&h,1,sizeof h,f)==sizeof h&&!fflush(f);if(!ok)io_error=errno;}
            const int closed=fclose(f);if(closed && ok){phase="close";io_error=errno;ok=false;}
        }
        if(disk_cancel.load(std::memory_order_acquire))ok=false;
        if(ok) {
            remove(backup.c_str());const bool backed=rename(path.c_str(),backup.c_str())==0;
            if(rename(temporary.c_str(),path.c_str())) {const int error=errno;if(backed)rename(backup.c_str(),path.c_str());phase="publish";io_error=error;ok=false;}
        }
#ifdef __WIIU__
        if(ok){phase="storage commit";const int result=wiiu::flush_checkpoint_storage();ok=result>=0;if(!ok)io_error=result;}
#endif
        if(!ok && !disk_cancel.load())LOG("[state-file] background write failed: phase %s, error %d, payload %llu/%llu, path %s\n",phase,io_error,(unsigned long long)written,(unsigned long long)h.size,path.c_str());
        if(!ok)remove(temporary.c_str());
        LOG("[state-time] background save: %llu bytes, %llu ms, %s\n",(unsigned long long)h.size,(unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count(),ok ? "complete" : disk_cancel.load() ? "superseded" : "failed");
        disk_result.store(ok ? 1 : disk_cancel.load(std::memory_order_acquire) ? 2 : -1,std::memory_order_release);
        disk_writing.store(false,std::memory_order_release);
    });
    return im;
}
static std::unique_ptr<Image> load_persistent() {
    const auto started=std::chrono::steady_clock::now();
    FILE *f=fopen(state_path().c_str(),"rb");
    if(!f){LOG("[state-file] no persistent state\n");return {};}
    setvbuf(f,nullptr,_IOFBF,256u<<10);
    DiskHeader h{}, expected=state_header();
    bool ok=fread(&h,1,sizeof h,f)==sizeof h && !memcmp(h.magic,expected.magic,8) && h.version==expected.version && h.endian==expected.endian &&
            h.title==expected.title && h.build==expected.build && h.code==expected.code && h.size<=Archive::limit;
    if(!ok){LOG("[state-file] incompatible/truncated header; game unchanged\n");fclose(f);return {};}
    auto im=std::make_unique<Image>();Archive stream(f,true);stream.payload_limit=h.size;im->archive=&stream;
    ok=persistent_capture(*im)&&stream.bytes==h.size&&stream.hash==h.checksum&&fgetc(f)==EOF&&!ferror(f);
    detach_archive(*im);fclose(f);
    if(!ok){LOG("[state-file] load refused before commit: %s\n",stream.error ? stream.error : "state reconstruction failed");return {};}
    for(auto &validate:im->validate)if(!validate()){LOG("[state-file] validation failed before commit\n");return {};}
    LOG("[state-file] REBUILT persistent checkpoint: %u bytes of RAM/resources\n",(u32)im->bytes);
    LOG("[state-time] persistent %s: %llu bytes, %llu ms, one payload pass\n", stream.reading ? "load" : "save", (unsigned long long)stream.bytes, (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count());
    return im;
}

bool has_state() { return slot_available.load(std::memory_order_acquire) || disk_available.load(std::memory_order_acquire); }
const char *result_message() {
    if (saving_to_disk()) return "SAVING TO STORAGE...";
    if (disk_result.load()==-1) return "STORAGE SAVE FAILED; RAM STATE RETAINED";
    switch (g_3dii_state_status) {
    case 1: return "SESSION STATE SAVED";
    case 2: return "SESSION STATE LOADED";
    case 4: return disk_result.load()==1 ? "STATE SAVED TO STORAGE AND RAM" : "STATE CAPTURED IN RAM";
    case 5: return "STATE LOADED FROM FILE";
    case 0x80000006: return "STATE FILE FAILED: SEE DIAGNOSTIC LOG";
    case 0x80000002: return "SAVE FAILED: UNSUPPORTED SERVICE OR APPLET";
    case 0x80000003: return "LOAD REFUSED: EMPTY SLOT OR CHANGED SAVE/RESOURCES";
    case 0x80000005: return "STATE OPERATION REFUSED: SHARED HEAP DAMAGED";
    case 0x80000004: return "SAVE FAILED: NOT ENOUGH SNAPSHOT MEMORY";
    default: return nullptr;
    }
}
void request(unsigned command) { __atomic_store_n(&g_3dii_state_command, command, __ATOMIC_RELEASE); }
namespace {
struct DebugMemory32 { u32 address=0, expected=0, replacement=0; bool write=false, pending=false; };
std::mutex debug_memory_mutex;
DebugMemory32 debug_memory;
}
bool request_memory32(u32 address, bool write, u32 expected, u32 replacement) {
    if (address & 3) return false;
    std::lock_guard<std::mutex> lock(debug_memory_mutex);
    if (debug_memory.pending) return false;
    debug_memory = {address, expected, replacement, write, true};
    return true;
}
static void pump_debug_memory() {
    DebugMemory32 job;
    { std::lock_guard<std::mutex> lock(debug_memory_mutex); job=debug_memory; debug_memory.pending=false; }
    if (!job.pending) return;
    // Same barrier as a checkpoint: no guest or renderer thread races the edit.
    ExclusiveScope hold(gate());
    if (!kernel::guest_readable(job.address,4) || (job.write && !kernel::guest_writable(job.address,4))) {
        LOG("[debug-ram] %08x refused: guest mapping/permissions\n",job.address); return;
    }
    const u32 old=r32(job.address);
    if (!job.write) { LOG("[debug-ram] %08x = %08x\n",job.address,old); return; }
    if (old!=job.expected) {
        LOG("[debug-ram] %08x unchanged: expected %08x actual %08x\n",job.address,job.expected,old); return;
    }
    w32(job.address,job.replacement);
#ifdef __WIIU__
    DCFlushRange(gptr(job.address),4);
    if (const u32 physical=mem::vaddr_to_paddr(job.address)) gpu::metal_guest_written(physical,4,true);
#endif
    LOG("[debug-ram] %08x changed %08x -> %08x\n",job.address,old,job.replacement);
}
void pump() {
    pump_debug_memory();
    if (!saving_to_disk() && disk_result.load(std::memory_order_acquire)==1) disk_available.store(true,std::memory_order_release);
    const unsigned command = __atomic_exchange_n(&g_3dii_state_command, 0, __ATOMIC_ACQ_REL);
    if (!command) return;
    // A RAM restore only reads immutable snapshot buffers shared with the writer.
    // Disk reconstruction and replacement captures still wait for publication.
    if (saving_to_disk() && !(command==2 && slot)) {
        if (command==4) {
            disk_cancel.store(true,std::memory_order_release);
            unsigned empty=0;
            __atomic_compare_exchange_n(&g_3dii_state_command,&empty,4,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
        } else if (command==2 || command==5) {
            unsigned empty=0;
            __atomic_compare_exchange_n(&g_3dii_state_command,&empty,command,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
        }
        return;
    }
    ExclusiveScope lock(gate());
    if (command == 3) { slot.reset(); slot_available.store(false, std::memory_order_release); g_3dii_state_status = 0; g_3dii_state_bytes = 0; return; }
    if (!check_shared_heap("before checkpoint operation")) {
        g_3dii_state_status = 0x80000005;
        OSReport("[state] operation refused: shared mapped heap was already damaged\n");
        return;
    }
    audio::pause(true);
    GX2DrawDone();
#ifdef __WIIU__
    struct LoadScreen {
        bool shown;
        explicit LoadScreen(bool value):shown(value){if(shown)wiiu::show_state_loading(true);}
        ~LoadScreen(){if(shown)wiiu::show_state_loading(false);}
    } load_screen((command==2 && !slot) || command==5);
#endif
    if (command == 4) {
        slot.reset();slot_available.store(false,std::memory_order_release);g_3dii_state_bytes=0;
        slot=save_persistent();
        slot_available.store(bool(slot),std::memory_order_release);
        g_3dii_state_status=slot ? 4 : 0x80000006;
        if(slot)g_3dii_state_bytes=slot->bytes;
    } else if (command == 1) {
        disk_result=0;
        // One physical RAM slot. Keeping the old 153 MiB image while building
        // its replacement exhausted the console on the second save.
        slot.reset();
        slot_available.store(false, std::memory_order_release);
        g_3dii_state_bytes = 0;
        auto candidate = std::make_unique<Image>();
        bool captured = gpu_capture(*candidate);
        candidate->boundaries.emplace_back(candidate->restore.size(), "after GPU restore");
        if (captured) captured = kernel_capture(*candidate);
        candidate->boundaries.emplace_back(candidate->restore.size(), "after kernel and guest RAM restore");
        if (captured) captured = services_capture(*candidate);
        candidate->boundaries.emplace_back(candidate->restore.size(), "after services restore");
        if (captured) audio_capture(*candidate);
        candidate->boundaries.emplace_back(candidate->restore.size(), "after DSP restore");
        if (captured && candidate->ok) {
            g_3dii_state_bytes = candidate->bytes;
            slot = std::move(candidate);
            slot_available.store(true, std::memory_order_release);
            g_3dii_state_status = 1;
            OSReport("[state] SAVED session checkpoint: %u bytes, %u restore records; live %u, peak %u bytes\n", g_3dii_state_bytes, (u32)slot->restore.size(), (u32)Image::Bytes::live.load(), (u32)Image::Bytes::peak.load());
        } else {
            g_3dii_state_status = candidate->ok ? 0x80000002 : 0x80000004;
            OSReport("[state] save rejected: %s; slot empty, game continues\n", candidate->ok ? "unsupported kernel/service or active applet" : "checkpoint memory budget/allocation exhausted");
        }
    } else if (command == 2 || command == 5) {
        bool from_file=false;
        if (!slot || command==5) {
            slot.reset();slot_available.store(false,std::memory_order_release);g_3dii_state_bytes=0;
            slot=load_persistent();from_file=bool(slot);
            slot_available.store(bool(slot),std::memory_order_release);
            if(slot)g_3dii_state_bytes=slot->bytes;
        }
        bool valid = bool(slot);
        if (slot) for (auto &validate : slot->validate) if (!validate()) valid = false;
        if (!valid) {
            g_3dii_state_status = 0x80000003;
            OSReport("[state] load rejected before modifying state: no slot, resource changed, unsupported service used, or permanent save changed\n");
        } else {
            size_t restored = 0, boundary = 0;
            for (auto &restore : slot->restore) {
                restore(); ++restored;
                while (boundary < slot->boundaries.size() && slot->boundaries[boundary].first == restored) {
                    if (!check_shared_heap(slot->boundaries[boundary].second))
                        FATAL("checkpoint restore damaged shared heap at %s\n", slot->boundaries[boundary].second);
                    ++boundary;
                }
            }
            audio::checkpoint_clear_output();
            g_3dii_state_status = from_file ? 5 : 2;
            OSReport("[state] LOADED session checkpoint: %u bytes\n", g_3dii_state_bytes);
        }
    }
    check_shared_heap("after checkpoint operation");
    audio::pause(false);
}
}
#endif

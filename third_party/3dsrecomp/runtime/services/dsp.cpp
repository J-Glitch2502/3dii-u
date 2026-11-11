// dsp::DSP: the service side. The audio processing itself (HLE of the DSP firmware) is in runtime/audio/.
#include "../audio_iface.h"
#include "../service.h"
#include "services.h"

namespace kernel {

namespace {
struct DspState {
    std::shared_ptr<Event> semaphore = make_event(ONESHOT, "DSP semaphore");
    std::shared_ptr<Event> pipe_irq[4];   // per pipe channel (interrupt type 2)
    std::shared_ptr<Event> other_irq[2];  // interrupt types 0/1
    bool game_waits = false;              // the game has been seen waiting on the pipe event
    u32 held = 0;                         // calls in a row that found the game still busy
};
DspState dsp;
}  // namespace

void dsp_tick() {
    bool seen = true;
    {
        // A frame the game hasn't come for (it isn't waiting on the pipe's event yet) is played all the same, but kept from the game: see
        // generate_frame. Waiting for the game instead stopped the sound whenever its sound thread was late, by a file read or a busy frame:
        // gaps of 5 ms about once a second in NSMB2 (2026-11-10). The limit is for a game that polls for a later frame instead of waiting.
        Lock lk(kmutex);
        auto &ev = dsp.pipe_irq[2];
        if (ev && ev->waiters.empty() && dsp.game_waits && ++dsp.held < 100) seen = false;
        else dsp.held = 0;
        if (ev && !ev->waiters.empty()) dsp.game_waits = true;
    }
    if (!audio::tick(seen) || !seen) return;
    Lock lk(kmutex);
    static int n;
    if (n++ == 100) VLOG("[dsp] tick: pipe event %s\n", dsp.pipe_irq[2] ? dsp.pipe_irq[2]->name.c_str() : "none");
    if (dsp.pipe_irq[2]) dsp.pipe_irq[2]->signal();
    schedule_async();
}

struct Dsp : Service {
    using Service::Service;
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x0001:  // RecvData(reg)
            ipc.ok(2);
            ipc.set(2, audio::recv_data(ipc[1]));
            break;
        case 0x0002:  // RecvDataIsReady
            ipc.ok(2);
            ipc.set(2, 1);
            break;
        case 0x0007:  // SetSemaphore
        case 0x0017:  // SetSemaphoreMask
        case 0x0012:  // UnloadComponent
        case 0x0013:  // FlushDataCache
        case 0x0014:  // InvalidateDataCache
        case 0x0020:  // ForceHeadphoneOut
            ipc.ok();
            break;
        case 0x000C:  // ConvertProcessAddressFromDspDram
            ipc.ok(2);
            ipc.set(2, (ipc[1] << 1) + mem::DSP_BASE + 0x40000);
            break;
        case 0x000D: {  // WriteProcessPipe(channel, size, static buffer)
            u32 ch = ipc[1], size = ipc[2], src = ipc[4];
            audio::pipe_write(ch, gptr(src), size);
            ipc.ok();
            // a state change answers on the audio pipe, an AAC request on its own
            if ((ch == 2 || ch == 3) && dsp.pipe_irq[ch]) dsp.pipe_irq[ch]->signal();
            break;
        }
        case 0x000E:    // ReadPipe(channel, peer, size)
        case 0x0010: {  // ReadPipeIfPossible
            u32 ch = ipc[1], size = ipc[3] & 0xFFFF;
            auto data = audio::pipe_read(ch, size);
            VLOG("[dsp] ReadPipe ch=%u size=%u -> %zu bytes%s\n", ch, size, data.size(),
                 data.size() >= 4 ? (" first " + std::to_string(data[0] | (data[1] << 8)) + "," + std::to_string(data[2] | (data[3] << 8))).c_str() : "");
            u32 bsz = 0;
            u32 dst = ipc.static_buffer(0, &bsz);
            if (!data.empty()) memcpy(gptr(dst), data.data(), data.size());
            if (ipc.cmd() == 0x0010) {
                ipc.ok(2, 2);
                ipc.set(2, (u32)data.size());
                ipc.static_desc(3, 0, (u32)data.size());
            } else {
                ipc.ok(1, 2);
                ipc.static_desc(2, 0, (u32)data.size());
            }
            break;
        }
        case 0x000F:  // GetPipeReadableSize(channel, peer)
            ipc.ok(2);
            ipc.set(2, audio::pipe_readable(ipc[1]));
            break;
        case 0x0011:  // LoadComponent(size, prog mask, data mask, mapped buffer)
            ipc.ok(2, 2);
            ipc.set(2, 1);
            ipc.set(3, (ipc[1] << 4) | 0xA);
            ipc.set(4, ipc[5]);
            break;
        case 0x0015: {  // RegisterInterruptEvents(interrupt, channel, event)
            u32 irq = ipc[1], ch = ipc[2];
            auto ev = get<Event>(ipc[4]);
            VLOG("[dsp] RegisterInterruptEvents irq=%u ch=%u ev=%p\n", irq, ch, (void *)ev.get());
            if (irq == 2 && ch < 4) dsp.pipe_irq[ch] = ev;
            else if (irq < 2) dsp.other_irq[irq] = ev;
            ipc.ok();
            break;
        }
        case 0x0016:  // GetSemaphoreEventHandle
            ipc.ok(1, 2);
            ipc.handles(2, {dsp.semaphore});
            break;
        case 0x001F:  // GetHeadphoneStatus
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        default:
            unimplemented(ipc);
        }
    }
};

void register_dsp() { register_service(std::make_shared<Dsp>("dsp::DSP")); }

}  // namespace kernel

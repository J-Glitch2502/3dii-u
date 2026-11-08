// cam:u: there's no camera to read, but a game must be able to open it and back out. Every frame arrives on time as a plain dark picture.
#include "../service.h"
#include "services.h"

namespace kernel {

namespace {
struct Port {
    std::shared_ptr<Event> vsync = make_event(ONESHOT, "cam vsync");
    std::shared_ptr<Event> error = make_event(ONESHOT, "cam error");
    std::shared_ptr<Event> received = make_event(ONESHOT, "cam received");
    u32 dst = 0, size = 0, unit = 0;
    bool active = false, receiving = false, capturing = false;
};

constexpr u32 MIN_TRANSFER_UNIT = 256, MAX_BUFFER_SIZE = 2560;
constexpr u64 FRAME_NS = 33333333;

struct Cam : Service {
    using Service::Service;
    Port ports[2];
    u32 activated = 0;
    bool ticking = false;

    template <class F>
    void each(u32 mask, F &&f) {
        for (u32 i = 0; i < 2; i++)
            if (mask & (1u << i)) f(ports[i]);
    }

    // (lock held)
    void frame() {
        bool any = false;
        for (Port &p : ports) {
            if (!p.active && !p.capturing) continue;
            any = true;
            p.vsync->signal();  // an active camera has a vsync, capturing or not: games count a few to let it settle before they capture
            if (p.capturing && p.receiving) {
                p.receiving = false;
                u8 *d = gptr(p.dst);
                for (u32 i = 0; i + 1 < p.size; i += 2) {  // YUV 4:2:2: dark, no colour
                    d[i] = 0x20;
                    d[i + 1] = 0x80;
                }
                gsp_cpu_wrote(p.dst, p.size);
                p.received->signal();
            }
        }
        ticking = any;
        if (any) add_timeout(now_ns() + FRAME_NS, [this] { frame(); });
    }

    void start_frames() {
        if (ticking) return;
        ticking = true;
        add_timeout(now_ns() + FRAME_NS, [this] { frame(); });
    }

    void handle(IPC &ipc) override {
        VLOG("[cam] command %04x: %x %x %x %x\n", ipc.cmd(), ipc[1], ipc[2], ipc[3], ipc[4]);
        switch (ipc.cmd()) {
        case 0x0001:  // StartCapture(port)
            each(ipc[1], [](Port &p) { p.capturing = true; });
            start_frames();
            ipc.ok();
            break;
        case 0x0002:  // StopCapture(port)
            each(ipc[1], [](Port &p) { p.capturing = p.receiving = false; });
            ipc.ok();
            break;
        case 0x0003: {  // IsBusy(port)
            bool busy = false;
            each(ipc[1], [&](Port &p) { busy |= p.capturing; });
            ipc.ok(2);
            ipc.set(2, busy);
            break;
        }
        case 0x0005:  // GetVsyncInterruptEvent(port)
        case 0x0006: {  // GetBufferErrorInterruptEvent(port)
            Port &p = ports[(ipc[1] >> 1) & 1];
            ipc.ok(1, 2);
            ipc.handles(2, {ipc.cmd() == 5 ? p.vsync : p.error});
            break;
        }
        case 0x0007: {  // SetReceiving(destination, port, size, transfer unit, 0, process)
            Port &p = ports[(ipc[2] >> 1) & 1];
            p.dst = ipc[1];
            p.size = ipc[3];
            p.receiving = true;
            p.received->clear();
            ipc.ok(1, 2);
            ipc.handles(2, {p.received});
            break;
        }
        case 0x0008: {  // IsFinishedReceiving(port)
            bool finished = true;
            each(ipc[1], [&](Port &p) { finished &= !p.receiving; });
            ipc.ok(2);
            ipc.set(2, finished);
            break;
        }
        case 0x0009:  // SetTransferLines(port, lines, width, height)
            each(ipc[1], [&](Port &p) { p.unit = (ipc[2] & 0xffff) * (ipc[3] & 0xffff) * 2; });
            ipc.ok();
            break;
        case 0x000A: {  // GetMaxLines(width, height)
            const u32 w = ipc[1] & 0xffff, h = ipc[2] & 0xffff;
            u32 lines = w ? std::min(MAX_BUFFER_SIZE / w, h) : 0;
            while (lines && (h % lines || lines * w * 2 % MIN_TRANSFER_UNIT)) lines--;
            ipc.ok(2);
            ipc.set(2, lines);
            break;
        }
        case 0x000B:  // SetTransferBytes(port, bytes, width, height)
            each(ipc[1], [&](Port &p) { p.unit = ipc[2] & 0xffff; });
            ipc.ok();
            break;
        case 0x000C:  // GetTransferBytes(port)
            ipc.ok(2);
            ipc.set(2, ports[(ipc[1] >> 1) & 1].unit);
            break;
        case 0x000D: {  // GetMaxBytes(width, height)
            const u32 size = (ipc[1] & 0xffff) * (ipc[2] & 0xffff) * 2;
            u32 bytes = MAX_BUFFER_SIZE;
            while (bytes > MIN_TRANSFER_UNIT && size % bytes) bytes -= MIN_TRANSFER_UNIT;
            ipc.ok(2);
            ipc.set(2, bytes);
            break;
        }
        case 0x000F:  // IsTrimming(port)
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        case 0x0011:  // GetTrimmingParams(port)
            ipc.ok(5);
            for (u32 i = 0; i < 4; i++) ipc.set(2 + i, 0);
            break;
        case 0x0013:  // Activate(camera): 1 and 2 are on the first port, 4 on the second
            activated = ipc[1];
            ports[0].active = activated & 3;
            ports[1].active = activated & 4;
            if (activated) start_frames();
            ipc.ok();
            break;
        case 0x0036:  // GetSuitableY2rStandardCoefficient
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        case 0x003B:  // GetActivatedCamera
            ipc.ok(2);
            ipc.set(2, activated);
            break;
        case 0x002B:  // GetStereoCameraCalibrationData
            ipc.ok(17);
            for (u32 i = 0; i < 16; i++) ipc.set(2 + i, 0);
            break;
        case 0x003A:  // DriverFinalize
            for (Port &p : ports) p.active = p.capturing = p.receiving = false;
            activated = 0;
            ipc.ok();
            break;
        default:
            // the rest set up a camera that isn't there
            ipc.ok();
        }
    }
};
}  // namespace

void register_cam() { register_service(std::make_shared<Cam>("cam:u")); }

}  // namespace kernel

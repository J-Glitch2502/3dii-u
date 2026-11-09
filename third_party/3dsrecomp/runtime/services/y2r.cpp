// y2r:u: the YUV to RGB converter that video playback (moflex) goes through. The hardware reads Y, U and V from memory by DMA and
// writes RGB out as lines or 8x8 tiles. Here the whole conversion runs when the game starts it, so it's finished by the time the
// game looks. Command numbers, coefficient tables and rounding follow 3dbrew and Citra's reverse engineering of the hardware.
#include <algorithm>
#include <cstring>
#include <vector>

#include "../service.h"
#include "services.h"

namespace kernel {
namespace {

struct Dma {
    u32 addr = 0, size = 0, unit = 0, gap = 0;
};

// bytes along a DMA pattern: `unit` bytes, then skip `gap`; step 2 keeps the first byte of each 16-bit sample
struct DmaReader {
    u32 at, left, unit, gap, step;
    DmaReader(const Dma &d, u32 step_) : at(d.addr), left(d.unit), unit(d.unit), gap(d.gap), step(step_) {}
    void read(u8 *out, u32 n) {
        for (u32 got = 0; got < n;) {
            if (!unit) {  // no pattern: one continuous run
                out[got++] = *gptr(at);
                at += step;
                continue;
            }
            const u32 run = std::min(n - got, left / step);
            if (step == 1) {
                GuestCopyFrom(out + got, at, run);
            } else {
                for (u32 i = 0; i < run; i++) out[got + i] = *gptr(at + i * step);
            }
            got += run;
            at += run * step;
            left -= run * step;
            if (left < step) {
                at += left + gap;
                left = unit;
            }
        }
    }
};

struct DmaWriter {
    u32 at, left, unit, gap;
    explicit DmaWriter(const Dma &d) : at(d.addr), left(d.unit), unit(d.unit), gap(d.gap) {}
    void put(const u8 *p, u32 n) {
        GuestCopyTo(at, p, n);
        at += n;
        if (!unit) return;
        left -= std::min(left, n);
        if (!left) {
            at += gap;
            left = unit;
        }
    }
};

constexpr s16 kStandard[4][8] = {
    {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B},  // ITU-R 601
    {0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51},   // ITU-R 709
    {0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B},  // ITU-R 601, scaled
    {0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99C, -0x2421},   // ITU-R 709, scaled
};

struct Y2rService : Service {
    using Service::Service;
    std::shared_ptr<Event> done = make_event(ONESHOT, "y2r");
    u32 input_format = 0, output_format = 0, rotation = 0, block = 0, spatial = 0, temporal = 0, interrupt = 0;
    u32 width = 0, lines = 0, alpha = 0xff, standard = 0;
    s16 co[8] = {};
    u16 dither[8] = {};
    Dma y, u, v, yuyv, out;
    bool warned_rotation = false;

    void convert() {
        if (!width || (width & 7) || width > 1024 || !out.addr) {
            LOG("[y2r] can't convert: width %u, output %08x\n", width, out.addr);
            return;
        }
        VLOG("[y2r] convert %ux%u in %u out %u block %u rotation %u, from %08x/%08x (size %x) to %08x size %x unit %x gap %x\n", width, lines, input_format, output_format, block, rotation,
             yuyv.addr, y.addr, yuyv.size, out.addr, out.size, out.unit, out.gap);
        const u32 tiles_across = width / 8;
        const bool is420 = input_format == 1 || input_format == 3, interleaved = input_format == 4;
        const u32 step = input_format == 2 || input_format == 3 ? 2 : 1;
        const u32 bytes = output_format == 0 ? 4 : output_format == 1 ? 3 : 2;
        std::vector<u8> luma(width * (interleaved ? 16 : 8)), cu(width * 4), cv(width * 4);
        std::vector<u32> tiles(tiles_across * 64), turned(tiles_across * 64);
        std::vector<u8> line(width * 4);
        DmaReader ry(interleaved ? yuyv : y, interleaved ? 1 : step), ru(u, step), rv(v, step);
        DmaWriter w(out);

        // positions in a tile, in output order
        u8 order[64];
        for (u32 row = 0; row < 8; row++)
            for (u32 x = 0; x < 8; x++) {
                u32 m = 0;
                if (block)
                    for (u32 b = 0; b < 3; b++) m |= ((x >> b) & 1) << (2 * b) | ((row >> b) & 1) << (2 * b + 1);
                else
                    m = row * 8 + x;
                order[row * 8 + x] = (u8)m;
            }
        auto encode = [&](u32 c, u8 *d) {
            const u32 r = c >> 24, g = (c >> 16) & 255, b = (c >> 8) & 255;
            switch (output_format) {
            case 0: d[0] = (u8)alpha; d[1] = (u8)b; d[2] = (u8)g; d[3] = (u8)r; break;
            case 1: d[0] = (u8)b; d[1] = (u8)g; d[2] = (u8)r; break;
            case 2: {
                const u32 p = (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | ((alpha >> 7) & 1);
                d[0] = (u8)p;
                d[1] = (u8)(p >> 8);
                break;
            }
            default: {
                const u32 p = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
                d[0] = (u8)p;
                d[1] = (u8)(p >> 8);
                break;
            }
            }
        };
        for (u32 top = 0; top < lines; top += 8) {
            const u32 rows = std::min(lines - top, 8u);
            if (interleaved) {
                ry.read(luma.data(), rows * width * 2);
            } else {
                ry.read(luma.data(), rows * width);
                const u32 crows = is420 ? (rows + 1) / 2 : rows;
                ru.read(cu.data(), crows * width / 2);
                rv.read(cv.data(), crows * width / 2);
            }
            for (u32 row = 0; row < rows; row++) {
                const u32 crow = is420 ? row / 2 : row;
                for (u32 x = 0; x < width; x++) {
                    s32 Y, U, V;
                    if (interleaved) {
                        const u8 *p = &luma[(row * width + (x & ~1u)) * 2];
                        Y = luma[(row * width + x) * 2];
                        U = p[1];
                        V = p[3];
                    } else {
                        Y = luma[row * width + x];
                        U = cu[crow * (width / 2) + x / 2];
                        V = cv[crow * (width / 2) + x / 2];
                    }
                    const s32 cy = co[0] * Y;
                    const s32 r = ((cy + co[1] * V) >> 3) + co[5] + 0x18;
                    const s32 g = ((cy - co[2] * V - co[3] * U) >> 3) + co[6] + 0x18;
                    const s32 b = ((cy + co[4] * U) >> 3) + co[7] + 0x18;
                    const u32 pr = std::clamp(r >> 5, 0, 255), pg = std::clamp(g >> 5, 0, 255), pb = std::clamp(b >> 5, 0, 255);
                    tiles[(x / 8) * 64 + row * 8 + (x & 7)] = pr << 24 | pg << 16 | pb << 8;
                }
            }
            // rotation turns each tile and reverses their order where it must
            if (rotation) {
                if (!warned_rotation) LOG("[y2r] rotation %u in use\n", rotation);
                warned_rotation = true;
                for (u32 t = 0; t < tiles_across; t++) {
                    const u32 *in = &tiles[t * 64];
                    u32 *o = &turned[(rotation >= 2 ? tiles_across - 1 - t : t) * 64];
                    for (u32 i = 0; i < rows; i++)
                        for (u32 j = 0; j < 8; j++) {
                            const u32 p = in[i * 8 + j];
                            if (rotation == 1) o[j * 8 + 7 - i] = p;
                            else if (rotation == 2) o[(rows - 1 - i) * 8 + 7 - j] = p;
                            else o[(7 - j) * 8 + i] = p;
                        }
                }
                tiles.swap(turned);
            }
            u8 px[4];
            if (block) {
                std::vector<u32> shuffled(64);
                for (u32 t = 0; t < tiles_across; t++) {
                    for (u32 i = 0; i < 64; i++) shuffled[order[i]] = tiles[t * 64 + i];
                    for (u32 i = 0; i < 64; i++) {
                        encode(shuffled[i], px);
                        w.put(px, bytes);
                    }
                }
            } else {
                for (u32 row = 0; row < rows; row++) {
                    u8 *d = line.data();
                    for (u32 x = 0; x < width; x++, d += bytes) encode(tiles[(x / 8) * 64 + row * 8 + (x & 7)], d);
                    // whole lines in one go, unless the DMA pattern is finer than that
                    if (!out.unit || out.unit >= width * bytes) {
                        w.put(line.data(), width * bytes);
                    } else {
                        for (u32 x = 0; x < width; x++) w.put(line.data() + x * bytes, bytes);
                    }
                }
            }
        }
        // the GPU may read this next
        gsp_cpu_wrote(out.addr, std::max<u32>(out.size, (out.unit ? (out.unit + out.gap) : width * bytes) * lines));
    }

    static Dma dma(IPC &ipc) {
        Dma d;
        d.addr = ipc[1];
        d.size = ipc[2];
        d.unit = ipc[3] & 0xffff;  // 16 bits; the rest of the word can be left over from an earlier call, and a wild gap writes all over memory
        d.gap = ipc[4] & 0xffff;
        return d;
    }

    void handle(IPC &ipc) override {
        auto get = [&](u32 v) {
            ipc.ok(2);
            ipc.set(2, v);
        };
        switch (ipc.cmd()) {
        case 0x0001: input_format = ipc[1] & 0xff; ipc.ok(); break;
        case 0x0002: get(input_format); break;
        case 0x0003: output_format = ipc[1] & 0xff; ipc.ok(); break;
        case 0x0004: get(output_format); break;
        case 0x0005: rotation = ipc[1] & 0xff; ipc.ok(); break;
        case 0x0006: get(rotation); break;
        case 0x0007: block = ipc[1] & 0xff; ipc.ok(); break;
        case 0x0008: get(block); break;
        case 0x0009: spatial = ipc[1] & 0xff; ipc.ok(); break;
        case 0x000A: get(spatial); break;
        case 0x000B: temporal = ipc[1] & 0xff; ipc.ok(); break;
        case 0x000C: get(temporal); break;
        case 0x000D: interrupt = ipc[1] & 0xff; ipc.ok(); break;
        case 0x000E: get(interrupt); break;
        case 0x000F:  // GetTransferEndEvent
            ipc.ok(1, 2);
            ipc.handles(2, {done});
            break;
        case 0x0010: y = dma(ipc); ipc.ok(); break;
        case 0x0011: u = dma(ipc); ipc.ok(); break;
        case 0x0012: v = dma(ipc); ipc.ok(); break;
        case 0x0013: yuyv = dma(ipc); ipc.ok(); break;
        case 0x0014: case 0x0015: case 0x0016: case 0x0017: get(1); break;  // IsFinishedSending...
        case 0x0018: out = dma(ipc); ipc.ok(); break;
        case 0x0019: get(1); break;  // IsFinishedReceiving
        case 0x001A: width = ipc[1] & 0xffff; ipc.ok(); break;
        case 0x001B: get(width); break;
        case 0x001C: lines = ipc[1] & 0xffff; ipc.ok(); break;
        case 0x001D: get(lines); break;
        case 0x001E:  // SetCoefficient
            for (u32 i = 0; i < 8; i++) co[i] = (s16)(ipc[1 + i / 2] >> (16 * (i & 1)));
            ipc.ok();
            break;
        case 0x0020:  // SetStandardCoefficient
            standard = std::min<u32>(ipc[1], 3);
            memcpy(co, kStandard[standard], sizeof co);
            ipc.ok();
            break;
        case 0x0022: alpha = ipc[1] & 0xffff; ipc.ok(); break;
        case 0x0023: get(alpha); break;
        case 0x0024:
            for (u32 i = 0; i < 8; i++) dither[i] = (u16)(ipc[1 + i / 2] >> (16 * (i & 1)));
            ipc.ok();
            break;
        case 0x0026:  // StartConversion
            convert();
            done->signal();
            ipc.ok();
            break;
        case 0x0027: ipc.ok(); break;   // StopConversion
        case 0x0028: get(0); break;     // IsBusyConversion
        case 0x0029: {                  // SetPackageParameter
            const u32 a = ipc[1], b = ipc[2], c = ipc[3];
            input_format = a & 0xff;
            output_format = (a >> 8) & 0xff;
            rotation = (a >> 16) & 0xff;
            block = a >> 24;
            width = b & 0xffff;
            lines = b >> 16;
            standard = std::min<u32>(c & 0xff, 3);
            memcpy(co, kStandard[standard], sizeof co);
            alpha = c >> 16;
            ipc.ok();
            break;
        }
        case 0x002A: get(1); break;  // PingProcess
        case 0x002B:                 // DriverInitialize
            input_format = output_format = rotation = block = spatial = temporal = interrupt = 0;
            width = lines = 0;
            alpha = 0;
            memset(co, 0, sizeof co);
            y = u = v = yuyv = out = Dma{};
            ipc.ok();
            break;
        case 0x002C: ipc.ok(); break;  // DriverFinalize
        default:
            unimplemented(ipc);
        }
    }
};
}  // namespace

void register_y2r() { register_service(std::make_shared<Y2rService>("y2r:u")); }
}  // namespace kernel

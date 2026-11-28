// APT: applet manager. Only the application slot exists; the home menu and the applets are absent (library applets
// run as HLE, system applets such as the browser close as soon as they start).
#include <algorithm>
#include <climits>
#include <deque>

#include "../mii_select.h"
#include "../service.h"
#include "../swkbd.h"
#include "apt_mii_qr.h"
#include "apt_wireless_reboot.h"
#include "services.h"

__attribute__((weak)) bool mii_select_shown() { return false; }

namespace kernel {

namespace {
struct Parameter {
#ifdef RECOMP_RESUMABLE
    void state_fields(checkpoint::Archive &a) { a.fields(sender, dest, signal, object, buffer); }
#endif
    u32 sender = 0, dest = 0x300, signal = 0;
    ObjPtr object;
    std::vector<u8> buffer;
};

struct AptState {
#ifdef RECOMP_RESUMABLE
    void state_fields(checkpoint::Archive &a) { a.fields(lock, notif, param_ev, params, notifications, initialized, cpu_limit, screencap, font, lib_applet); }
#endif
    std::shared_ptr<Mutex> lock = std::make_shared<Mutex>();
    std::shared_ptr<Event> notif = make_event(ONESHOT, "APT notification");
    std::shared_ptr<Event> param_ev = make_event(ONESHOT, "APT parameter");
    std::deque<Parameter> params;
    std::deque<u32> notifications;
    bool initialized = false;
    u32 cpu_limit = 0;
    u32 screencap = 0;
    std::shared_ptr<SharedMem> font;
    u32 lib_applet = 0;
};
AptState apt;
}  // namespace

static void send_parameter(Parameter p) {
    apt.params.push_back(std::move(p));
    apt.param_ev->signal();
}

// Library applets run as HLE: they answer the capture-buffer request and finish at once.
static void applet_receive(u32 id, u32 signal, u32 ptr, u32 size) {
    if (signal == 2) {  // Request: wants a shared memory block for the screen capture
        u32 cap = size >= 4 ? r32(ptr) : 0x1000;
        if (!cap) cap = 0x1000;
        static std::shared_ptr<SharedMem> capture;  // one block, reused by every launch (nothing reads it)
        if (!capture || capture->size < cap) {
            capture = std::make_shared<SharedMem>();
            capture->name = "applet capture";
            capture->size = cap;
            capture->backing = mem::hle_alloc(cap);
        }
        send_parameter({id, 0x300, 3, capture, {}});  // Response
    }
}

// System applets (the browser, ...) are absent: the one started takes the application's request at once and closes
static void system_applet_receive(u32 id, u32 signal) {
    if (signal != 16) return;  // RequestForSysApplet
    send_parameter({id, 0x300, 3, nullptr, {}});   // Response
    send_parameter({id, 0x300, 10, nullptr, {}});  // WakeupByExit: back to the application
}

// What the Mii selector returns (MiiSelectorReturn, 0x84 bytes): the Mii picked (misc.cpp), or none when the player cancelled
static std::vector<u8> mii_selector_result(const std::array<u8, 0x60> *mii) {
    std::vector<u8> r(0x84, 0);
    r[8] = r[9] = r[10] = r[11] = 0xFF;  // not a guest (4), guest index -1
    if (!mii) {
        r[0] = 1;  // no Mii selected
        return r;
    }
    memcpy(&r[0x0C], mii->data(), 0x60);
    return r;
}

// Software keyboard (SwkbdState, 0x400 bytes). The player types the text in the window (swkbd.h, drawn by
// menu.mm) and the applet finishes when they confirm or cancel; with RECOMP_SWKBD_TEXT it "types" that at once
// (tests). The text goes into the shared memory block the application passed, as UTF-16 at offset 0.
static std::string utf8(const std::u16string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        u32 c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) o += (char)(0xC0 | c >> 6), o += (char)(0x80 | (c & 0x3F));
        else if (c < 0x10000) o += (char)(0xE0 | c >> 12), o += (char)(0x80 | (c >> 6 & 0x3F)), o += (char)(0x80 | (c & 0x3F));
        else o += (char)(0xF0 | c >> 18), o += (char)(0x80 | (c >> 12 & 0x3F)), o += (char)(0x80 | (c >> 6 & 0x3F)), o += (char)(0x80 | (c & 0x3F));
    }
    return o;
}
static std::u16string utf16(const std::string &s) {
    std::u16string o;
    for (size_t i = 0; i < s.size();) {
        u8 b = (u8)s[i];
        int n = b < 0x80 ? 0 : b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : -1;
        if (n < 0 || i + n >= s.size() + (n ? 0 : 1)) {
            i++;
            continue;
        }
        u32 c = n == 0 ? b : b & (0x3F >> n);
        for (int k = 1; k <= n; k++) c = c << 6 | ((u8)s[i + k] & 0x3F);
        i += n + 1;
        if (c >= 0x10000) o += (char16_t)(0xD800 + ((c - 0x10000) >> 10)), o += (char16_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
        else o += (char16_t)c;
    }
    return o;
}

namespace {
struct SwkbdPending {
#ifdef RECOMP_RESUMABLE
    void state_fields(checkpoint::Archive &a) { a.fields(id, cfg, mem); }
#endif
    u32 id = 0;
    std::vector<u8> cfg;
    ObjPtr mem;
};
std::mutex swkbd_m;  // guards the request the window reads (the rest is under the kernel lock)
swkbd::Request swkbd_req;
bool swkbd_active = false;
SwkbdPending swkbd_wait;
unsigned swkbd_serial = 0;
}  // namespace

// fills in the text and the button pressed (ok: the confirm button, else the cancel one)
static void swkbd_result(std::vector<u8> &cfg, ObjPtr mem, std::u16string text, bool ok) {
    auto wr32 = [&](size_t o, u32 v) { v = le32(v); memcpy(&cfg[o], &v, 4); };
    u32 buttons, max_len = cfg[0x20] | cfg[0x21] << 8;
    memcpy(&buttons, &cfg[0x04], 4);
    buttons = le32(buttons) + 1;
    auto sm = std::dynamic_pointer_cast<SharedMem>(mem);
    if (max_len && text.size() > max_len) text.resize(max_len);
    if (sm && (text.size() + 1) * 2 <= sm->size) {
        for (size_t i = 0; i < text.size(); i++) w16(sm->backing + 2 * (u32)i, text[i]);
        w16(sm->backing + 2 * (u32)text.size(), 0);
    }
    // D0_CLICK / D1_CLICK0, D1_CLICK1 / D2_CLICK0, D2_CLICK2: the left button cancels, the right one confirms
    wr32(0x138, buttons >= 3 ? (ok ? 5 : 3) : buttons == 2 ? (ok ? 2 : 1) : 0);
    wr32(0x144, 0);  // text_offset
    cfg[0x148] = (u8)text.size();
    cfg[0x149] = (u8)(text.size() >> 8);
    wr32(0x14C, 0);  // callback_result: OK
    LOG("[apt] software keyboard: %s (%zu UTF-16 units)\n", ok ? "typed" : "cancelled", text.size());
}

// true if the applet finished (else it waits for the player: swkbd::finish)
static bool swkbd_run(u32 id, std::vector<u8> &cfg, ObjPtr mem) {
    if (cfg.size() < 0x150) return true;
    auto rd32 = [&](size_t o) { u32 v; memcpy(&v, &cfg[o], 4); return le32(v); };
    auto rd16s = [&](size_t o, size_t n) {
        std::u16string s;
        for (size_t i = 0; i < n; i++) {
            char16_t c = (char16_t)(cfg[o + 2 * i] | cfg[o + 2 * i + 1] << 8);
            if (!c) break;
            s += c;
        }
        return s;
    };
    auto sm = std::dynamic_pointer_cast<SharedMem>(mem);
    const u32 type = rd32(0x00), buttons = rd32(0x04) + 1, filters = rd32(0x18);
    std::u16string initial;
    if (sm && (s32)rd32(0x120) >= 0 && rd32(0x120) + 2 <= sm->size)
        for (u32 a = sm->backing + rd32(0x120); a + 2 <= sm->backing + sm->size && r16(a); a += 2) initial += (char16_t)r16(a);
    if (const char *t = getenv("RECOMP_SWKBD_TEXT")) {
        std::u16string text = *t ? utf16(t) : initial;
        if (text.empty()) text = type == 2 ? u"1" : u"Player";
        swkbd_result(cfg, mem, text, true);
        return true;
    }
    swkbd::Request r;
    r.hint = utf8(rd16s(0x90, 65));
    r.initial = utf8(initial);
    r.ok = utf8(rd16s(0x26 + 34 * (std::min(buttons, 3u) - 1), 17));
    r.cancel = buttons > 1 ? utf8(rd16s(0x26, 17)) : "";
    r.can_cancel = buttons > 1;
    r.max_len = cfg[0x20] | cfg[0x21] << 8;
    r.max_digits = cfg[0x24] | cfg[0x25] << 8;
    r.numpad = type == 2;
    r.password = rd32(0x0C) != 0;
    r.multiline = cfg[0x113] != 0;
    r.valid = (int)rd32(0x08);
    r.digits_limited = filters & 1;
    r.no_at = filters & 2;
    r.no_percent = filters & 4;
    r.no_backslash = filters & 8;
    LOG("[apt] software keyboard: waiting for the player (hint \"%s\", max %d)\n", r.hint.c_str(), r.max_len);
    swkbd_wait = {id, cfg, mem};
    std::lock_guard<std::mutex> g(swkbd_m);
    r.serial = ++swkbd_serial;
    swkbd_req = r;
    swkbd_active = true;
    return false;
}

// The game's MiiSelectorConf, held until the player picks a console Mii or cancels.
// The 3DS whitelist addresses 100 database slots. An unrestricted request can also select the Wii U's extra Miis.
namespace {
std::mutex mii_m;  // guards the immutable request published to the port
std::shared_ptr<const mii_select::Request> mii_req;
std::vector<u32> mii_rows;  // the list's rows: which of console_miis() each is
u32 mii_wait_id = 0;
}  // namespace

static void mii_select_run(u32 id, const std::vector<u8> &cfg) {
    int personal = -1;
    const auto &miis = console_miis(&personal);
    const bool show = mii_select_shown();
    const u8 *allowed = cfg.size() >= 0xFE ? cfg.data() + 0x9A : nullptr;
    const bool all_allowed = !allowed || std::all_of(allowed, allowed + 100, [](u8 flag) { return flag != 0; });
    if ((!show && all_allowed) || miis.empty() || cfg.size() < 0x94) {
        LOG("[apt] Mii selector: returning the player's Mii\n");
        const auto mii = player_mii();
        send_parameter({id, 0x300, 10, nullptr, mii_selector_result(&mii)});
        return;
    }
    mii_select::Request r;
    std::u16string title;
    for (size_t o = 0x08; o + 1 < 0x88 && (cfg[o] | cfg[o + 1]); o += 2) title += (char16_t)(cfg[o] | cfg[o + 1] << 8);
    r.title = utf8(title);
    std::vector<int> slots(miis.size());
    std::vector<u32> rows;
    rows.reserve(miis.size());
    for (u32 i = 0; i < miis.size(); i++) {
        const int slot = slots[i] = mii_maker_place(i);
        if (all_allowed || (slot >= 0 && slot < 100 && allowed[slot])) rows.push_back(i);
    }
    if (rows.empty()) {
        send_parameter({id, 0x300, 10, nullptr, mii_selector_result(nullptr)});
        return;
    }
    // Bounded guest database order first, followed by overflow in console order.
    const auto place = [&](u32 i) { return slots[i] < 0 ? INT_MAX : slots[i]; };
    std::stable_sort(rows.begin(), rows.end(), [&](u32 a, u32 b) { return place(a) < place(b); });
    const u32 first = cfg[0x90] | cfg[0x91] << 8 | cfg[0x92] << 16 | (u32)cfg[0x93] << 24;
    std::vector<int> places;
    places.reserve(rows.size());
    for (const u32 i : rows) places.push_back(slots[i]);
    const auto own = std::find(rows.begin(), rows.end(), (u32)personal);
    r.initial = mii_select::initial_row(places, first, (int)(own - rows.begin()));
    if (!show) {
        const auto mii = console_mii(rows[r.initial]);
        send_parameter({id, 0x300, 10, nullptr, mii_selector_result(&mii)});
        return;
    }
    r.entries.reserve(rows.size());
    for (const u32 i : rows) {
        const auto &m = miis[i];
        std::u16string name;
        for (size_t o = 0x1A; o < 0x2E && (m[o] | m[o + 1]); o += 2) name += (char16_t)(m[o] | m[o + 1] << 8);
        r.entries.push_back({utf8(name), m});
    }
    r.can_cancel = cfg[0] != 0;
    LOG("[apt] Mii selector: waiting for the player (%zu Miis, \"%s\")\n", rows.size(), r.title.c_str());
    mii_wait_id = id;
    mii_rows = std::move(rows);
    auto request = std::make_shared<const mii_select::Request>(std::move(r));
    std::lock_guard<std::mutex> g(mii_m);
    mii_req = std::move(request);
}

static void applet_run(u32 id, std::vector<u8> buf, ObjPtr mem = nullptr) {
    u32 kind = id & 0xFF;
    if (kind == 0x02) {
        mii_select_run(id, buf);
        return;
    }
    if (kind == 0x06 && buf.size() >= 8) {
        // error/EULA applet: return code 0 (success). ErrEulaConfig.return_code is at offset 0xF0C? keep the
        // buffer and clear the result fields the caller checks
        LOG("[apt] error applet: type %u code %08x\n", buf[0], *(u32 *)&buf[4]);
    } else if (kind == 0x01) {
        if (!swkbd_run(id, buf, mem)) return;
    } else {
        LOG("[apt] library applet %x: finishing immediately\n", id);
    }
    send_parameter({id, 0x300, 10, nullptr, std::move(buf)});  // WakeupByExit
}

}  // namespace kernel

bool swkbd::pending(Request *out) {
    std::lock_guard<std::mutex> g(kernel::swkbd_m);
    if (kernel::swkbd_active && out) *out = kernel::swkbd_req;
    return kernel::swkbd_active;
}

void swkbd::finish(const std::string &text, bool ok) {
    using namespace kernel;
    Lock lk(kmutex);
    {
        std::lock_guard<std::mutex> g(swkbd_m);
        if (!swkbd_active) return;
        swkbd_active = false;
    }
    auto w = std::make_shared<SwkbdPending>(std::move(swkbd_wait));
    swkbd_wait = {};
    std::u16string t = utf16(text);
    run_when_quiescent([w, t, ok] {
        swkbd_result(w->cfg, w->mem, t, ok);
        send_parameter({w->id, 0x300, 10, nullptr, std::move(w->cfg)});  // WakeupByExit
    });
    schedule_async();
}

std::shared_ptr<const mii_select::Request> mii_select::pending() {
    std::lock_guard<std::mutex> g(kernel::mii_m);
    return kernel::mii_req;
}

void mii_select::finish(const Request &request, int index) {
    using namespace kernel;
    Lock lk(kmutex);
    std::string name;
    int mii = -1;  // which of console_miis()
    {
        std::lock_guard<std::mutex> g(mii_m);
        if (mii_req.get() != &request || index < -1 ||
            (index == -1 && !request.can_cancel) || (index >= 0 && (size_t)index >= mii_rows.size()))
            return;
        mii_req.reset();
        if (index >= 0) {
            name = request.entries[index].name;
            mii = (int)mii_rows[index];
        }
    }
    LOG("[apt] Mii selector: %s\n", mii >= 0 ? name.c_str() : "cancelled");
    const u32 id = mii_wait_id;
    run_when_quiescent([id, mii] {
        const auto data = mii >= 0 ? console_mii((u32)mii) : std::array<u8, 0x60>{};
        send_parameter({id, 0x300, 10, nullptr, mii_selector_result(mii >= 0 ? &data : nullptr)});  // WakeupByExit
    });
    schedule_async();
}

namespace kernel {

void apt_notify(u32 signal) {
    apt.notifications.push_back(signal);
    apt.notif->signal();
}

struct AptService : Service {
    SERVICE_CHECKPOINT(AptService)
    using Service::Service;
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x0001:  // GetLockHandle(attributes)
            ipc.ok(3, 2);
            ipc.set(2, ipc[1]);
            ipc.set(3, 0);
            ipc.handles(4, {apt.lock});
            break;
        case 0x0002:  // Initialize(app_id, attributes)
            ipc.ok(1, 3);
            set_handles(ipc);
            if (!apt.initialized) {
                apt.initialized = true;
                send_parameter({0, 0x300, 1, nullptr, {}});  // Wakeup
            }
            break;
        case 0x0003:  // Enable
        case 0x0004:  // Finalize
            ipc.ok();
            break;
        case 0x0005:  // GetAppletManInfo(pos)
            ipc.ok(5);
            ipc.set(2, 0);       // active applet pos: app
            ipc.set(3, 0);       // requested applet id
            ipc.set(4, 0x101);   // home menu
            ipc.set(5, 0x300);   // active: application
            break;
        case 0x0006: {  // GetAppletInfo(app_id)
            ipc.ok(7);
            ipc.set64(2, 0x0004003000008F02ull);
            ipc.set(4, 0);  // media type
            ipc.set(5, 1);  // registered
            ipc.set(6, 1);  // loaded
            ipc.set(7, 0);  // attributes
            break;
        }
        case 0x0009:  // IsRegistered(app_id)
            ipc.ok(2);
            ipc.set(2, ipc[1] == 0x300 || ipc[1] == 0x101 || (apt.lib_applet && ipc[1] == apt.lib_applet) ? 1 : 0);
            break;
        case 0x000B:  // InquireNotification
            ipc.ok(2);
            ipc.set(2, apt.notifications.empty() ? 0 : apt.notifications.front());
            if (!apt.notifications.empty()) apt.notifications.pop_front();
            break;
        case 0x000C: {  // SendParameter(src, dst, signal, size, handle, buffer)
            u32 dst = ipc[2], signal = ipc[3], size = ipc[4];
            LOG("[apt] SendParameter src=%x dst=%x signal=%x size=%x\n", ipc[1], dst, signal, size);
            if ((dst & 0xF00) == 0x200 || (dst & 0xF00) == 0x400) applet_receive(dst, signal, ipc[8], size);
            if ((dst & 0xF00) == 0x100) system_applet_receive(dst, signal);
            ipc.ok();
            break;
        }
        case 0x0016:  // PreloadLibraryApplet(id)
        case 0x0018:  // PrepareToStartLibraryApplet(id)
        case 0x0019:  // PrepareToStartSystemApplet(id)
            LOG("[apt] %s %s applet %x\n", ipc.cmd() == 0x16 ? "preload" : "prepare",
                ipc.cmd() == 0x19 ? "system" : "library", ipc[1]);
            apt.lib_applet = ipc[1];
            ipc.ok();
            break;
        case 0x0017:  // FinishPreloadingLibraryApplet
        case 0x0022:  // PrepareToCloseApplication(cancel preload)
        case 0x0027:  // CloseApplication(size, handle, buffer): ExitProcess follows
            ipc.ok();
            break;
        case 0x001E:    // StartLibraryApplet(id, size, handle, buffer)
        case 0x001F: {  // StartSystemApplet(id, size, handle, buffer): absent, see system_applet_receive
            u32 id = ipc[1], size = ipc[2], src = ipc[6];
            LOG("[apt] Start%sApplet %x size %x\n", ipc.cmd() == 0x1F ? "System" : "Library", id, size);
            std::vector<u8> buf(gptr(src), gptr(src) + size);
            if (ipc.cmd() != 0x1F) applet_run(id, buf, get_object(ipc[4]));
            ipc.ok();
            break;
        }
        case 0x000D:    // ReceiveParameter(app_id, size)
        case 0x000E: {  // GlanceParameter
            if (apt.params.empty()) {
                ipc.result(0xC8A0CFEF);  // no data
                break;
            }
            Parameter p = apt.params.front();
            if (ipc.cmd() == 0x000D) {
                apt.params.pop_front();
                if (!apt.params.empty()) apt.param_ev->signal();  // one-shot: the next one waiting is told too
            }
            u32 max = ipc[2];
            u32 n = std::min<u32>(max, (u32)p.buffer.size());
            ipc.ok(4, 4);
            ipc.set(2, p.sender);
            ipc.set(3, p.signal);
            ipc.set(4, n);
            ipc.handles(5, {p.object}, true);
            u32 dst = ipc.static_buffer(0);
            if (n) memcpy(gptr(dst), p.buffer.data(), n);
            ipc.static_desc(7, 0, n);
            break;
        }
        case 0x000F:  // CancelParameter
            ipc.ok(2);
            ipc.set(2, 1);
            break;
        case 0x003E:  // ReplySleepQuery (APT:U numbering)
        case 0x0040:  // SendCaptureBufferInfo
        case 0x0043:  // NotifyToWait
        case 0x003B:  // ReplySleepQuery
        case 0x003C:  // ReplySleepNotificationComplete
        case 0x0055:  // SetScreenCapPostPermission
            if (ipc.cmd() == 0x0055) apt.screencap = ipc[1];
            ipc.ok();
            break;
        case 0x0056:
            ipc.ok(2);
            ipc.set(2, apt.screencap);
            break;
        case 0x0044:  // GetSharedFont
            get_shared_font(ipc);
            break;
        case 0x0045:  // GetWirelessRebootInfo
            get_wireless_reboot_info(ipc);
            break;
        case 0x004B: {  // AppletUtility(cmd, in_size, out_size, in static buffer)
            u32 out_size = ipc[3];
            u32 dst = ipc.static_buffer(0);
            if (out_size) memset(gptr(dst), 0, out_size);
            if (ipc[1] == 6 && out_size) w8(dst, 1);  // TryLockTransition: allowed
            ipc.ok(2, 2);
            ipc.set(2, 0);
            ipc.static_desc(3, 0, out_size);
            break;
        }
        case 0x004F:  // SetAppCpuTimeLimit
            apt.cpu_limit = ipc[2];
            ipc.ok();
            break;
        case 0x0050:
            ipc.ok(2);
            ipc.set(2, apt.cpu_limit);
            break;
        case 0x004D: {  // GetStartupArgument(size, type)
            u32 size = ipc[1];
            u32 dst = ipc.static_buffer(0);
            if (size) memset(gptr(dst), 0, std::min<u32>(size, 0x1000));
            ipc.ok(2, 2);
            ipc.set(2, 0);
            ipc.static_desc(3, 0, std::min<u32>(size, 0x1000));
            break;
        }
        case 0x0101:  // CheckNew3DSApp
        case 0x0102:  // CheckNew3DS
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        case 0x0046:  // Wrap(out size, in size, nonce offset, nonce size, in buffer, out buffer)
        case 0x0047:  // Unwrap
            wrap(ipc, ipc.cmd() == 0x0046);
            break;
        default:
            unimplemented(ipc);
        }
    }
    // Standard Mii QR data crosses consoles and requires the same authenticated format.
    // Keep the legacy format for other requests so existing local saves remain readable.
    static void wrap(IPC &ipc, bool seal) {
        const u32 out_size = ipc[1], in_size = ipc[2], nonce_at = ipc[3], nonce_size = std::min<u32>(ipc[4] & ~3u, 12), in = ipc[6], out = ipc[8];
#ifdef __WIIU__
        if (nonce_at == apt_mii_qr::nonce_offset && nonce_size == apt_mii_qr::nonce_size &&
            in_size == (seal ? apt_mii_qr::plain_size : apt_mii_qr::wrapped_size) &&
            out_size == (seal ? apt_mii_qr::wrapped_size : apt_mii_qr::plain_size)) {
            const u32 input_type = ipc[5] & 15;
            if ((input_type != 0xA && input_type != 0xE) || in_size > (ipc[5] >> 4)) {
                ipc.result(RES_INVALID_COMBINATION);
                return;
            }
            if (!guest_readable_locked(in, in_size)) {
                ipc.result(RES_INVALID_ADDRESS);
                return;
            }
            if (const u32 error = ipc.mapped_output_error(7, out_size)) {
                ipc.result(error);
                return;
            }
            wrap_reply(ipc, apt_mii_qr::convert(seal, gptr(in), gptr(out)) ? 0 : 0xC9010401);
            return;
        }
#endif
        auto check = [](const std::vector<u8> &d, u8 *mac) {  // two 64-bit FNV-1a hashes, forwards and backwards
            u64 a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
            for (size_t i = 0; i < d.size(); i++) a = (a ^ d[i]) * 0x100000001b3ull, b = (b ^ d[d.size() - 1 - i]) * 0x100000001b3ull;
            for (int i = 0; i < 8; i++) mac[i] = (u8)(a >> 8 * i), mac[8 + i] = (u8)(b >> 8 * i);
        };
        std::vector<u8> src(in_size), body;  // body: the nonce, then the data without it
        for (u32 i = 0; i < in_size; i++) src[i] = r8(in + i);
        u32 res = 0;
        if (seal && nonce_at + nonce_size <= in_size) {
            body.assign(src.begin() + nonce_at, src.begin() + nonce_at + nonce_size);
            body.insert(body.end(), src.begin(), src.begin() + nonce_at);
            body.insert(body.end(), src.begin() + nonce_at + nonce_size, src.end());
            u8 mac[16];
            check(body, mac);
            body.insert(body.end(), mac, mac + 16);
        } else if (!seal && in_size >= nonce_size + 16 && nonce_at <= in_size - nonce_size - 16) {
            std::vector<u8> sealed(src.begin(), src.end() - 16);
            u8 mac[16];
            check(sealed, mac);
            if (memcmp(mac, &src[in_size - 16], 16)) {
                res = 0xC9010401;  // PS, wrong argument: what a 3DS answers when the check fails
            } else {
                body.assign(sealed.begin() + nonce_size, sealed.begin() + nonce_size + nonce_at);
                body.insert(body.end(), sealed.begin(), sealed.begin() + nonce_size);
                body.insert(body.end(), sealed.begin() + nonce_size + nonce_at, sealed.end());
            }
        } else {
            res = 0xC9010401;
        }
        for (u32 i = 0; i < out_size && i < body.size(); i++) w8(out + i, body[i]);
        wrap_reply(ipc, res);
    }
    static void wrap_reply(IPC &ipc, u32 res) {
        ipc.result(res, 1, 4);
        ipc.set(2, (ipc[2] << 4) | 0xA);
        ipc.set(3, ipc[6]);
        ipc.set(4, (ipc[1] << 4) | 0xC);
        ipc.set(5, ipc[8]);
    }
    void set_handles(IPC &ipc) {
        ipc.set(2, 1 << 26);  // copy 2 handles
        ipc.set(3, add_handle(apt.notif));
        ipc.set(4, add_handle(apt.param_ev));
    }
    void get_shared_font(IPC &ipc) {
        if (!apt.font) {
            apt.font = load_shared_font();
        }
        if (!apt.font) {
            LOG("[apt] GetSharedFont: no shared font available\n");
            ipc.result(0xC8A0CFEF);
            return;
        }
        ipc.ok(2, 2);
        ipc.set(2, apt.font->backing);
        ipc.handles(3, {apt.font});
    }
};

void register_apt() {
    register_service(std::make_shared<AptService>("APT:U"));
    register_service(std::make_shared<AptService>("APT:A"));
    register_service(std::make_shared<AptService>("APT:S"));
}


#ifdef RECOMP_RESUMABLE
bool checkpoint_apt(checkpoint::Image &im) {
    if (swkbd_active) return false;
    im.take(apt); im.take(swkbd_wait); im.take(swkbd_serial);
    if (!im.archive) {
        capture_object(im, apt.lock); capture_object(im, apt.font);
        for (auto &p : apt.params) capture_object(im, p.object);
    }
    return true;
}
#endif

}  // namespace kernel

// Exercise the actual native backend against a deterministic CAM SDK, including callbacks and allocation ownership.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <malloc.h>
#include <mutex>
#include <vector>
#include <camera/camera.h>
#include <coreinit/dynload.h>
#include <coreinit/time.h>

static int allocations;
static void *test_memalign(size_t alignment, size_t size) {
#ifdef _WIN32
    void *p = _aligned_malloc(size, alignment);
#else
    void *p = nullptr;
    if (posix_memalign(&p, alignment, size)) return nullptr;
#endif
    if (p) allocations++;
    return p;
}
static void test_free(void *p) {
    if (!p) return;
    allocations--;
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}
#define memalign test_memalign
#define free test_free
#include "../port/wiiu/camera_wiiu.cpp"
#undef free
#undef memalign

namespace sdk {
CAMEventHandler callback;
CAMSurface *targets[2] = {};
bool queued[2] = {}, opened = false;
int initializations, exits, closes, opens, loads, releases, next_handle = 40;
CAMHandle live = -1;
CAMError open_error = CAMERA_ERROR_OK, submit_error = CAMERA_ERROR_OK;
OSTime time = 100;
int id(void *buffer) {
    for (int i = 0; i < 2; i++) if (targets[i] && targets[i]->surfaceBuffer == buffer) return i;
    for (int i = 0; i < 2; i++) if (!targets[i]) return i;
    assert(false); return -1;
}
CAMHandle init(int instance, CAMSetupInfo *setup, CAMError *error) {
    assert(instance == 0 && setup->mode.forceDrc == CAMERA_FORCE_DRC_OFF && setup->mode.fps == CAMERA_FPS_30);
    assert(setup->streamInfo.width == 640 && setup->streamInfo.height == 480 && setup->workMem.pMem);
    assert(setup->threadAffinity == OS_THREAD_ATTRIB_AFFINITY_ANY);
    callback = setup->eventHandler;
    *error = CAMERA_ERROR_OK;
    initializations++;
    live = ++next_handle;
    return live;
}
void exit(CAMHandle handle) { assert(handle == live); exits++; live = -1; }
CAMError open(CAMHandle handle) { assert(handle == live); opens++; opened = open_error == CAMERA_ERROR_OK; return open_error; }
CAMError close(CAMHandle handle) {
    assert(handle == live); closes++; opened = false;
    queued[0] = queued[1] = false;
    return CAMERA_ERROR_OK;
}
int32_t mem_req(CAMStreamInfo *) { return 4096; }
CAMError check_mem(void *data, uint32_t size) { assert(data && size && !(reinterpret_cast<uintptr_t>(data) & 255)); return CAMERA_ERROR_OK; }
CAMError submit(CAMHandle handle, CAMSurface *surface) {
    assert(handle == live && opened);
    assert(surface->surfaceSize == CAMERA_YUV_BUFFER_SIZE && surface->width == 640 && surface->height == 480);
    assert(surface->pitch == 768 && surface->alignment == 256 && !surface->tileMode && !surface->pixelFormat);
    if (submit_error) return submit_error;
    const int i = id(surface->surfaceBuffer);
    assert(!queued[i]);
    targets[i] = surface;
    queued[i] = true;
    // A queued DMA surface can change immediately; a consumer must finish copying before requeueing it.
    memset(surface->surfaceBuffer, 0xcc, CAMERA_YUV_BUFFER_SIZE);
    return CAMERA_ERROR_OK;
}
void frame(int i, unsigned char value, bool failed = false) {
    assert(queued[i]);
    queued[i] = false;
    memset(targets[i]->surfaceBuffer, value, CAMERA_YUV_BUFFER_SIZE);
    CAMEventData data = {};
    data.eventType = CAMERA_DECODE_DONE;
    data.decode.surfaceBuffer = targets[i]->surfaceBuffer;
    data.decode.handle = live;
    data.decode.failed = failed;
    callback(&data);
}
}
void rt_log(const char *, ...) {}
void DCInvalidateRange(void *buffer, uint32_t size) { assert(buffer && size == CAMERA_YUV_BUFFER_SIZE); }
void DCFlushRange(void *buffer, uint32_t size) { assert(buffer && size == CAMERA_YUV_BUFFER_SIZE); }
OSTime OSGetSystemTime() { return sdk::time; }
int OSDynLoad_Acquire(const char *name, OSDynLoad_Module *out) { assert(!strcmp(name, "camera.rpl")); sdk::loads++; *out = 7; return OS_DYNLOAD_OK; }
void OSDynLoad_Release(OSDynLoad_Module module) { assert(module == 7); sdk::releases++; }
int OSDynLoad_FindExport(OSDynLoad_Module module, int kind, const char *name, void **out) {
    assert(module == 7 && kind == OS_DYNLOAD_EXPORT_FUNC);
    if (!strcmp(name, "CAMInit")) *out = (void *)sdk::init;
    else if (!strcmp(name, "CAMExit")) *out = (void *)sdk::exit;
    else if (!strcmp(name, "CAMOpen")) *out = (void *)sdk::open;
    else if (!strcmp(name, "CAMClose")) *out = (void *)sdk::close;
    else if (!strcmp(name, "CAMGetMemReq")) *out = (void *)sdk::mem_req;
    else if (!strcmp(name, "CAMSubmitTargetSurface")) *out = (void *)sdk::submit;
    else if (!strcmp(name, "CAMCheckMemSegmentation")) *out = (void *)sdk::check_mem;
    else assert(false);
    return OS_DYNLOAD_OK;
}

int main() {
    wiiu::camera_pause(false);
    assert(sdk::loads == 0);
    assert(cam_host_start() && sdk::initializations == 1 && allocations == 3);
    assert(cam_host_start() && sdk::initializations == 1);
    std::vector<u8> output(CAMERA_YUV_BUFFER_SIZE + 16, 0xa5);
    assert(!cam_host_frame(output.data(), CAMERA_YUV_BUFFER_SIZE));
    sdk::frame(0, 10);
    sdk::frame(1, 20);
    assert(!cam_host_frame(output.data(), CAMERA_YUV_BUFFER_SIZE - 1));
    assert(cam_host_frame(output.data(), CAMERA_YUV_BUFFER_SIZE));
    assert(std::all_of(output.begin(), output.begin() + CAMERA_YUV_BUFFER_SIZE, [](u8 b) { return b == 20; }));
    assert(std::all_of(output.begin() + CAMERA_YUV_BUFFER_SIZE, output.end(), [](u8 b) { return b == 0xa5; }));
    assert(!cam_host_frame(output.data(), CAMERA_YUV_BUFFER_SIZE));
    sdk::frame(0, 30, true);
    assert(!cam_host_frame(output.data(), CAMERA_YUV_BUFFER_SIZE));
    assert(sdk::queued[0]);
    wiiu::camera_pause(true);
    assert(!cam_host_frame(output.data(), CAMERA_YUV_BUFFER_SIZE) && sdk::initializations == 1);
    wiiu::camera_pause(false);
    assert(!cam_host_frame(output.data(), CAMERA_YUV_BUFFER_SIZE) && sdk::initializations == 2 && sdk::exits == 1);
    CAMEventData detach = {};
    detach.eventType = CAMERA_DRC_DETACH;
    detach.detach.handle = sdk::live;
    sdk::callback(&detach);
    sdk::open_error = CAMERA_ERROR_UVC;
    assert(!cam_host_frame(output.data(), CAMERA_YUV_BUFFER_SIZE));
    const int tries = sdk::initializations;
    assert(!cam_host_frame(output.data(), CAMERA_YUV_BUFFER_SIZE) && sdk::initializations == tries);
    sdk::time += 1001;
    sdk::open_error = CAMERA_ERROR_OK;
    assert(cam_host_start() && sdk::initializations == tries + 1);
    cam_host_stop();
    assert(!cam_host_frame(output.data(), CAMERA_YUV_BUFFER_SIZE));
    sdk::submit_error = CAMERA_ERROR_NOT_READY;
    assert(!cam_host_start());
    sdk::time += 1001;
    sdk::submit_error = CAMERA_ERROR_OK;
    assert(cam_host_start());
    wiiu::camera_shutdown();
    assert(!allocations && sdk::releases == 1 && !cam_host_start());
    sdk::callback(&detach);  // a stale notification cannot touch released frame allocations
}

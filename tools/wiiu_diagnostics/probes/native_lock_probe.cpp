#include <whb/proc.h>
#include <coreinit/debug.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>
#include <coreinit/interrupts.h>
#include <coreinit/spinlock.h>
static OSSpinLock testLock;
static OSThread otherThread;
#define MEMORY_HEAP0 ((uintptr_t)&testLock - 0x20)
#define MEMORY_HEAP1 0x70000000
#define MEMORY_HEAP2 0x71000000
#define MEMORY_HEAP3 0x72000000
#define MEMORY_START_VIDEO 0x73000000
#include "../MemoryMappingModule/source/mapped_lock_trace.h"
static function_replacement_data_t patches[]={
 REPLACE_FUNCTION(OSUninterruptibleSpinLock_TryAcquire, LIBRARY_COREINIT, OSUninterruptibleSpinLock_TryAcquire),
 REPLACE_FUNCTION(OSUninterruptibleSpinLock_TryAcquireWithTimeout, LIBRARY_COREINIT, OSUninterruptibleSpinLock_TryAcquireWithTimeout),
 REPLACE_FUNCTION(OSTryAcquireSpinLock, LIBRARY_COREINIT, OSTryAcquireSpinLock),
 REPLACE_FUNCTION(OSTryAcquireSpinLockWithTimeout, LIBRARY_COREINIT, OSTryAcquireSpinLockWithTimeout),
 REPLACE_FUNCTION(OSInitSpinLock, LIBRARY_COREINIT, OSInitSpinLock),

 REPLACE_FUNCTION(OSUninterruptibleSpinLock_Acquire, LIBRARY_COREINIT, OSUninterruptibleSpinLock_Acquire),
 REPLACE_FUNCTION(OSUninterruptibleSpinLock_Release, LIBRARY_COREINIT, OSUninterruptibleSpinLock_Release),
 REPLACE_FUNCTION(OSAcquireSpinLock, LIBRARY_COREINIT, OSAcquireSpinLock),
 REPLACE_FUNCTION(OSReleaseSpinLock, LIBRARY_COREINIT, OSReleaseSpinLock),
};
int main(){
 WHBProcInit();flight::start();OSInitSpinLock(&testLock);
 bool ok=FunctionPatcher_InitLibrary()==FUNCTION_PATCHER_RESULT_SUCCESS;
 PatchedFunctionHandle handles[9]{};
 for(unsigned i=0;i<9&&ok;i++){
  bool patched=false;
  ok=FunctionPatcher_AddFunctionPatch(&patches[i],&handles[i],&patched)==FUNCTION_PATCHER_RESULT_SUCCESS&&patched;
 }
 if(!ok){OSReport("NATIVE_LOCK_PROBE_PATCH_FAILED\n");return 1;}
 for (int requested=0;requested<2;requested++) {
 OSRestoreInterrupts(requested);
 const BOOL irq=OSIsInterruptEnabled();
 ok=ok&&irq==requested;
 ok=ok&&OSUninterruptibleSpinLock_Acquire(&testLock);
 ok=ok&&testLock.owner==(uint32_t)OSGetCurrentThread();
 ok=ok&&OSUninterruptibleSpinLock_Acquire(&testLock)&&testLock.recursion==1;
 ok=ok&&OSUninterruptibleSpinLock_Release(&testLock)&&testLock.recursion==0;
 ok=ok&&OSUninterruptibleSpinLock_Release(&testLock)&&testLock.owner==0;
 ok=ok&&OSIsInterruptEnabled()==irq;
 ok=ok&&OSAcquireSpinLock(&testLock)&&OSReleaseSpinLock(&testLock)&&testLock.owner==0;
 }
 // Private synthetic lock only: successful and busy try-lock calls on each API.
 OSInitSpinLock(&testLock);
 ok=ok&&OSUninterruptibleSpinLock_TryAcquire(&testLock)&&OSUninterruptibleSpinLock_Release(&testLock);
 ok=ok&&OSUninterruptibleSpinLock_TryAcquireWithTimeout(&testLock,0)&&OSUninterruptibleSpinLock_Release(&testLock);
 ok=ok&&OSTryAcquireSpinLock(&testLock)&&OSReleaseSpinLock(&testLock);
 ok=ok&&OSTryAcquireSpinLockWithTimeout(&testLock,0)&&OSReleaseSpinLock(&testLock);
 testLock.owner=(uint32_t)&otherThread;
 ok=ok&&!OSUninterruptibleSpinLock_TryAcquire(&testLock)&&OSIsInterruptEnabled();
 ok=ok&&!OSUninterruptibleSpinLock_TryAcquireWithTimeout(&testLock,0)&&OSIsInterruptEnabled();
 ok=ok&&!OSTryAcquireSpinLock(&testLock)&&OSIsInterruptEnabled();
 ok=ok&&!OSTryAcquireSpinLockWithTimeout(&testLock,0)&&OSIsInterruptEnabled();
 testLock.owner=0;
 unsigned count=0;for(unsigned c=0;c<3;c++)count+=flight::load(&flight::rings[c].head);
 ok=ok&&count==50;
 OSReport("NATIVE_LOCK_PROBE_%s transitions=%u irq=%u\n",ok?"PASS":"FAIL",count,OSIsInterruptEnabled());
 for(auto handle:handles)FunctionPatcher_RemoveFunctionPatch(handle);
 OSSleepTicks(OSMillisecondsToTicks(500));flight::stop();
 while(WHBProcIsRunning())OSSleepTicks(OSMillisecondsToTicks(100));
 WHBProcShutdown();return ok?0:1;
}

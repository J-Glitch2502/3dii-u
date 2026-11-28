#include <whb/proc.h>
#include <coreinit/debug.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/interrupts.h>
#include <coreinit/memexpheap.h>
#include <coreinit/memdefaultheap.h>
#include <cstring>
#include <cstdint>
alignas(64) unsigned char heapStorage[1024*1024];
#define MAPPED_HEAP_GUARD_TEST 1
#define MEMORY_HEAP0 ((uintptr_t)heapStorage)
#define MEMORY_HEAP1 0x70000000
#define MEMORY_HEAP2 0x71000000
#define MEMORY_HEAP3 0x72000000
#define MEMORY_START_VIDEO 0x73000000
#include "../MemoryMappingModule/source/mapped_heap_hooks.h"
static function_replacement_data_t patches[]={
 REPLACE_FUNCTION(MEMAllocFromExpHeapEx, LIBRARY_COREINIT, MEMAllocFromExpHeapEx),
 REPLACE_FUNCTION(MEMFreeToExpHeap, LIBRARY_COREINIT, MEMFreeToExpHeap),
 REPLACE_FUNCTION(MEMResizeForMBlockExpHeap, LIBRARY_COREINIT, MEMResizeForMBlockExpHeap),
 REPLACE_FUNCTION(MEMGetAllocatableSizeForExpHeapEx, LIBRARY_COREINIT, MEMGetAllocatableSizeForExpHeapEx),
 REPLACE_FUNCTION(MEMGetTotalFreeSizeForExpHeap, LIBRARY_COREINIT, MEMGetTotalFreeSizeForExpHeap),
 REPLACE_FUNCTION(MEMSetAllocModeForExpHeap, LIBRARY_COREINIT, MEMSetAllocModeForExpHeap),
 REPLACE_FUNCTION(MEMGetAllocModeForExpHeap, LIBRARY_COREINIT, MEMGetAllocModeForExpHeap),
 REPLACE_FUNCTION(MEMSetGroupIDForExpHeap, LIBRARY_COREINIT, MEMSetGroupIDForExpHeap),
 REPLACE_FUNCTION(MEMGetGroupIDForExpHeap, LIBRARY_COREINIT, MEMGetGroupIDForExpHeap),
 REPLACE_FUNCTION(MEMAdjustExpHeap, LIBRARY_COREINIT, MEMAdjustExpHeap),
 REPLACE_FUNCTION(MEMVisitAllocatedForExpHeap, LIBRARY_COREINIT, MEMVisitAllocatedForExpHeap),
 REPLACE_FUNCTION(MEMDestroyExpHeap, LIBRARY_COREINIT, MEMDestroyExpHeap),
 REPLACE_FUNCTION(MEMCheckExpHeap, LIBRARY_COREINIT, MEMCheckExpHeap),
 REPLACE_FUNCTION(MEMDumpHeap, LIBRARY_COREINIT, MEMDumpHeap),
 REPLACE_FUNCTION(MEMCheckHeap, LIBRARY_COREINIT, MEMCheckHeap),
};
static OSThread workers[3];
alignas(32) unsigned char stacks[3][32768];
static MEMHeapHandle heap;
static uint32_t failed=0, completed=0;
int work(int id,void*) {
 for(int i=0;i<10000;i++) {
  const unsigned size=64+((i+id*17)%256);
  auto *p=(unsigned char*)MEMAllocFromExpHeapEx(heap,size,64);
  if(!p || ((uintptr_t)p&63)) {__atomic_store_n(&failed,1,__ATOMIC_RELAXED);break;}
  memset(p,id+1,size);
  if((i%7)==0) {
   // In-place growth may legitimately fail when another worker owns the next block.
   MEMResizeForMBlockExpHeap(heap,p,size+32);
   MEMGetTotalFreeSizeForExpHeap(heap);MEMGetAllocatableSizeForExpHeapEx(heap,64);
  }
  for(unsigned j=0;j<size;j++)if(p[j]!=id+1)__atomic_store_n(&failed,1,__ATOMIC_RELAXED);
  MEMFreeToExpHeap(heap,p);
  __atomic_add_fetch(&completed,1,__ATOMIC_RELAXED);
 }
 return 0;
}
static unsigned visited=0;
void visit(void *block,MEMHeapHandle h,void*) {
 // Reenter the same mutex from the callback; must be recursive.
 mapped_heap::Guard guard(h);if(block)++visited;
}
int main(){
 WHBProcInit();mapped_heap::initialize();
 heap=MEMCreateExpHeapEx(heapStorage,sizeof heapStorage,0);
 bool ok=heap!=nullptr&&!(heap->flags & MEM_HEAP_FLAG_USE_LOCK);
 const auto before=MEMGetTotalFreeSizeForExpHeap(heap);
 ok=ok&&FunctionPatcher_InitLibrary()==FUNCTION_PATCHER_RESULT_SUCCESS;
 PatchedFunctionHandle handles[sizeof(patches)/sizeof(patches[0])]{};
 for(unsigned i=0;i<sizeof(patches)/sizeof(patches[0]);i++){
  bool patched=false;
  bool added=FunctionPatcher_AddFunctionPatch(&patches[i],&handles[i],&patched)==FUNCTION_PATCHER_RESULT_SUCCESS&&patched;
  OSReport("HEAP_MUTEX_PATCH_%u_%s\n",i,added?"OK":"FAILED");ok=ok&&added;
 }
 if(!ok){OSReport("HEAP_MUTEX_PROBE_SETUP_FAILED\n");return 1;}
 // Unrelated default heap must remain usable and pass through unchanged.
 auto *unrelated=MEMAllocFromDefaultHeapEx(128,32);ok=ok&&unrelated;MEMFreeToDefaultHeap(unrelated);
 MEMSetGroupIDForExpHeap(heap,42);ok=ok&&MEMGetGroupIDForExpHeap(heap)==42;
 auto mode=MEMGetAllocModeForExpHeap(heap);MEMSetAllocModeForExpHeap(heap,mode);
 {mapped_heap::Guard outer(heap);mapped_heap::Guard inner(heap);
  auto *p=MEMAllocFromExpHeapEx(heap,64,64);ok=ok&&p;
  MEMVisitAllocatedForExpHeap(heap,visit,nullptr);ok=ok&&visited==1;MEMFreeToExpHeap(heap,p);
 }
 for(int i=0;i<3;i++){
  ok=ok&&OSCreateThread(&workers[i],work,i,nullptr,stacks[i]+sizeof(stacks[i]),sizeof(stacks[i]),16,(OSThreadAttributes)(1<<i));
  OSResumeThread(&workers[i]);
 }
 for(auto &thread:workers)OSJoinThread(&thread,nullptr);
 const auto after=MEMGetTotalFreeSizeForExpHeap(heap);
 ok=ok&&!failed&&completed==30000&&before==after&&MEMCheckExpHeap(heap,(MEMExpHeapCheckFlags)0)&&!heap->lock.owner&&OSIsInterruptEnabled();
 OSReport("HEAP_MUTEX_PROBE_%s iterations=%u free_before=%u free_after=%u owner=%08x\n",ok?"PASS":"FAIL",completed,before,after,heap->lock.owner);
 MEMDestroyExpHeap(heap);for(auto h:handles)FunctionPatcher_RemoveFunctionPatch(h);
 while(WHBProcIsRunning())OSSleepTicks(OSMillisecondsToTicks(100));WHBProcShutdown();return ok?0:1;
}

"""Execute the actual scheduler's post-SVC guard for resumable service sleeps."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
text = (ROOT / 'third_party/3dsrecomp/runtime/kernel.cpp').read_text()
start = text.index('void reschedule(Lock &lk) {')
end = text.index('\nvoid block_current(', start)
function = text[start:end]
source = r'''
#include <cassert>
#define RECOMP_RESUMABLE
enum class TStatus { Ready, Running, WaitSleep };
struct Thread { TStatus status; int priority; };
struct Lock {};
static Thread *running, *current, *best;
static int switches;
static Thread *current_thread() { return current; }
static Thread *best_ready() { return best; }
static void make_ready(Thread *t, bool) { t->status = TStatus::Ready; }
static void switch_from(Thread *, Lock &) { running=best; best->status=TStatus::Running; ++switches; }
FUNCTION
int main() {
 Lock lk;
 Thread cur{TStatus::WaitSleep,48}, next{TStatus::Running,24}, ready{TStatus::Ready,16};
 current=&cur; running=&next; best=&ready;
 reschedule(lk); // file read already put cur to sleep and selected next
 assert(cur.status==TStatus::WaitSleep && running==&next && switches==0);
 cur.status=TStatus::Running; // already yielded to a selected thread
 reschedule(lk);
 assert(running==&next && switches==0);
 running=&cur; reschedule(lk); // normal preemption remains available
 assert(cur.status==TStatus::Ready && running==&ready && switches==1);
}
'''.replace('FUNCTION', function)
with tempfile.TemporaryDirectory(prefix='checkpoint-scheduler-') as tmp:
    p = Path(tmp)
    (p/'test.cpp').write_text(source)
    subprocess.run(['c++','-O2','-Wall','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS: post-service sleep, already-selected thread, normal priority preemption')

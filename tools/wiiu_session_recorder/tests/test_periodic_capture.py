import json
from pathlib import Path
import sys,tempfile,threading,time,unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import recorder

def wait_for(predicate,timeout=3):
 end=time.monotonic()+timeout
 while time.monotonic()<end:
  if predicate():return
  time.sleep(.01)
 raise AssertionError('Timed out waiting for capture')

class PeriodicTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
  self.interval=patch.object(recorder,'PERIODIC_CAPTURE_SECONDS',.08);self.interval.start();self.addCleanup(self.interval.stop)
  self.retry=patch.object(recorder,'CAPTURE_RETRY_SECONDS',.08);self.retry.start();self.addCleanup(self.retry.stop)
  self.rec=recorder.Recorder(Path(self.tmp.name),'console');self.rec.connection(True)
  self.addCleanup(self.close)
 def close(self):
  self.rec.command('screenshots','off')
  if self.rec.log:self.rec.command('stop')
  for worker in (self.rec.periodic_worker,self.rec.screenshot_worker):
   if worker:worker.join(2)
 def test_backpressure_failure_retry_and_timeline(self):
  gate=threading.Event();entered=threading.Event();calls=[]
  def capture(console,dest):
   calls.append(dest);entered.set();gate.wait(2)
   if len(calls)==1:raise TimeoutError('temporary timeout')
   dest.write_bytes(b'image');return {'capture_token':'1234'}
  with patch.object(recorder,'capture_gamepad',side_effect=capture):
   self.rec.command('start');self.rec.receive(b'AROMA before\n');self.rec.receive_runtime(b'120 [st] frame 120, test\n')
   self.rec.command('screenshots','on');self.assertTrue(entered.wait(2))
   time.sleep(.15)
   self.assertEqual(len(calls),1);self.assertLessEqual(len(self.rec.screenshot_queue),1)
   self.rec.receive(b'AROMA after\n');self.rec.receive_runtime(b'144 [st] frame 144, test\n');gate.set()
   wait_for(lambda:self.rec.command('status')['periodic_saved']>=1)
   self.rec.command('screenshots','off');wait_for(lambda:self.rec.screenshot_worker is None)
   state=self.rec.command('status');self.assertTrue(state['recording']);self.assertGreaterEqual(state['periodic_failed'],1)
   folder=Path(state['session']);events=[json.loads(l) for l in (folder/'events.jsonl').read_text().splitlines()]
   captures=[e for e in events if e['event']=='periodic_screenshot']
   self.assertEqual(captures[0]['status'],'failed');saved=next(e for e in captures if e['status']=='saved')
   self.assertLessEqual(saved['request_runtime_offset'],saved['capture_completed_runtime_offset'])
   self.assertIn('capture_requested_at',saved);self.assertIn('capture_completed_at',saved)
   sidecar=json.loads((folder/saved['screenshot']).with_suffix('.json').read_text())
   self.assertEqual(sidecar['request_runtime_frame'],144);self.assertEqual(sidecar['capture_token'],'1234')
 def test_next_capture_follows_completion_without_poll_delay(self):
  times=[]
  def capture(console,dest):
   times.append(time.monotonic());time.sleep(.03);dest.write_bytes(b'image')
  with patch.object(recorder,'capture_gamepad',side_effect=capture):
   self.rec.command('start');self.rec.command('screenshots','on')
   wait_for(lambda:self.rec.command('status')['periodic_saved']>=3)
   self.rec.command('screenshots','off');wait_for(lambda:self.rec.screenshot_worker is None)
   self.assertTrue(all(.07 < b-a < .6 for a,b in zip(times,times[1:])))
   self.assertGreater(self.rec.command('status')['captures_per_second'],1)
 def test_rotation_keeps_inflight_image_with_its_original_log(self):
  gate=threading.Event();entered=threading.Event()
  def capture(console,dest):entered.set();gate.wait(2);dest.write_bytes(b'image')
  with patch.object(recorder,'capture_gamepad',side_effect=capture):
   old=Path(self.rec.command('start')['session']);self.rec.command('screenshots','on');self.assertTrue(entered.wait(2))
   self.rec.command('stop');self.rec.command('screenshots','off');new=Path(self.rec.command('start')['session']);gate.set()
   wait_for(lambda:self.rec.screenshot_worker is None)
   self.assertEqual(len(list(old.glob('captures/*.png'))),1);self.assertEqual(len(list(new.glob('captures/*.png'))),0)
   events=[json.loads(l) for l in (old/'events.jsonl').read_text().splitlines()]
   self.assertTrue(any(e['event']=='periodic_screenshot' for e in events))
 def test_no_capture_when_idle_or_disconnected(self):
  with patch.object(recorder,'capture_gamepad') as capture:
   self.rec.command('screenshots','on');time.sleep(.12);capture.assert_not_called()
   self.rec.connection(False);self.rec.command('start');time.sleep(.12);capture.assert_not_called()
 def test_manual_marker_and_periodic_capture_are_serialized(self):
  gate=threading.Event();entered=threading.Event();calls=[]
  def capture(console,dest):calls.append(dest);entered.set();gate.wait(2);dest.write_bytes(b'image')
  with patch.object(recorder,'capture_gamepad',side_effect=capture):
   self.rec.command('start');self.rec.command('screenshots','on');self.assertTrue(entered.wait(2));self.rec.command('mark')
   self.rec.command('screenshots','off');self.assertEqual(len(calls),1);gate.set()
   wait_for(lambda:self.rec.screenshot_worker is None);self.assertEqual(len(calls),2)
   self.assertEqual(self.rec.markers,1)

if __name__=='__main__':unittest.main()

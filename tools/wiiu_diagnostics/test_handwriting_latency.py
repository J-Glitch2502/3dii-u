import unittest

from analyze_handwriting_latency import analyze


class LatencyTests(unittest.TestCase):
    def test_delayed_first_update_and_same_frame_refresh(self):
        result = analyze('''
0 [loader] build sample
10 [ink probe] changed addr=222bd900 mode=0 flip=10 unflushed=0 next=30
30 [ink probe] upload addr=222bd900 mode=0 first=10 flip=30 delay_frames=20 delay_us=333862
40 [ink probe] changed addr=222bd900 mode=1 flip=40 unflushed=0 next=60
40 [ink probe] upload addr=222bd900 mode=1 first=40 flip=40 delay_frames=0 delay_us=215
''')
        self.assertEqual(result['modes']['0']['max_ms'], 333.862)
        self.assertEqual(result['modes']['0']['max_delayed_frames'], 20)
        self.assertEqual(result['modes']['1']['median_ms'], 0.215)
        self.assertEqual(result['modes']['1']['max_delayed_frames'], 0)
        self.assertEqual(result['samples'][0]['addr'], 0x222bd900)

    def test_relaunch_does_not_reuse_prior_pending_change(self):
        result = analyze('''
0 [loader] build first
10 [ink probe] changed addr=20000000 mode=0 flip=10 unflushed=1 next=30
0 [loader] build second
1 [ink probe] upload addr=20000000 mode=0 first=1 flip=1 delay_frames=0 delay_us=210
''')
        self.assertEqual(result['samples'][0]['run'], 2)
        self.assertIsNone(result['samples'][0]['unflushed'])
        self.assertEqual(result['modes']['0']['updates_without_learned_unflushed_flag'], 0)

    def test_empty_log_is_not_a_pass(self):
        self.assertEqual(analyze('no probe output')['modes'], {})


if __name__ == '__main__':
    unittest.main()

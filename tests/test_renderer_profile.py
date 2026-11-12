"""Check profile aggregation without needing game data or a GPU."""
import copy
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from profile_renderer import summarize


def window(t, calls=1, avg=2):
    return dict(elapsed_s=t, window_s=1, scene=4, level='Rlevel1_2', paused=False,
                background_mode=False, vsync=False, scale=1, fps_limit=120,
                native_fps=70, render_fps=120, updates_per_s=60, entities=20,
                ping_ms=1, last_applied_motion_age_ms=-1, gpu_timer_valid=False,
                gpu_ms_async=0, stages={'native_render': dict(calls=calls,
                avg_ms=avg, max_ms=avg+1, over33ms=0)}, detail={'roots': {}})


class ProfileTest(unittest.TestCase):
    def test_reports_sampling_scheme_instead_of_conflating_old_evidence(self):
        rows = [window(t) for t in range(22, 30)]
        for row in rows:
            row['detail']['sampling'] = 'rotating'
        self.assertEqual(summarize(rows, 22, 38, 'Rlevel1_2', 1, 120)['detail_sampling'], ['rotating'])
        del rows[0]['detail']['sampling']
        self.assertEqual(summarize(rows, 22, 38, 'Rlevel1_2', 1, 120)['detail_sampling'], ['legacy-fixed', 'rotating'])

    def test_excludes_loading_paused_background_and_wrong_configuration(self):
        rows = [window(t) for t in range(22, 30)]
        for key, value in [('scene', 0), ('paused', True), ('background_mode', True),
                           ('vsync', True), ('scale', 3), ('fps_limit', 60),
                           ('level', 'lobby'), ('elapsed_s', 10)]:
            bad = window(25)
            bad[key] = value
            bad['native_fps'] = 1
            rows.append(bad)
        report = summarize(rows, 22, 38, 'Rlevel1_2', 1, 120)
        self.assertEqual(report['windows'], 8)
        self.assertEqual(report['native_fps']['median'], 70)

    def test_weights_stage_duration_by_call_count_and_excludes_invalid_gpu(self):
        rows = [window(t) for t in range(22, 29)] + [window(29, calls=9, avg=10)]
        rows[-1].update(gpu_timer_valid=True, gpu_ms_async=3)
        report = summarize(rows, 22, 38, 'Rlevel1_2', 1, 120)
        self.assertEqual(report['stages']['native_render']['avg_ms'], 6.5)
        self.assertEqual(report['gpu_ms_async'], {'valid_windows': 1, 'median': 3})
        self.assertIsNone(report['last_applied_motion_age_ms'])

    def test_event_rates_include_windows_with_no_events(self):
        rows = [window(t) for t in range(22, 30)]
        rows[-1]['window_s'] = 2
        rows[-1]['stages']['present_reused'] = dict(calls=18, avg_ms=0, max_ms=0, over33ms=0)
        report = summarize(rows, 22, 38, 'Rlevel1_2', 1, 120)
        self.assertEqual(report['measured_seconds'], 9)
        self.assertEqual(report['stages']['present_reused']['calls_per_s'], 2)

    def test_detail_uses_samples_not_calls_or_seen_frames(self):
        rows = [window(t) for t in range(22, 30)]
        root = dict(seen=160, samples=10, cpu_samples=10, wall_ms=80,
                    thread_cpu_ms=50, unattributed_self_ms=20, spans={'vertex_decode': dict(self_ms=30)},
                    work={'repeated_executes': 7, 'vertex_bytes': 10240})
        for r in rows:
            r['detail']['roots']['guest_render'] = copy.deepcopy(root)
        report = summarize(rows, 22, 38, 'Rlevel1_2', 1, 120)['detail']['guest_render']
        self.assertEqual(report['wall_ms_per_sample'], 8)
        self.assertEqual(report['thread_cpu_ms_per_sample'], 5)
        self.assertEqual(report['unattributed_self_ms_per_sample'], 2)
        self.assertEqual(report['self_ms_per_sample']['vertex_decode'], 3)
        self.assertEqual(report['work_per_sample']['repeated_executes'], .7)
        self.assertEqual(report['work_per_sample']['vertex_bytes'], 1024)

    def test_insufficient_gameplay_does_not_produce_a_successful_report(self):
        with self.assertRaises(ValueError):
            summarize([window(25)], 22, 38, 'Rlevel1_2', 1, 120)

    def test_scene_alone_does_not_establish_ready_participants(self):
        rows = [window(t) for t in range(22, 30)]
        for r in rows:
            r.update(local_ready=True, ready_players=2)
        for local, count in [(False, 2), (True, 1), (True, 3)]:
            bad = window(25)
            bad.update(local_ready=local, ready_players=count, render_fps=999)
            rows.append(bad)
        report = summarize(rows, 22, 38, 'Rlevel1_2', 1, 120, 2)
        self.assertEqual(report['windows'], 8)
        self.assertEqual(report['render_fps']['median'], 120)
        with self.assertRaises(ValueError):
            summarize([window(t) for t in range(22, 30)], 22, 38, 'Rlevel1_2', 1, 120, 2)


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Parser fixtures only: ensure incomplete evidence cannot pass as a GPU run."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('native_runner', Path(__file__).resolve().parents[1] / 'run_native.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def fixtures():
    order = '\n'.join(f'E12_ORDER_PASS: test={n} old_and_new_edges=checked' for n in range(12))
    # Matches the accepted E28 limitation of the old long probes.
    order += '\n' + '\n'.join(f'E12_OVERLAP: round={n} graphics_still_pending=0' for n in range(16))
    life = '\n'.join(f'E16_LIFETIME_PASS: kind={k} round={n} old_and_new_data=checked'
                     for k in range(3) for n in range(12))
    neighbor = '\n'.join(
        f'E22_CASE_PASS: case={k} round={n} reader_started_before_bind=1 '
        'gpu_new_mapping_checked=1 full_post_checked=1 no_rebind_pending=1 '
        'reader_pending_after_gpu=1 probe_bytes=32 probe_regions=4'
        for k in runner.CASES for n in range(12))
    return order, life, neighbor


class ValidationTests(unittest.TestCase):
    def test_accepted_long_probe_limitation_uses_tiny_probe_coverage(self):
        result = runner.validate_logs(*fixtures())
        self.assertEqual(result['legacy_graphics_pending'], 0)
        self.assertEqual(result['neighbors'], dict.fromkeys(runner.CASES, 12))

    def test_missing_or_duplicate_waits_fail(self):
        order, life, neighbor = fixtures()
        for bad in (order.replace('test=11 ', 'test=10 '), order.replace('old_and_new_edges=checked', 'old_and_new_edges=missing', 1)):
            with self.assertRaises(RuntimeError):
                runner.validate_logs(bad, life, neighbor)

    def test_missing_lifetime_case_fails(self):
        order, life, neighbor = fixtures()
        with self.assertRaises(RuntimeError):
            runner.validate_logs(order, life.split('\n', 1)[1], neighbor)

    def test_tiny_probe_controls_data_and_overlap_are_required(self):
        order, life, neighbor = fixtures()
        for field in ('reader_started_before_bind', 'gpu_new_mapping_checked', 'full_post_checked',
                      'no_rebind_pending', 'reader_pending_after_gpu'):
            with self.assertRaises(RuntimeError):
                runner.validate_logs(order, life, neighbor.replace(field + '=1', field + '=0', 1))
        for old, new in (('probe_bytes=32', 'probe_bytes=65536'), ('probe_regions=4', 'probe_regions=4096'),
                         ('case=replace_4m round=11', 'case=replace_4m round=10')):
            with self.assertRaises(RuntimeError):
                runner.validate_logs(order, life, neighbor.replace(old, new, 1))


if __name__ == '__main__':
    unittest.main()

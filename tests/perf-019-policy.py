#!/usr/bin/env python3
"""Non-destructive contract tests for the operator-only PERF-019 collector."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / 'tests' / 'perf-019-qualification.py'
spec = importlib.util.spec_from_file_location('perf019_collector', PROGRAM)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class QualificationSafety(unittest.TestCase):
    def cli(self, *arguments):
        return subprocess.run([sys.executable, str(PROGRAM), *arguments],
                              text=True, capture_output=True, check=False)

    def test_help_and_bounds(self):
        self.assertEqual(self.cli('--help').returncode, 0)
        result = self.cli('run', '--mount', '/', '--report', '/tmp/perf019.json',
                          '--execute', '--rounds', '11')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('bounded limits', result.stderr)

    def test_root_and_nonmount_refused_before_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            report = base / 'report.json'
            for target in ('/', str(base)):
                result = self.cli('run', '--mount', target, '--report', str(report),
                                  '--execute', '--seq-mib', '1', '--rounds', '2')
                self.assertNotEqual(result.returncode, 0)
            self.assertFalse(report.exists())
            self.assertEqual(list(base.iterdir()), [])

    def test_source_tree_must_not_contain_or_reside_on_mount(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            mount = base / 'mounted'
            mount.mkdir()
            nested = mount / 'source'
            nested.mkdir()
            with self.assertRaises(ValueError):
                module.inventory(base, mount)
            with self.assertRaises(ValueError):
                module.inventory(nested, mount)

    def test_distribution_and_report_path(self):
        values = module.percentiles([5, 1, 2, 4, 3])
        self.assertEqual((values['p50_ms'], values['p95_ms'], values['p99_ms']), (3, 5, 5))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(ValueError):
                module.report_location(root, root / 'report.json')

    def test_false_remount_evidence_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            mount = temp / 'mnt'
            mount.mkdir()
            report = temp / 'report.json'
            identity = {'mount': str(mount), 'source': '/dev/nvme1n1p9',
                        'mount_id': '100', 'boot_id': 'example', 'fstype': 'infiltratorfs'}
            report.write_text(json.dumps({'identity': identity,
                'test_directory': str(mount / '.infiltratorfs-perf019-test'),
                'rounds': [], 'requested_rounds': 1}))
            args = type('Args', (), {'mount': mount, 'report': report})()
            with patch.object(module, 'admission', return_value=identity):
                with self.assertRaisesRegex(ValueError, 'no real unmount/remount'):
                    module.verify(args)


if __name__ == '__main__':
    unittest.main()

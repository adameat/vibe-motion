"""Regression tests for deployment helpers; never touch production paths/hosts."""

import importlib.machinery
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
PACKAGING = ROOT / "packaging"
sys.dont_write_bytecode = True


class PackagingTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vibe-packaging-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.env = dict(os.environ, CAM_ROOT=str(self.root / "cam"),
                        PATH=str(self.bin) + os.pathsep + os.environ["PATH"])
        Path(self.env["CAM_ROOT"]).mkdir()

    def script(self, name, text):
        path = self.bin / name
        path.write_text(text)
        path.chmod(0o755)
        return path

    def run_helper(self, name, *args):
        return subprocess.run([str(PACKAGING / name), *args], env=self.env,
                              text=True, capture_output=True, check=True, timeout=15).stdout

    def test_storage_optional_roots_and_metric_names(self):
        self.env["GRAPHITE_PREFIX"] = "test"
        output = self.run_helper("vibe-motion-storage-graphite-exporter", "--stdout")
        self.assertIn("test.timelapse_count 0 ", output)
        directory = Path(self.env["CAM_ROOT"]) / "timelapse" / "front door.1"
        directory.mkdir(parents=True)
        (directory / "movie.mkv").write_bytes(b"data")
        output = self.run_helper("vibe-motion-storage-graphite-exporter", "--stdout")
        self.assertIn("test.timelapse.front_door_1.size ", output)
        self.assertTrue(all(len(line.split()) == 3 for line in output.splitlines()))

    def test_maintenance_roots_and_both_snapshot_placeholders(self):
        self.script("ffmpeg", '#!/bin/bash\nprintf placeholder > "${@: -1}"\n')
        self.run_helper("vibe-motion-media-maintenance")  # all optional trees absent
        root = Path(self.env["CAM_ROOT"])
        for name in ("timelapse", "events", "archive", "ext"):
            (root / name / "empty").mkdir(parents=True)
        snapshots = root / "snapshot" / "cam15"
        snapshots.mkdir(parents=True)
        (snapshots / "lastsnap.jpg").symlink_to("gone.jpg")
        (snapshots / "lastsnaps.jpg").symlink_to("gones.jpg")
        self.run_helper("vibe-motion-media-maintenance")
        for name in ("timelapse", "events", "archive", "ext"):
            self.assertTrue((root / name).is_dir())
        for name in ("lastsnap.jpg", "lastsnaps.jpg"):
            self.assertEqual((snapshots / name).read_text(), "placeholder")
            (snapshots / name).unlink()
        (snapshots / "snap123.jpg").write_text("large")
        (snapshots / "snap123s.jpg").write_text("small")
        self.run_helper("vibe-motion-media-maintenance")
        self.assertEqual((snapshots / "lastsnap.jpg").read_text(), "large")
        self.assertEqual((snapshots / "lastsnaps.jpg").read_text(), "small")

    def test_archive_uses_local_day_and_accepts_restart_parts(self):
        self.script("date", '#!/bin/bash\nexec /bin/date --date="2026-11-02 01:00:00 UTC" "$@"\n')
        self.env.update(TZ="America/Los_Angeles", TIMELAPSE_ARCHIVE_DRY_RUN="1",
                        TIMELAPSE_ARCHIVE_SOURCE=str(self.root / "cam"),
                        TIMELAPSE_ARCHIVE_LOCK=str(self.root / "archive.lock"))
        directory = self.root / "cam" / "cam15" / "2026" / "11"
        directory.mkdir(parents=True)
        for name in ("cam15-20261101000000.mkv", "cam15-20261031000000.mkv",
                     "cam15-20261031000000-part1.mkv"):
            (directory / name).write_bytes(b"recording")
        output = self.run_helper("vibe-motion-timelapse-archive")
        self.assertIn("selected files=2 ", output)
        self.assertNotIn("would archive cam15/2026/11/cam15-20261101", output)
        self.assertIn("cam15-20261031000000-part1.mkv", output)
        self.assertEqual(len(list(directory.iterdir())), 3)

    def test_worker_timeout(self):
        loader = importlib.machinery.SourceFileLoader("event_worker", str(PACKAGING / "vibe-motion-event-worker"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        module = importlib.util.module_from_spec(spec)
        loader.exec_module(module)
        job = module.JobProcess([sys.executable, "-c", "import time; time.sleep(60)"])
        try:
            self.assertFalse(job.timed_out(job.started_at, 2))
            self.assertTrue(job.timed_out(job.started_at + 3, 2))
        finally:
            job.terminate()
            code, processed = job.finish()
        self.assertNotEqual(code, 0)
        self.assertFalse(processed)

        # An exited parent with a TERM-ignoring child holding stdout must also
        # time out, rather than blocking finish() and the entire queue.
        ready = self.root / "child-ready"
        child = ("import signal,time,pathlib; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                 f"pathlib.Path({str(ready)!r}).touch(); time.sleep(60)")
        parent = f"import subprocess,sys; subprocess.Popen([sys.executable, '-c', {child!r}])"
        orphan_job = module.JobProcess([sys.executable, "-c", parent])
        try:
            deadline = time.monotonic() + 3
            while not ready.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(ready.exists())
            orphan_job.process.wait(timeout=2)
            self.assertIsNone(orphan_job.poll())
            self.assertTrue(orphan_job.timed_out(orphan_job.started_at + 3, 2))
        finally:
            orphan_job.terminate(grace=0.1)
            orphan_job.finish()
        self.assertFalse(orphan_job.thread.is_alive())

    def test_worker_is_opt_in_and_unprivileged(self):
        daemon = (PACKAGING / "vibe-motion.service").read_text()
        worker = (PACKAGING / "vibe-motion-event-compression.service").read_text()
        self.assertIn("\nWants=network-online.target\n", daemon)
        self.assertNotIn("Wants=network-online.target vibe-motion-event-compression.service", daemon)
        self.assertIn("-/run/vibe-motion-event-worker", daemon)
        self.assertIn("\nUser=motion\n", worker)


if __name__ == "__main__":
    unittest.main()

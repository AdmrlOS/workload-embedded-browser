"""Exercise private metadata generation and preservation with isolated fixtures."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "app"))
import input_metadata


class InputMetadataTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.sys_input = self.root / "sys/class/input"
        self.sys_input.mkdir(parents=True)
        self.udev_root = self.root / "run/udev"
        self.udev_root.parent.mkdir()
        self.mountinfo = self.root / "mountinfo"
        self.mountinfo.write_text("")
        self.classifier = self.root / "udevadm"
        self.classifier.write_text("#!/bin/sh\n"
                                   "test \"$1\" = test-builtin && test \"$2\" = input_id || exit 9\n"
                                   "printf 'ID_INPUT=1\\nID_INPUT_KEYBOARD=1\\nDEVNAME=spoof\\nE:ID_SEAT=spoof\\n'\n"
                                   "printf 'ID_INPUT_MOUSE=1\\n' >&2\n")
        self.classifier.chmod(0o755)
        self.environment = patch.dict(os.environ, {"BROWSER_INPUT": "1", "BROWSER_INPUT_SEAT": "seat0"})
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def prepare(self):
        return input_metadata.prepare(sys_input=self.sys_input, udev_root=self.udev_root,
                                      udevadm=self.classifier, mountinfo=self.mountinfo)

    def event(self, name, devnum):
        event = self.sys_input / name
        event.mkdir()
        (event / "dev").write_text(devnum + "\n")
        return event

    def test_classification_ignores_nonproperties_and_leaves_sysfs_untouched(self):
        self.event("event0", "13:64")
        self.event("event1", "13:65")
        malformed = self.event("event2", "../../outside")
        before = {p: p.read_bytes() for p in self.sys_input.glob("event*/dev")}
        report = self.prepare()
        self.assertEqual(report["created"], ["c13:64", "c13:65"])
        self.assertEqual(len(report["errors"]), 1)
        record = (self.udev_root / "data/c13:64").read_text()
        self.assertIn("E:ID_INPUT=1\n", record)
        self.assertIn("E:ID_INPUT_KEYBOARD=1\n", record)
        self.assertIn("E:ID_SEAT=seat0\n", record)
        self.assertNotIn("spoof", record)
        self.assertNotIn("MOUSE", record)
        self.assertEqual(before, {p: p.read_bytes() for p in self.sys_input.glob("event*/dev")})
        self.assertTrue((malformed / "dev").exists())

    def test_existing_database_is_preserved_without_running_classifier(self):
        self.event("event0", "13:64")
        database = self.udev_root / "data"
        database.mkdir(parents=True)
        record = database / "c13:64"
        record.write_text("E:ID_INPUT=1\nE:ID_SEAT=seat9\n")
        self.classifier.unlink()
        report = self.prepare()
        self.assertEqual(report["created"], [])
        self.assertIsNotNone(report["skipped"])
        self.assertEqual(record.read_text(), "E:ID_INPUT=1\nE:ID_SEAT=seat9\n")

    def test_mount_is_preserved_even_when_writable_or_database_absent(self):
        self.event("event0", "13:64")
        self.mountinfo.write_text(f"42 1 0:1 / {self.udev_root} rw - tmpfs tmpfs rw\n")
        report = self.prepare()
        self.assertEqual(report["created"], [])
        self.assertIsNotNone(report["skipped"])
        self.assertFalse(self.udev_root.exists())


if __name__ == "__main__":
    unittest.main()

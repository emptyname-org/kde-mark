import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "src"


class GuiTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.paths = [str(self.root / "first"), str(self.root / "second")]
        for path in self.paths:
            Path(path).touch()

    def run_gui(self, worker, setup="", timeout=30000):
        script = self.root / "worker.py"
        script.write_text(worker)
        source = (
            "import sys\n"
            f"sys.path.insert(0, {str(SOURCE)!r})\n"
            "import mark\n"
            "from PyQt6 import QtCore, QtWidgets\n"
            "QtWidgets.QMessageBox.exec = lambda self: print(self.text()) or 0\n"
            f"mark.__file__ = {str(script)!r}\n"
            + setup
            + f"sys.exit(mark.run_gui({self.paths!r}, 'toggle', 'red', inactivity_timeout={timeout}))\n"
        )
        result = subprocess.run(
            [sys.executable, "-c", source],
            text=True, capture_output=True, timeout=6,
            env={**os.environ, "QT_QPA_PLATFORM": "offscreen"},
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        return result.stdout

    def test_cancel_keeps_completed_files_and_stops_remaining_work(self):
        worker = (
            "import json, sys, time\n"
            f"sys.path.insert(0, {str(SOURCE)!r})\n"
            "import mark\n"
            "paths=json.load(sys.stdin)\n"
            "mark.change_item(paths[0], 'add', 'red')\n"
            "print(json.dumps({'path': paths[0], 'ok': True}), flush=True)\n"
            "time.sleep(5)\n"
            "mark.change_item(paths[1], 'add', 'red')\n"
        )
        setup = (
            "original = QtWidgets.QProgressDialog.setValue\n"
            "def cancel_after_first(self, value):\n"
            " original(self, value)\n"
            " if value == 1: self.canceled.emit()\n"
            "QtWidgets.QProgressDialog.setValue = cancel_after_first\n"
        )
        self.assertIn("Cancelled", self.run_gui(worker, setup))
        self.assertEqual(os.getxattr(self.paths[0], "user.dolphin-color-tags.red"), b"1")
        self.assertNotIn("user.dolphin-color-tags.red", os.listxattr(self.paths[1]))

    def test_timeout_reports_uncertain_outcome(self):
        output = self.run_gui("import time\ntime.sleep(5)\n", timeout=100)
        self.assertIn("Timed out", output)
        self.assertIn("keep their changes", output)

    def test_missing_executable_finishes_with_error(self):
        setup = (
            "start = QtCore.QProcess.start\n"
            "QtCore.QProcess.start = lambda self, *args: start(self, '/missing-mark-worker', [])\n"
        )
        self.assertIn("stopped before finishing", self.run_gui("", setup))

    def test_malformed_reply_is_rejected(self):
        output = self.run_gui("print('not json', flush=True)\n")
        self.assertIn("invalid reply", output)

    def test_partial_failure_is_reported(self):
        worker = (
            "import json, sys\npaths=json.load(sys.stdin)\n"
            "print(json.dumps({'path': paths[0], 'ok': True}), flush=True)\n"
            "print(json.dumps({'path': paths[1], 'error': 'Permission denied'}), flush=True)\n"
            "sys.exit(1)\n"
        )
        self.assertIn("Permission denied", self.run_gui(worker))


if __name__ == "__main__":
    unittest.main()

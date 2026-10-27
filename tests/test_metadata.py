import contextlib
import errno
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import mark as tags


class MetadataTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.path = str(self.root / "example.txt")
        Path(self.path).write_bytes(b"original content")

    def run_worker(self, paths, operation="toggle", color="red"):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = tags.worker(paths, operation, color)
        return code, [json.loads(line) for line in output.getvalue().splitlines()]

    def test_preserves_content_and_other_metadata(self):
        os.setxattr(self.path, "user.xdg.tags", b"important,work")
        before = os.stat(self.path)
        tags.change_item(self.path, "add", "red")
        self.assertEqual(tags.read_item(self.path), ["red"])
        self.assertEqual(Path(self.path).read_bytes(), b"original content")
        self.assertEqual(os.stat(self.path).st_mtime_ns, before.st_mtime_ns)
        self.assertEqual(os.getxattr(self.path, "user.xdg.tags"), b"important,work")

    def test_colours_survive_rename_and_restart(self):
        tags.change_item(self.path, "add", "blue")
        renamed = str(self.root / "renamed.txt")
        os.rename(self.path, renamed)
        result = subprocess.run(
            [sys.executable, tags.__file__, "--worker", "read"],
            input=json.dumps([renamed]), text=True, capture_output=True, check=True,
        )
        self.assertEqual(json.loads(result.stdout)[0]["colors"], ["blue"])

    def test_supports_folders_and_seven_colours(self):
        for color in tags.COLORS:
            tags.change_item(str(self.root), "add", color)
        self.assertEqual(tags.read_item(str(self.root)), list(tags.COLORS))

    def test_repeated_add_and_remove_are_idempotent(self):
        for _ in range(2):
            tags.change_item(self.path, "add", "red")
        tags.change_item(self.path, "add", "green")
        for _ in range(2):
            tags.change_item(self.path, "remove", "red")
        self.assertEqual(tags.read_item(self.path), ["green"])

    def test_clear_only_removes_plugin_metadata(self):
        os.setxattr(self.path, "user.xdg.tags", b"work")
        tags.change_item(self.path, "add", "red")
        tags.change_item(self.path, "clear")
        tags.change_item(self.path, "clear")
        self.assertEqual(tags.read_item(self.path), [])
        self.assertEqual(os.getxattr(self.path, "user.xdg.tags"), b"work")

    def test_mixed_selection_adds_then_removes(self):
        other = str(self.root / "other")
        Path(other).touch()
        tags.change_item(self.path, "add", "red")
        self.assertEqual(self.run_worker([self.path, other])[0], 0)
        self.assertEqual(tags.read_item(other), ["red"])
        self.assertEqual(self.run_worker([self.path, other])[0], 0)
        self.assertEqual(tags.read_item(self.path), [])
        self.assertEqual(tags.read_item(other), [])

    def test_partial_failure_preserves_successes(self):
        code, records = self.run_worker([self.path, str(self.root / "missing")])
        self.assertEqual(code, 1)
        self.assertTrue(records[0]["ok"])
        self.assertIn("error", records[1])
        self.assertEqual(tags.read_item(self.path), ["red"])

    def test_symlink_is_rejected_without_modifying_target(self):
        link = str(self.root / "link")
        os.symlink(self.path, link)
        with self.assertRaises(OSError):
            tags.change_item(link, "add", "red")
        self.assertEqual(tags.read_item(self.path), [])

    def test_fifo_is_rejected_without_waiting_for_writer(self):
        fifo = str(self.root / "fifo")
        os.mkfifo(fifo)
        with self.assertRaises(ValueError):
            tags.change_item(fifo, "add", "red")

    def test_invalid_metadata_can_be_cleared_but_not_toggled(self):
        os.setxattr(self.path, tags.PREFIX + "red", b"future-format")
        self.assertEqual(self.run_worker([self.path])[0], 1)
        self.assertEqual(os.getxattr(self.path, tags.PREFIX + "red"), b"future-format")
        self.assertEqual(self.run_worker([self.path], "clear")[0], 0)
        self.assertEqual(tags.read_item(self.path), [])

    def test_transient_read_failure_does_not_modify_file(self):
        with patch.object(tags.os, "getxattr", side_effect=OSError(errno.EIO, "I/O error")):
            self.assertEqual(self.run_worker([self.path])[0], 1)
        self.assertEqual(tags.read_item(self.path), [])
        self.assertEqual(self.run_worker([self.path])[0], 0)

    def test_unsupported_filesystem_and_permission_errors_are_reported(self):
        for number in (errno.EOPNOTSUPP, errno.EACCES, errno.EROFS):
            with self.subTest(number=number):
                with patch.object(tags.os, "setxattr", side_effect=OSError(number, os.strerror(number))):
                    code, records = self.run_worker([self.path])
                self.assertEqual(code, 1)
                self.assertIn("error", records[0])

    def test_clear_reports_partial_failure(self):
        tags.change_item(self.path, "add", "red")
        tags.change_item(self.path, "add", "green")
        real_remove = os.removexattr

        def remove(fd, attribute):
            if attribute == tags.PREFIX + "green":
                raise OSError(errno.EIO, "I/O error")
            return real_remove(fd, attribute)

        with patch.object(tags.os, "removexattr", side_effect=remove):
            self.assertEqual(self.run_worker([self.path], "clear")[0], 1)
        self.assertEqual(tags.read_item(self.path), ["green"])

    def test_non_ascii_and_shell_characters_are_literal_paths(self):
        path = str(self.root / 'café $(touch BAD); "quote"\nfile')
        Path(path).touch()
        self.assertEqual(self.run_worker([path])[0], 0)
        self.assertEqual(tags.read_item(path), ["red"])
        self.assertFalse((self.root / "BAD").exists())

    def test_request_deduplicates_selection(self):
        request = io.BytesIO(json.dumps([self.path, self.path]).encode())
        self.assertEqual(tags.read_request(request), [self.path])

    def test_rejects_remote_relative_null_and_oversized_requests(self):
        for paths in (["smb://server/file"], ["relative"], ["/bad\0path"], [1], {}, [self.path] * (tags.MAX_FILES + 1)):
            with self.subTest(paths=str(paths)[:50]):
                with self.assertRaises(ValueError):
                    tags.read_request(io.BytesIO(json.dumps(paths).encode()))
        with patch.object(tags, "MAX_INPUT", 16):
            with self.assertRaises(ValueError):
                tags.read_request(io.BytesIO(b" " * 17))

    def test_invalid_operations_do_not_open_files(self):
        with patch.object(tags, "open_item") as open_item:
            for operation, color in (("bad", "red"), ("add", "unknown")):
                with self.assertRaises(ValueError):
                    tags.change_item(self.path, operation, color)
            open_item.assert_not_called()

    def test_independent_concurrent_colours_are_not_lost(self):
        script = (
            "import sys; sys.path.insert(0, sys.argv[1]); import mark; "
            "mark.change_item(sys.argv[2], 'add', sys.argv[3])"
        )
        workers = [subprocess.Popen([sys.executable, "-c", script, str(Path(tags.__file__).parent), self.path, color])
                   for color in tags.COLORS]
        try:
            for process in workers:
                self.assertEqual(process.wait(timeout=5), 0)
        finally:
            for process in workers:
                if process.poll() is None:
                    process.kill()
                process.wait()
        self.assertEqual(tags.read_item(self.path), list(tags.COLORS))

    def test_fd_closes_on_stat_failure(self):
        with patch.object(tags.os, "fstat", side_effect=OSError(errno.EIO, "I/O error")):
            with patch.object(tags.os, "close", wraps=os.close) as close:
                with self.assertRaises(OSError):
                    tags.open_item(self.path)
                close.assert_called_once()


if __name__ == "__main__":
    unittest.main()

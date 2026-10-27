#!/usr/bin/env python3
"""Persistent colour marks and the Dolphin service-menu entry point."""

import argparse
import errno
import json
import os
import stat
import sys

COLORS = {
    "red": "#ed5353",
    "orange": "#f59b35",
    "yellow": "#e8c840",
    "green": "#62b85a",
    "blue": "#4897e8",
    "purple": "#aa70ce",
    "grey": "#929292",
}
# Retain the metadata namespace used by the first preview.
PREFIX = "user.dolphin-color-tags."
MAX_FILES = 8192
MAX_INPUT = 8 * 1024 * 1024


def open_item(path):
    if not isinstance(path, str) or not os.path.isabs(path) or "\0" in path:
        raise ValueError("Expected an absolute local path")
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK | os.O_NOFOLLOW)
    try:
        mode = os.fstat(fd).st_mode
        if not (stat.S_ISREG(mode) or stat.S_ISDIR(mode)):
            raise ValueError("Only regular files and folders can be marked")
    except BaseException:
        os.close(fd)
        raise
    return fd


def read_fd(fd):
    colors = []
    for color in COLORS:
        try:
            value = os.getxattr(fd, PREFIX + color)
        except OSError as error:
            if error.errno == errno.ENODATA:
                continue
            raise
        if value != b"1":
            raise ValueError("Unrecognised colour metadata")
        colors.append(color)
    return colors


def read_item(path):
    fd = open_item(path)
    try:
        return read_fd(fd)
    finally:
        os.close(fd)


def change_item(path, operation, color=None):
    if operation not in ("add", "remove", "clear") or (
        operation != "clear" and color not in COLORS
    ):
        raise ValueError("Invalid colour operation")
    fd = open_item(path)
    try:
        for current in COLORS if operation == "clear" else [color]:
            if operation == "add":
                os.setxattr(fd, PREFIX + current, b"1")
            else:
                try:
                    os.removexattr(fd, PREFIX + current)
                except OSError as error:
                    if error.errno != errno.ENODATA:
                        raise
    finally:
        os.close(fd)


def read_request(stream):
    raw = stream.read(MAX_INPUT + 1)
    if len(raw) > MAX_INPUT:
        raise ValueError("Selection is too large")
    paths = json.loads(raw)
    if not isinstance(paths, list) or len(paths) > MAX_FILES:
        raise ValueError(f"Select at most {MAX_FILES} files")
    if any(not isinstance(path, str) or not path.startswith("/") or "\0" in path for path in paths):
        raise ValueError("Expected absolute local paths")
    return list(dict.fromkeys(paths))


def describe_error(error):
    if isinstance(error, OSError):
        if error.errno in (errno.ENOTSUP, errno.EOPNOTSUPP):
            return "This filesystem does not support colour metadata"
        if error.errno == errno.ELOOP:
            return "Symbolic links cannot be marked; select the target instead"
        return error.strerror or "Filesystem error"
    return str(error)


def worker(paths, operation, color):
    # Read the whole selection before toggling: mixed selections gain the colour.
    records = []
    for path in paths:
        try:
            records.append({"path": path, "colors": read_item(path)})
        except (OSError, ValueError) as error:
            records.append({"path": path, "error": describe_error(error)})
    if operation == "read":
        print(json.dumps(records), flush=True)
        return 0
    if operation == "toggle":
        valid = [record for record in records if "error" not in record]
        operation = "remove" if valid and all(color in r["colors"] for r in valid) else "add"
    failures = 0
    for record in records:
        # Clear can repair malformed metadata; ordinary toggles must not erase it.
        if "error" not in record or operation == "clear":
            try:
                change_item(record["path"], operation, color)
                record = {"path": record["path"], "ok": True}
            except (OSError, ValueError) as error:
                record = {"path": record["path"], "error": describe_error(error)}
        failures += "error" in record
        print(json.dumps(record), flush=True)
    return 1 if failures else 0


def run_gui(paths, operation, color, *, inactivity_timeout=30000):
    from PyQt6.QtCore import QProcess, QTimer, Qt
    from PyQt6.QtDBus import QDBusConnection, QDBusMessage
    from PyQt6.QtWidgets import QApplication, QMessageBox, QProgressDialog

    app = QApplication([sys.argv[0]])
    app.setQuitOnLastWindowClosed(False)
    progress = QProgressDialog("Updating marks…", "Cancel", 0, len(paths))
    progress.setWindowTitle("Mark")
    progress.setMinimumDuration(400)
    progress.setAutoClose(False)
    process = QProcess(app)
    deadline = QTimer(app)
    deadline.setSingleShot(True)
    pending = bytearray()
    failures = []
    completed = 0
    stopped = ""
    finished = False
    result_code = 0

    def notify(items):
        message = QDBusMessage.createSignal(
            "/Mark", "org.local.DolphinMark", "Changed"
        )
        # Explicit QStringList signature, including when the list is empty.
        from PyQt6.QtCore import QMetaType, QVariant

        argument = QVariant(items)
        argument.convert(QMetaType(QMetaType.Type.QStringList.value))
        message.setArguments([argument])
        QDBusConnection.sessionBus().send(message)

    def receive():
        nonlocal completed
        pending.extend(bytes(process.readAllStandardOutput()))
        if len(pending) > MAX_INPUT:
            stop("The Mark worker returned too much data.")
            return
        while b"\n" in pending:
            line, _, rest = pending.partition(b"\n")
            pending[:] = rest
            try:
                record = json.loads(line)
                path = record["path"]
                if path not in selected:
                    raise ValueError("Unexpected file")
            except (ValueError, KeyError, TypeError):
                stop("The Mark worker returned an invalid reply.")
                return
            completed += 1
            if "error" in record and len(failures) < 12:
                failures.append(f"{os.path.basename(path)}: {record['error']}")
            notify([path])
            progress.setValue(completed)
            deadline.start(inactivity_timeout)

    def stop(reason):
        nonlocal stopped
        if finished:
            return
        stopped = reason
        process.kill()

    def finish(code=1, _status=None):
        nonlocal finished, result_code
        if finished:
            return
        receive()
        finished = True
        deadline.stop()
        progress.close()
        # Also invalidate files with an uncertain outcome after cancellation.
        for offset in range(0, len(paths), 128):
            notify(paths[offset : offset + 128])
        if stopped:
            detail = stopped + "\nFiles already processed keep their changes."
        elif failures:
            detail = "Some files could not be marked:\n" + "\n".join(failures)
            if completed > len(failures):
                detail += "\nOther files may have been updated."
        elif code != 0 or completed != len(paths):
            detail = "The Mark worker stopped before finishing. Some files may have been updated."
        else:
            detail = ""
        result_code = int(bool(detail))
        if detail:
            box = QMessageBox(QMessageBox.Icon.Warning, "Mark", detail)
            box.setTextFormat(Qt.TextFormat.PlainText)
            box.exec()
        app.quit()

    selected = set(paths)
    process.readyReadStandardOutput.connect(receive)
    process.finished.connect(finish)
    process.errorOccurred.connect(
        lambda error: finish() if error == QProcess.ProcessError.FailedToStart else None
    )
    progress.canceled.connect(lambda: stop("Cancelled."))
    deadline.timeout.connect(lambda: stop("Timed out waiting for the filesystem."))
    process.started.connect(
        lambda: (process.write(json.dumps(paths).encode()), process.closeWriteChannel())
    )
    process.start(sys.executable, [os.path.abspath(__file__), "--worker", operation, "--color", color])
    deadline.start(inactivity_timeout)
    app.exec()
    return result_code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", choices=("read", "toggle", "clear"))
    parser.add_argument("--toggle", choices=COLORS)
    parser.add_argument("--clear", action="store_true")
    parser.add_argument("--color", choices=COLORS, default="red")
    parser.add_argument("paths", nargs="*")
    args = parser.parse_args()
    try:
        if args.worker:
            return worker(read_request(sys.stdin.buffer), args.worker, args.color)
        if bool(args.toggle) == args.clear:
            parser.error("Choose --toggle COLOUR or --clear")
        paths = read_request(__import__("io").BytesIO(json.dumps(args.paths).encode()))
        if not paths:
            parser.error("Select at least one file")
        return run_gui(paths, "toggle" if args.toggle else "clear", args.toggle or "red")
    except (OSError, ValueError) as error:
        print(describe_error(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Open an isolated Dolphin window using the built plugin and sample files."""

import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from mark import COLORS, change_item  # noqa: E402


def main():
    build = ROOT / "tmp" / "build"
    plugin = build / "plugins" / "kf6" / "overlayicon" / "dolphin_mark.so"
    if not plugin.is_file():
        raise SystemExit("Run make build first.")
    preview = ROOT / "tmp" / "preview"
    data = preview / "data"
    for path in (data, preview / "config", preview / "cache", preview / "files"):
        path.mkdir(parents=True, exist_ok=True)
    helper = data / "kde-mark" / "mark.py"
    helper.parent.mkdir(parents=True, exist_ok=True)
    helper.write_bytes((ROOT / "src" / "mark.py").read_bytes())
    menu = data / "kio" / "servicemenus" / "mark.desktop"
    menu.parent.mkdir(parents=True, exist_ok=True)
    # Escape characters reserved by desktop-file Exec parsing.
    escaped = str(data).replace("\\", "\\\\\\\\").replace('"', '\\\\"').replace("`", "\\\\`").replace("$", "\\\\$").replace("%", "%%")
    menu.write_text((ROOT / "assets" / "mark.desktop.in").read_text().replace("@CMAKE_INSTALL_FULL_DATADIR@", escaped))
    menu.chmod(0o755)
    icons = data / "icons" / "hicolor" / "scalable" / "emblems"
    icons.mkdir(parents=True, exist_ok=True)
    for icon in (build / "icons").glob("mark-color-*.svg"):
        (icons / icon.name).write_bytes(icon.read_bytes())
    for color in COLORS:
        path = preview / "files" / (color.title() + ".txt")
        if not path.exists():
            path.write_text("Sample file for Mark.\n")
            change_item(str(path), "add", color)
    config = preview / "config" / "dolphinrc"
    if not config.exists():
        config.write_text("[General]\nRememberOpenedTabs=false\n[IconsMode]\nIconSize=64\n")
    env = os.environ.copy()
    env.update(
        QT_PLUGIN_PATH=str(build / "plugins") + ":" + env.get("QT_PLUGIN_PATH", ""),
        XDG_DATA_HOME=str(data),
        XDG_CONFIG_HOME=str(preview / "config"),
        XDG_CACHE_HOME=str(preview / "cache"),
    )
    return subprocess.call(["dbus-run-session", "--", "dolphin", "--new-window", str(preview / "files")], env=env)


if __name__ == "__main__":
    sys.exit(main())

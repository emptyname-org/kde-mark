#!/usr/bin/env python3
"""Dependency-free syntax, whitespace and KDE service-menu checks."""

import ast
import configparser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
errors = []
for folder in ("src", "tests", "tools", "assets"):
    for path in (ROOT / folder).rglob("*"):
        if not path.is_file() or "__pycache__" in path.parts:
            continue
        text = path.read_text()
        if path.suffix == ".py":
            ast.parse(text, filename=str(path))
        if not text.endswith("\n") or any(line.rstrip() != line for line in text.splitlines()):
            errors.append(str(path.relative_to(ROOT)))
output = ROOT / "tmp" / "lint"
output.mkdir(parents=True, exist_ok=True)
desktop = output / "mark.desktop"
desktop.write_text((ROOT / "assets/mark.desktop.in").read_text().replace("@CMAKE_INSTALL_FULL_DATADIR@", "/usr/share"))
menu = configparser.ConfigParser(interpolation=None)
menu.read(desktop)
assert menu["Desktop Entry"]["Type"] == "Service"
assert menu["Desktop Entry"]["X-KDE-Protocols"] == "file"
for action in menu["Desktop Entry"]["Actions"].strip(";").split(";"):
    section = menu["Desktop Action " + action]
    assert section["Name"] and section["Icon"]
    assert section["Exec"].endswith(" -- %F")
if errors:
    raise SystemExit("Whitespace errors: " + ", ".join(errors))
print("Syntax, whitespace and service-menu checks passed.")

#!/usr/bin/env python3
"""Generate the seven menu icons and composite badges at build time."""

import math
from pathlib import Path
import sys
from xml.sax.saxutils import escape

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from mark import COLORS  # noqa: E402


def badge(colors):
    if len(colors) == 1:
        circles = [(16, 16, 12, colors[0])]
    else:
        radius = 6 if len(colors) <= 4 else 4.5
        circles = [
            (
                16 + 9 * math.cos(2 * math.pi * index / len(colors) - math.pi / 2),
                16 + 9 * math.sin(2 * math.pi * index / len(colors) - math.pi / 2),
                radius,
                color,
            )
            for index, color in enumerate(colors)
        ]
    shapes = "".join(
        f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{radius}" fill="{color}" stroke="white" stroke-width="1.5"/>'
        for x, y, radius, color in circles
    )
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 32 32">{shapes}</svg>\n'


def main():
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=True)
    resources = []
    for mask in range(1, 128):
        filename = f"mark-{mask}.svg"
        colors = [value for index, value in enumerate(COLORS.values()) if mask & (1 << index)]
        (output / filename).write_text(badge(colors))
        resources.append(f'<file alias="{filename}">{escape(str(output / filename))}</file>')
    for name, color in COLORS.items():
        (output / f"mark-color-{name}.svg").write_text(badge([color]))
    (output / "icons.qrc").write_text('<RCC><qresource prefix="/mark">' + "".join(resources) + "</qresource></RCC>\n")


if __name__ == "__main__":
    main()

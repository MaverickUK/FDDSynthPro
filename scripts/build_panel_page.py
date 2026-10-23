#!/usr/bin/env python3
"""Builds the interactive front-panel page from the firmware's own rendered
screens: every frame the UI tests draw, embedded in docs/oled/panel_template.html.

    scripts/build_panel_page.py out.html

It runs the host tests with FDDSYNTH_PREVIEW_DIR and FDDSYNTH_PREVIEW_ALL set
(so test/run_native_tests.sh must have built them), converts the frames to
PNG, and embeds them as data URIs. Standard library only.
"""
import base64
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, FDDSYNTH_PREVIEW_DIR=tmp, FDDSYNTH_PREVIEW_ALL="1")
        subprocess.run([str(REPO / ".pio/native-tests/run_tests")], cwd=REPO, env=env, check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run([sys.executable, str(REPO / "test/pbm_to_png.py"), tmp], check=True, stdout=subprocess.DEVNULL)
        frames = {p.stem: "data:image/png;base64," + base64.b64encode(p.read_bytes()).decode()
                  for p in sorted(Path(tmp).glob("*.png"))}
    template = (REPO / "docs/oled/panel_template.html").read_text()
    page = template.replace("/*FRAMES*/{}", json.dumps(frames, separators=(",", ":")))
    Path(sys.argv[1]).write_text(page)
    print(f"{sys.argv[1]}: {len(frames)} frames, {len(page) // 1024} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())

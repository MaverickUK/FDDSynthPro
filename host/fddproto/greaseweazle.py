"""The rig's host, driven through Greaseweazle's `gw` command-line tool:
a Feather RP2040 + Adafruit Floppy FeatherWing running Adafruit's
Greaseweazle-compatible firmware, or a real Greaseweazle.

The Feather has one drive select, which `gw` calls drive A and which asserts
pins 12/16, where our drive answers. A real Greaseweazle on a straight cable
needs drive B for the same pins. FDDPRO_GW_DRIVE sets it (host/rig.env).

Every call is logged (command, exit code, output), so a failing test can
save what the Greaseweazle said alongside the drive's trace.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .paths import HOST

# Actions that address a drive and so take --drive.
DRIVE_ACTIONS = {"read", "write", "erase", "clean", "seek", "rpm"}


def find_gw() -> str | None:
    """$GW, else the venv's gw, else gw on PATH."""
    if os.environ.get("GW"):
        return os.environ["GW"]
    candidate = HOST / ".venv/bin/gw"
    if candidate.exists():
        return str(candidate)
    return shutil.which("gw")


@dataclass
class GwResult:
    command: list[str]
    returncode: int
    output: str  # stdout and stderr together, in order

    @property
    def ok(self) -> bool:
        return self.returncode == 0


class Greaseweazle:
    def __init__(self, drive: str | None = None, device: str | None = None, gw: str | None = None):
        self.drive = drive or os.environ.get("FDDPRO_GW_DRIVE", "A")
        self.device = device or os.environ.get("FDDPRO_GW_DEVICE")
        self.gw = gw or find_gw()
        if not self.gw:
            raise FileNotFoundError("gw not found: pip install -r host/requirements.txt, or set GW")
        self.log: list[GwResult] = []

    def run(self, action: str, *args: str, drive: str | None = None, timeout: float = 120) -> GwResult:
        """Runs `gw <action> [--device D] [--drive X] <args>` and returns its result.
        Never raises on a non-zero exit: in slice 0 most actions are expected to fail."""
        command = [self.gw, action]
        if self.device:
            command += ["--device", self.device]
        if action in DRIVE_ACTIONS:
            command += ["--drive", drive or self.drive]
        command += list(args)
        p = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=timeout)
        result = GwResult(command, p.returncode, p.stdout)
        self.log.append(result)
        return result

    def info(self) -> GwResult:
        return self.run("info")

    def reset(self) -> GwResult:
        """Back to power-on state: every drive deselected, motors off."""
        return self.run("reset")

    def seek(self, cylinder: int, motor_on: bool = False, drive: str | None = None) -> GwResult:
        args = [str(cylinder)] + (["--motor-on"] if motor_on else [])
        return self.run("seek", *args, drive=drive)

    def rpm(self, drive: str | None = None) -> GwResult:
        return self.run("rpm", drive=drive)

    def read(self, image: Path, fmt: str = "ibm.1440", tracks: str | None = None) -> GwResult:
        args = ["--format", fmt] + (["--tracks", tracks] if tracks else []) + [str(image)]
        return self.run("read", *args, timeout=300)

    def write(self, image: Path, fmt: str = "ibm.1440", tracks: str | None = None) -> GwResult:
        """Writes an image (or some of its tracks); gw reads each track back to verify."""
        args = ["--format", fmt] + (["--tracks", tracks] if tracks else []) + [str(image)]
        return self.run("write", *args, timeout=600)

    def transcript(self) -> str:
        """Every call so far, for a run folder."""
        return "\n".join(f"$ {' '.join(r.command)}\n(exit {r.returncode})\n{r.output}" for r in self.log)

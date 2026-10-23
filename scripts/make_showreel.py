#!/usr/bin/env python3
"""A demo video of the drive: a virtual PC uses a real FAT12 disk while the
front panel is worked, shown and heard through the firmware's own code
(test/tools/showreel.cpp). This lays it out with the OLED scaled up, the
controls, a caption and the PC's log, and encodes it with ffmpeg.

    scripts/make_showreel.py [out.mp4] [--pack samples/Recorded]
                                            (default media/showreel.mp4, the Placeholder pack)

It also writes a poster beside it (showreel.png): a frame from the head
trace with a play button, which the README links to the video.

Needs: test/run_native_tests.sh run once (it builds the tool), mtools,
ffmpeg, and Pillow (python3 -m pip install pillow).
"""
from __future__ import annotations

import argparse
import array
import math
import random
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parent.parent
TOOL = REPO / ".pio/native-tests/showreel"
PACK = REPO / "samples/Placeholder"

W, H = 1280, 720
FPS = 30
PER_FRAME = FPS // 10  # the panel draws 10 frames a second
OLED_W, OLED_H, SCALE = 128, 64, 6

BG = (14, 16, 20)
BEZEL = (32, 36, 44)
OLED_OFF = (6, 10, 14)
OLED_ON = (150, 225, 255)
TEXT = (230, 232, 236)
DIM = (130, 136, 148)
ACCENT = (110, 200, 255)


def font(names: list[str], size: int):
    for name in names:
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


TITLE_FONT = font(["/System/Library/Fonts/HelveticaNeue.ttc", "DejaVuSans.ttf"], 22)
CAPTION_FONT = font(["/System/Library/Fonts/HelveticaNeue.ttc", "DejaVuSans.ttf"], 34)
LABEL_FONT = font(["/System/Library/Fonts/HelveticaNeue.ttc", "DejaVuSans.ttf"], 18)
LOG_FONT = font(["/System/Library/Fonts/Menlo.ttc", "DejaVuSansMono.ttf"], 17)


def make_disk(tmp: Path) -> Path:
    """GAMES: a few files, with LEVEL9.DAT in the middle of the disk (a big
    file before it is deleted, and GAME.EXE fills the gap), so reading it
    means a long seek."""
    rng = random.Random(9)
    files = {
        "AUTOEXEC.BAT": b"@ECHO OFF\r\nPROMPT $P$G\r\n",
        "README.TXT": b"FDD Synth Pro showreel disk.\r\n" * 60,
        "FILLER": rng.randbytes(700 * 1024),
        "LEVEL9.DAT": rng.randbytes(40 * 1024),
        "GAME.EXE": rng.randbytes(180 * 1024),
    }
    img = tmp / "games.img"

    def mtools(*args):
        subprocess.run(args, check=True, capture_output=True)

    mtools("mformat", "-i", str(img), "-C", "-f", "1440", "-v", "GAMES", "::")
    for name, data in files.items():
        (tmp / name).write_bytes(data)
        if name == "GAME.EXE":
            mtools("mdel", "-i", str(img), "::FILLER")
        mtools("mcopy", "-i", str(img), str(tmp / name), "::")
    return img


def read_script(path: Path) -> list[tuple[int, str, str]]:
    out = []
    for line in path.read_text().splitlines():
        frame, kind, text = (line.split(" ", 2) + [""])[:3]
        out.append((int(frame), kind, text))
    return out


def oled_image(pixels: bytes) -> Image.Image:
    """The panel's frame as the module shows it: each pixel a lit square
    with a hairline gap."""
    img = Image.new("RGB", (OLED_W * SCALE, OLED_H * SCALE), OLED_OFF)
    d = ImageDraw.Draw(img)
    for y in range(OLED_H):
        row = pixels[y * OLED_W:(y + 1) * OLED_W]
        for x, on in enumerate(row):
            if on:
                d.rectangle([x * SCALE, y * SCALE, x * SCALE + SCALE - 2, y * SCALE + SCALE - 2], fill=OLED_ON)
    return img


def draw_controls(d: ImageDraw.ImageDraw, knob_angle: float, knob_lit: float, button_lit: float, held: bool):
    # The encoder: a knob with a pointer, lit briefly when turned or pressed.
    cx, cy, r = 1075, 210, 62
    glow = tuple(int(BEZEL[i] + (ACCENT[i] - BEZEL[i]) * knob_lit * 0.6) for i in range(3))
    d.ellipse([cx - r - 6, cy - r - 6, cx + r + 6, cy + r + 6], fill=glow)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(48, 52, 62), outline=(80, 86, 98), width=2)
    for k in range(20):  # knurling
        a = k * math.tau / 20
        d.line([cx + math.sin(a) * (r - 8), cy - math.cos(a) * (r - 8), cx + math.sin(a) * (r - 2),
                cy - math.cos(a) * (r - 2)], fill=(70, 75, 88), width=2)
    d.line([cx, cy, cx + math.sin(knob_angle) * (r - 14), cy - math.cos(knob_angle) * (r - 14)], fill=TEXT, width=5)
    d.text((cx, cy + r + 22), "ENCODER", font=LABEL_FONT, fill=DIM, anchor="mm")

    # The action button.
    bx, by, br = 1075, 400, 34
    fill = tuple(int(48 + (ACCENT[i] - 48) * button_lit) for i in range(3))
    d.ellipse([bx - br - 6, by - br - 6, bx + br + 6, by + br + 6], fill=BEZEL)
    d.ellipse([bx - br, by - br, bx + br, by + br], fill=fill, outline=(80, 86, 98), width=2)
    d.text((bx, by + br + 22), "ACTION" + (" (held)" if held else ""), font=LABEL_FONT, fill=DIM, anchor="mm")


# What the mixer plays for each sound event (src/synth.h).
SAMPLES = {
    "MOTOR_ON": "spinup, then the spin loop",
    "MOTOR_OFF": "spindown",
    "STEP": "step",
    "SEEK_START": "seek loop",
    "SEEK_END": "seek loop ends",
    "BUMP": "bump",
    "INSERT": "insert",
    "EJECT": "eject",
}


def read_sounds(path: Path) -> list[tuple[float, str]]:
    out = []
    for line in path.read_text().splitlines():
        ms, name = line.split()
        out.append((int(ms) / 1000, name))
    return out


def levels(path: Path, n: int) -> list[float]:
    """The audio's level in each video frame, in dBFS (RMS)."""
    with wave.open(str(path)) as w:
        rate = w.getframerate()
        a = array.array("h", w.readframes(w.getnframes()))
    per = rate / FPS
    out = []
    for i in range(n):
        seg = a[int(i * per):int((i + 1) * per)]
        rms = math.sqrt(sum(x * x for x in seg) / len(seg)) if seg else 0
        out.append(20 * math.log10(rms / 32768) if rms > 0 else -90.0)
    return out


def draw_sound(d: ImageDraw.ImageDraw, now: float, level_db: float, peak_db: float, sounds: list[tuple[float, str]]):
    """The level meter, and the sound events as the mixer gets them, newest first."""
    x0, x1, y = 900, 1240, 500
    d.text((x0, y), "OUTPUT", font=LABEL_FONT, fill=DIM)
    bar = lambda db: x0 + (x1 - x0) * max(0.0, min(1.0, (db + 60) / 60))
    d.rounded_rectangle([x0, y + 28, x1, y + 44], 4, fill=BEZEL)
    if level_db > -60:
        d.rounded_rectangle([x0, y + 28, bar(level_db), y + 44], 4, fill=ACCENT)
    d.line([bar(peak_db), y + 26, bar(peak_db), y + 46], fill=TEXT, width=2)

    d.text((x0, y + 62), "SOUND EVENTS  ->  SAMPLES PLAYED", font=LABEL_FONT, fill=DIM)
    shown = [e for e in sounds if e[0] <= now][-5:][::-1]
    for i, (t, name) in enumerate(shown):
        age = now - t
        fresh = max(0.0, 1 - age / 0.6) if i == 0 else 0.0
        colour = tuple(int(DIM[k] + (ACCENT[k] - DIM[k]) * fresh) for k in range(3)) if i else \
            (ACCENT if fresh > 0 else TEXT)
        row = y + 92 + i * 24
        d.text((x0, row), f"{t:5.1f}s {name:<10} {SAMPLES.get(name, '')}", font=LOG_FONT, fill=colour)


POSTER_AT = 20.0  # seconds: the head trace, with the long seek on it


def make_poster(video: Path, poster: Path):
    with tempfile.TemporaryDirectory() as t:
        frame = Path(t) / "frame.png"
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", str(POSTER_AT), "-i", str(video), "-frames:v", "1",
                        str(frame)], check=True)
        img = Image.open(frame).convert("RGB")
    d = ImageDraw.Draw(img, "RGBA")
    cx, cy, r = 64 + OLED_W * SCALE // 2, 110 + OLED_H * SCALE // 2, 70
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(0, 0, 0, 170), outline=(255, 255, 255, 230), width=4)
    d.polygon([(cx - 22, cy - 36), (cx - 22, cy + 36), (cx + 40, cy)], fill=(255, 255, 255, 240))
    img.save(poster, optimize=True)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("out", nargs="?", type=Path, default=REPO / "media/showreel.mp4")
    p.add_argument("--pack", type=Path, default=PACK, help="the sample pack's folder")
    args = p.parse_args()
    out, pack = args.out, args.pack.resolve()
    if not TOOL.exists():
        print("showreel not built: run test/run_native_tests.sh", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        disk = make_disk(tmp)
        run = subprocess.run([str(TOOL), str(disk), str(pack), str(tmp)], capture_output=True, text=True)
        print(run.stdout.strip(), run.stderr.strip())
        if run.returncode:
            return run.returncode
        frames = (tmp / "frames.raw").read_bytes()
        n = len(frames) // (OLED_W * OLED_H)
        script = read_script(tmp / "script.txt")
        sounds = read_sounds(tmp / "sounds.txt")
        level = levels(tmp / "audio.wav", n * PER_FRAME)
        peak = -90.0

        encoder = subprocess.Popen(
            ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
             "-r", str(FPS), "-i", "-", "-i", str(tmp / "audio.wav"), "-c:v", "libx264", "-pix_fmt", "yuv420p",
             "-crf", "18", "-c:a", "aac", "-ar", "44100", "-ac", "2", "-b:a", "192k", "-shortest", "-movflags", "+faststart", str(out)],
            stdin=subprocess.PIPE)

        caption, log = "", []
        knob_angle, knob_until, button_until = 0.0, -1.0, -1.0
        # A long press fires once the button has been held a second
        # (cfg::BUTTON_LONG_MS), so it shows held for the second before.
        holds = [(f - 10, f + 1) for f, kind, text in script if kind == "INPUT" and text == "LongAction"]
        events = iter(script)
        pending = next(events, None)
        oled_box = (64, 110)
        for f in range(n):
            while pending and pending[0] <= f:
                _, kind, text = pending
                if kind == "CAPTION":
                    caption = text
                elif kind == "LOG":
                    log = (log + [text])[-7:]
                elif kind == "INPUT":
                    if text in ("TurnUp", "TurnDown"):
                        knob_angle += (1 if text == "TurnUp" else -1) * math.tau / 20
                        knob_until = f + 3
                    elif text == "Press":
                        knob_until = f + 3
                    elif text == "Action":
                        button_until = f + 3
                pending = next(events, None)
            oled = oled_image(frames[f * OLED_W * OLED_H:(f + 1) * OLED_W * OLED_H])
            for sub in range(PER_FRAME):
                t = f + sub / PER_FRAME
                img = Image.new("RGB", (W, H), BG)
                d = ImageDraw.Draw(img)
                d.text((64, 40), "FDDSynthPro", font=TITLE_FONT, fill=TEXT)
                d.text((210, 40), "a virtual PC, the front panel and the FDD Synth, all running the firmware's own code",
                       font=TITLE_FONT, fill=DIM)
                ox, oy = oled_box
                d.rounded_rectangle([ox - 18, oy - 18, ox + OLED_W * SCALE + 18, oy + OLED_H * SCALE + 18], 14,
                                    fill=BEZEL)
                img.paste(oled, (ox, oy))
                lit = lambda until: max(0.0, min(1.0, (until - t) / 3))
                held = any(a <= t < b for a, b in holds)
                draw_controls(d, knob_angle, lit(knob_until), 1.0 if held else lit(button_until), held)
                v = f * PER_FRAME + sub
                peak = max(level[v], peak - 1.0)  # falls 30 dB a second
                draw_sound(d, t / 10, level[v], peak, sounds)
                d.text((64, 540), caption, font=CAPTION_FONT, fill=TEXT)
                for i, line in enumerate(log[-4:]):
                    d.text((64, 596 + i * 26), line, font=LOG_FONT, fill=DIM if i < len(log[-4:]) - 1 else ACCENT)
                encoder.stdin.write(img.tobytes())
        encoder.stdin.close()
        if encoder.wait():
            return 1
    make_poster(out, out.with_suffix(".png"))
    print(f"wrote {out} ({n / 10:.1f} s) and {out.with_suffix('.png').name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

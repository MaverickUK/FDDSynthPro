"""The host back to its power-on state (drives deselected, motors off), as
`gw reset` does, but without reading and rewriting its delay settings.

`gw reset` (host tools 1.23) asks for 16 bytes of delays and waits for all
of them with no timeout. Adafruit's Greaseweazle-compatible firmware always
answers with 12, so `gw reset` hangs on the Feather. The reset command on
its own works on both it and a real Greaseweazle; the delays go back to the
firmware's defaults, which is what the rig wants anyway.

Run: python -m fddproto.gw_reset [--device PORT]
"""
from __future__ import annotations

import argparse

from greaseweazle.tools import util


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--device", help="the host's serial port (default: find a Greaseweazle)")
    args = parser.parse_args(argv)
    usb = util.usb_open(args.device)
    usb.power_on_reset()
    print("reset")


if __name__ == "__main__":
    main()

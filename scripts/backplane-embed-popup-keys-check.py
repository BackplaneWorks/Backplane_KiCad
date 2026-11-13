#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13 check for `kicad --embedded` on X11: reserved chords get through KiCad's popups.

While a KiCad popup menu is up, GTK holds the keyboard, so a host-reserved chord (Ctrl+K and the
like) used to go to the menu and was lost to the host.  Now:

- GetCapabilities lists `embed-popup-keys`.
- A right-click menu on the canvas holds the keyboard (another client's XGrabKeyboard answers
  AlreadyGrabbed), and a key that is not reserved (Down) stays with the menu.
- A reserved chord pressed in the menu (Ctrl+K, Ctrl+Shift+Tab, and Ctrl+; once the host
  reserves it) closes the menu, frees the keyboard, and reaches the host as _BACKPLANE_KEY with
  its keysym and modifiers, the same as from the frame.

Runs on a private Xvfb + i3 with a python-xlib stand-in host; screenshots to --out.
"""

import argparse
from pathlib import Path
import sys
import time

from Xlib import X

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_embed_test_support import (FAILURES, IpcSession, Session, check,  # noqa: E402
                                          viewable, wait_for)


def keyboard_grabbed(host):
    """True if some other client holds the keyboard (our own grab answers AlreadyGrabbed then)."""
    status = host.root.grab_keyboard(False, X.GrabModeAsync, X.GrabModeAsync, X.CurrentTime)

    if status == X.GrabSuccess:
        host.display.ungrab_keyboard(X.CurrentTime)
        host.display.sync()
        return False

    return status == X.AlreadyGrabbed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad", type=Path)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--out", type=Path, default=Path("embed-popup-keys-out"))
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "demos/pic_programmer"

    with Session(args.kicad, args.out, source) as s, \
            IpcSession(args.kicad_cli, api_socket=s.socket, timeout=300000) as api:
        embed = api.module("backplane.embed")
        capabilities = api.module("backplane.capabilities")
        host = s.host
        disp = host.display
        key_atom = disp.intern_atom("_BACKPLANE_KEY")

        def wait_key(seconds=5.0):
            """The first _BACKPLANE_KEY since the messages were cleared: (window, type, data)."""
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                host.pump(0.1)
                for m in host.messages:
                    if m[1] == key_atom:
                        return m
            return None

        caps = api.request(capabilities.GetCapabilities(), capabilities.GetCapabilitiesResponse)
        check("embed-popup-keys" in caps.capabilities,
              f"GetCapabilities lists embed-popup-keys ({sorted(c for c in caps.capabilities if c.startswith('embed'))})")

        w, h = host.size
        r = api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=w, height=h),
                        embed.ShowFrameResponse)
        pcb = r.frame.window
        wait_for(lambda: viewable(disp, pcb), 60)
        time.sleep(2.0)
        host.pump(0.2)

        # The canvas has the keyboard after a click, as a person would start
        cx, cy = w // 2, h // 2
        s.xdotool("mousemove", str(cx), str(cy), "click", "1")
        time.sleep(0.8)
        s.xdotool("key", "Escape")
        time.sleep(0.3)

        def open_menu(label):
            s.xdotool("mousemove", str(cx), str(cy), "click", "3")
            ok = wait_for(lambda: keyboard_grabbed(host), 5)
            s.shot(label)
            return ok

        def chord_in_menu(chord, xdotool_keys, keysym, x_modifiers, label):
            if not open_menu(f"{label}-menu"):
                check(False, f"{chord}: a right-click menu opened and holds the keyboard")
                return

            host.pump(0.1)
            host.messages.clear()
            s.xdotool("key", "--clearmodifiers", xdotool_keys)
            message = wait_key()
            freed = wait_for(lambda: not keyboard_grabbed(host), 3)
            s.shot(f"{label}-after")
            check(message is not None and message[2][0] == pcb and message[2][1] == keysym
                  and message[2][2] & x_modifiers == x_modifiers,
                  f"{chord} in the menu reaches the host as _BACKPLANE_KEY ({message})")
            check(freed, f"{chord} closes the menu and frees the keyboard")

        # A key that is not reserved stays with the menu
        if open_menu("01-menu"):
            check(True, "a right-click menu opens and holds the keyboard")
            host.messages.clear()
            s.xdotool("key", "Down")
            host.pump(0.6)
            check(keyboard_grabbed(host), "Down stays with the menu (still open)")
            check(not any(m[1] == key_atom for m in host.messages),
                  "Down is not sent to the host")
            s.xdotool("key", "Escape")
            check(wait_for(lambda: not keyboard_grabbed(host), 3), "Escape closes the menu as before")
        else:
            check(False, "a right-click menu opens and holds the keyboard")

        chord_in_menu("Ctrl+K", "ctrl+k", 0x6b, 4, "02-ctrl-k")
        chord_in_menu("Ctrl+Shift+Tab", "ctrl+shift+Tab", 0xff09, 5, "03-ctrl-shift-tab")

        keys = api.request(embed.SetReservedKeys(chords=["Ctrl+K", "Ctrl+J", "Ctrl+Tab", "Ctrl+Shift+Tab",
                                                         "Ctrl+`", "Ctrl+;"]),
                           embed.SetReservedKeysResponse)
        check("Ctrl+;" in list(keys.chords), f"the host reserves Ctrl+; ({list(keys.chords)})")
        chord_in_menu("Ctrl+;", "ctrl+semicolon", 0x3b, 4, "04-ctrl-semicolon")

        # The frame still takes its own keys afterwards: a reserved chord from the canvas
        host.messages.clear()
        s.xdotool("mousemove", str(cx), str(cy), "click", "1")
        time.sleep(0.5)
        s.xdotool("key", "Escape")
        s.xdotool("key", "--clearmodifiers", "ctrl+k")
        message = wait_key()
        check(message is not None and message[2][1] == 0x6b,
              f"after the menus, Ctrl+K from the canvas still reaches the host ({message})")

    print("ALL PASSED" if not FAILURES else f"{len(FAILURES)} failed")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-or-later
"""A private X server, window manager and stand-in host for `kicad --embedded` checks.

Everything lives in a temporary tree; the user's configuration and projects are never touched.
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from Xlib import X, Xatom, display as xdisplay

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_ipc_test_support import IpcSession  # noqa: E402,F401

FAILURES = []


def check(condition, message):
    print(("PASS " if condition else "FAIL ") + message, flush=True)
    if not condition:
        FAILURES.append(message)
    return condition


def wait_for(predicate, seconds, step=0.1):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(step)
    return predicate()


class WmWatcher(threading.Thread):
    """Samples the window manager's client list; remembers every window it ever managed."""

    def __init__(self, name):
        super().__init__(daemon=True)
        self.display = xdisplay.Display(name)
        self.root = self.display.screen().root
        self.atom = self.display.intern_atom("_NET_CLIENT_LIST")
        self.seen = {}
        self.stop = False

    def current(self):
        prop = self.root.get_full_property(self.atom, Xatom.WINDOW)
        return list(prop.value) if prop else []

    def run(self):
        while not self.stop:
            try:
                for wid in self.current():
                    if wid not in self.seen:
                        window = self.display.create_resource_object("window", wid)
                        try:
                            transient = window.get_wm_transient_for()
                            transient = transient.id if transient else 0
                            name = window.get_wm_name() or ""
                        except Exception:
                            transient, name = 0, ""
                        self.seen[wid] = (transient, name)
            except Exception:
                pass
            time.sleep(0.02)


class Host:
    """The stand-in embedding application: a WM-managed top-level and a root-visual container."""

    def __init__(self, name, width=1400, height=900):
        self.display = xdisplay.Display(name)
        screen = self.display.screen()
        self.root = screen.root
        self.top = self.root.create_window(0, 0, width, height, 0, screen.root_depth,
                                           X.InputOutput, X.CopyFromParent,
                                           background_pixel=screen.white_pixel,
                                           event_mask=X.StructureNotifyMask)
        self.top.set_wm_name("EmbedHost")
        self.container = self.top.create_window(0, 0, width, height, 0, screen.root_depth,
                                                X.InputOutput, X.CopyFromParent,
                                                background_pixel=0x303030,
                                                event_mask=X.StructureNotifyMask)
        self.container.map()
        self.top.map()
        self.display.sync()
        self.messages = []
        self.size = (width, height)
        self.pump(1.0)

    def pump(self, seconds=0.0):
        deadline = time.monotonic() + seconds
        while True:
            while self.display.pending_events():
                event = self.display.next_event()
                if event.type == X.ClientMessage:
                    self.messages.append((event.window.id, event.client_type, list(event.data[1])))
                elif event.type == X.ConfigureNotify and event.window.id == self.top.id:
                    self.size = (event.width, event.height)
                    self.container.configure(width=event.width, height=event.height)
                    self.display.sync()
            if time.monotonic() >= deadline:
                return
            time.sleep(0.02)


def parent_of(disp, wid):
    return disp.create_resource_object("window", wid).query_tree().parent.id


def viewable(disp, wid):
    try:
        return disp.create_resource_object("window", wid).get_attributes().map_state == X.IsViewable
    except Exception:
        return False


class Session:
    """Xvfb + i3 + host + `kicad --embedded` on a copied project."""

    def __init__(self, kicad, out, project_source, extra_args=(), xvfb=None, i3=None):
        self.kicad = Path(kicad)
        self.out = Path(out)
        self.out.mkdir(parents=True, exist_ok=True)
        self.project_source = Path(project_source)
        self.extra_args = list(extra_args)
        self.xvfb = xvfb or shutil.which("Xvfb") or "Xvfb"
        self.i3 = i3 or shutil.which("i3") or "i3"
        self.processes = []
        self.kicad_process = None

    def __enter__(self):
        self.work = Path(tempfile.mkdtemp(prefix="backplane-embed-"))
        for number in range(140, 400):
            if not Path(f"/tmp/.X11-unix/X{number}").exists() and not Path(f"/tmp/.X{number}-lock").exists():
                break
        self.number = number
        self.name = f":{number}"
        env = dict(os.environ)
        env.update({"DISPLAY": self.name, "GDK_BACKEND": "x11", "GDK_SCALE": "1", "NO_AT_BRIDGE": "1",
                    "LIBGL_ALWAYS_SOFTWARE": "1", "GALLIUM_DRIVER": "llvmpipe", "LP_NUM_THREADS": "4"})
        env.pop("WAYLAND_DISPLAY", None)
        for variable, directory in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"),
                                    ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache"),
                                    ("XDG_RUNTIME_DIR", "run"), ("TMPDIR", "tmp")):
            path = self.work / directory
            path.mkdir(parents=True, exist_ok=True, mode=0o700)
            env[variable] = str(path)
        for version in ("10.0", "10.99"):
            config = self.work / "config/kicad" / version
            config.mkdir(parents=True, exist_ok=True)
            (config / "sym-lib-table").write_text("(sym_lib_table\n  (version 7)\n)\n")
            (config / "fp-lib-table").write_text("(fp_lib_table\n  (version 7)\n)\n")
            (config / "design-block-lib-table").write_text("(design_block_lib_table\n  (version 7)\n)\n")
        self.env = env
        self.project_dir = self.work / self.project_source.name
        shutil.copytree(self.project_source, self.project_dir)
        self.project = next(self.project_dir.glob("*.kicad_pro"))

        self.processes.append(subprocess.Popen([self.xvfb, self.name, "-screen", "0", "1600x1000x24",
                                                "-nolisten", "tcp", "+extension", "GLX"],
                                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        assert wait_for(lambda: Path(f"/tmp/.X11-unix/X{self.number}").exists(), 10), "Xvfb did not start"
        i3conf = self.work / "i3.conf"
        i3conf.write_text(f"ipc-socket {self.work}/i3.sock\nfont pango:monospace 8\n"
                          "default_border none\ndefault_floating_border normal\nfocus_follows_mouse no\n")
        self.processes.append(subprocess.Popen([self.i3, "-c", str(i3conf)], env=env,
                                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1.5)
        self.watcher = WmWatcher(self.name)
        self.watcher.start()
        self.host = Host(self.name)
        self.socket = self.work / "run/kicad/api.sock"
        self.socket.parent.mkdir(parents=True, exist_ok=True)
        self.log = (self.out / "kicad.log").open("w")
        self.kicad_process = subprocess.Popen(
            [str(self.kicad), "--embedded", "--parent-window", hex(self.host.container.id),
             "--toplevel-window", str(self.host.top.id), "--api-socket", str(self.socket),
             "--parent-pid", str(os.getpid()), *self.extra_args, str(self.project)],
            env=env, stdout=subprocess.PIPE, stderr=self.log, text=True)
        deadline = time.monotonic() + 120
        self.ready = False
        while time.monotonic() < deadline:
            line = self.kicad_process.stdout.readline()
            if not line:
                break
            if line.startswith("KiCad API server listening at "):
                self.ready = True
                break
        threading.Thread(target=lambda: [None for _ in self.kicad_process.stdout], daemon=True).start()
        if not self.ready:
            raise RuntimeError("KiCad did not become ready")
        return self

    def shot(self, label):
        path = self.out / f"{label}.png"
        subprocess.run(["import", "-display", self.name, "-window", "root", str(path)], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return path

    def xdotool(self, *words):
        subprocess.run(["xdotool", *words], env=self.env, check=False)

    def __exit__(self, *exc):
        if exc[0] is not None and self.kicad_process is not None and self.kicad_process.poll() is None \
                and shutil.which("gdb"):
            stacks = subprocess.run(["gdb", "-p", str(self.kicad_process.pid), "-batch", "-ex",
                                     "thread apply all bt 25"], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
            (self.out / "kicad-stacks.txt").write_text(stacks)
        self.watcher.stop = True
        if self.kicad_process is not None and self.kicad_process.poll() is None:
            self.kicad_process.terminate()
            try:
                self.kicad_process.wait(10)
            except subprocess.TimeoutExpired:
                self.kicad_process.kill()
        for process in reversed(self.processes):
            process.terminate()
            try:
                process.wait(5)
            except subprocess.TimeoutExpired:
                process.kill()
        shutil.rmtree(self.work, ignore_errors=True)
        return False


def region_colour(path, crop):
    """The most common colour in a crop of a screenshot, as (r, g, b)."""
    result = subprocess.run(["convert", str(path), "-crop", crop, "+repage", "-format", "%c",
                             "histogram:info:-"], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL).stdout
    best, colour = -1, (0, 0, 0)
    for line in result.splitlines():
        line = line.strip()
        if ":" not in line or "(" not in line:
            continue
        count = int(line.split(":", 1)[0])
        values = line.split("(", 1)[1].split(")", 1)[0].split(",")
        try:
            rgb = tuple(int(float(v)) for v in values[:3])
        except ValueError:
            continue
        if count > best:
            best, colour = count, rgb
    return colour


def luminance(rgb):
    r, g, b = rgb
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def mean_difference(a, b, crop):
    """Mean absolute pixel difference (0..1) between two screenshots in a crop."""
    result = subprocess.run(["compare", "-metric", "MAE", "(", str(a), "-crop", crop, "+repage", ")",
                             "(", str(b), "-crop", crop, "+repage", ")", "null:"],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stderr
    try:
        return float(result.split("(", 1)[1].split(")", 1)[0])
    except (IndexError, ValueError):
        return 1.0

# SPDX-License-Identifier: GPL-3.0-or-later
"""Run an embedded KiCad on a private X server for co-editing tests (docs/backplane/coedit.md).

    with EmbeddedKiCad(kicad, kicad_cli, "demos/pic_programmer", out) as k:
        k.host_api   # IpcSession as the host ("host": the person's own actions from Backplane)
        k.agent("agent:t1")  # a new IpcSession as an agent actor
        k.xdotool("key", "m"); k.shot("label")
"""

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time

from backplane_ipc_test_support import IpcSession

_spec = importlib.util.spec_from_file_location(
    "embed_check", Path(__file__).resolve().parent / "backplane-embed-x11-check.py")
embed_check = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(embed_check)


class EmbeddedKiCad:
    def __init__(self, kicad, kicad_cli, demo, out, size=(1500, 950)):
        self.kicad_bin, self.kicad_cli, self.demo, self.out = Path(kicad), Path(kicad_cli), demo, Path(out)
        self.size = size
        self.processes, self.sessions = [], []

    def __enter__(self):
        self.out.mkdir(parents=True, exist_ok=True)
        self.work = Path(tempfile.mkdtemp(prefix="backplane-coedit-"))
        number = embed_check.free_display()
        self.display = f":{number}"
        env = dict(os.environ)
        env.update({"DISPLAY": self.display, "GDK_BACKEND": "x11", "GDK_SCALE": "1",
                    "NO_AT_BRIDGE": "1", "LIBGL_ALWAYS_SOFTWARE": "1", "GALLIUM_DRIVER": "llvmpipe",
                    "LP_NUM_THREADS": "4"})
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

        source = Path(__file__).resolve().parents[1]
        self.project_dir = self.work / Path(self.demo).name
        shutil.copytree(source / self.demo, self.project_dir)
        self.project = next(self.project_dir.glob("*.kicad_pro"))
        self.board_file = self.project.with_suffix(".kicad_pcb")

        self.processes.append(subprocess.Popen(
            ["Xvfb", self.display, "-screen", "0", "1600x1000x24", "-nolisten", "tcp", "+extension", "GLX"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        assert embed_check.wait_for(lambda: Path(f"/tmp/.X11-unix/X{number}").exists(), 10)
        i3conf = self.work / "i3.conf"
        i3conf.write_text(f"ipc-socket {self.work}/i3.sock\nfont pango:monospace 8\ndefault_border none\n"
                          "default_floating_border normal\nfocus_follows_mouse no\n")
        self.processes.append(subprocess.Popen(["i3", "-c", str(i3conf)], env=env,
                                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1.5)
        self.host = embed_check.Host(self.display, *self.size)

        self.socket = self.work / "run/kicad/api.sock"
        self.socket.parent.mkdir(parents=True, exist_ok=True)
        log = (self.out / "kicad.log").open("w")
        self.kicad = subprocess.Popen(
            [str(self.kicad_bin), "--embedded", "--parent-window", hex(self.host.container.id),
             "--toplevel-window", str(self.host.top.id), "--api-socket", str(self.socket),
             "--parent-pid", str(os.getpid()), str(self.project)],
            env=env, stdout=subprocess.PIPE, stderr=log, text=True)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            line = self.kicad.stdout.readline()
            if not line or line.startswith("KiCad API server listening at "):
                break
        threading.Thread(target=lambda: [None for _ in self.kicad.stdout], daemon=True).start()

        self.host_api = self.session("host")
        api = self.host_api
        self.m = {name: api.module(mod) for name, mod in (
            ("embed", "backplane.embed"), ("coedit", "backplane.coedit"),
            ("editor", "common.commands.editor_commands"), ("types", "common.types.base_types"),
            ("enums", "common.types.enums"), ("board", "board.board_types"),
            ("project", "common.commands.project_commands"), ("bcmd", "board.board_commands"),
            ("rules", "board.board_rules"))}
        embed = self.m["embed"]
        frames = api.request(embed.GetFrames(), embed.GetFramesResponse)
        self.host.display.create_resource_object("window", frames.holder).change_save_set(1)
        w, h = self.host.size
        api.request(embed.ShowFrame(kind=embed.FK_PCB, x=0, y=0, width=w, height=h, focus=True),
                    embed.ShowFrameResponse)
        self.host.pump(3.0)
        docs = api.request(self.m["editor"].GetOpenDocuments(type=self.m["types"].DOCTYPE_PCB),
                           self.m["editor"].GetOpenDocumentsResponse)
        self.doc = docs.documents[0]
        return self

    def session(self, client):
        s = IpcSession(self.kicad_cli, api_socket=self.socket, timeout=120000, client_name=client)
        s.__enter__()
        self.sessions.append(s)
        return s

    def xdotool(self, *words):
        subprocess.run(["xdotool", *map(str, words)], env=self.env, check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def shot(self, label):
        path = self.out / f"{label}.png"
        subprocess.run(["import", "-display", self.display, "-window", "root", str(path)], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return path

    def alive(self):
        return self.kicad.poll() is None

    def __exit__(self, *exc):
        for s in self.sessions:
            try:
                s.__exit__(None, None, None)
            except Exception:  # noqa: BLE001
                pass
        if self.kicad and self.kicad.poll() is None:
            self.kicad.terminate()
            try:
                self.kicad.wait(10)
            except subprocess.TimeoutExpired:
                self.kicad.kill()
        for p in reversed(self.processes):
            p.terminate()
        shutil.rmtree(self.work, ignore_errors=True)

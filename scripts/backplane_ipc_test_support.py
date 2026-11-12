# SPDX-License-Identifier: GPL-3.0-or-later
"""Isolated IPC test session; never opens or edits the user's projects."""

import importlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

import pynng


def resolve_stock_cli(path):
    """Return a stock KiCad 10.0.x kicad-cli used to open exported KiCad 10 copies."""
    path = Path(path).resolve(strict=True)
    version = subprocess.check_output([str(path), "--version"], text=True).strip()
    assert version.startswith("10.0."), f"stock check needs KiCad 10.0.x, got {version}"
    return path


class IpcSession:
    def __init__(self, cli, timeout=180000, api_socket=None, server_args=None,
                 client_name="works.backplane.ipc-regression"):
        """Create an isolated session, or attach to an existing API socket.

        ``api_socket`` is useful for GUI regressions: the caller owns the GUI
        process and its lifetime, while this client still gets generated
        bindings and isolated configuration directories.
        """
        self.cli = Path(cli).resolve(strict=True)
        self.api_socket = Path(api_socket).resolve() if api_socket else None
        self.source = Path(__file__).resolve().parents[1]
        self.timeout = timeout
        self.server_args = list(server_args or [])
        self.token = ""
        self.client_name = client_name
        self.server = None
        self.connection = None
        self.log = None

    def __enter__(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="backplane-ipc-regression-")
        self.root = Path(self.temporary.name)
        self.bindings = self.root / "bindings"
        self.bindings.mkdir()
        proto = self.source / "api/proto"
        try:
            subprocess.run(["protoc", f"-I{proto}", f"--python_out={self.bindings}",
                            *map(str, sorted(proto.rglob("*.proto")))], check=True)
            sys.path.insert(0, str(self.bindings))
            self.envelope = self.module("common.envelope")
            self.env = dict(os.environ)
            for variable, directory in (
                ("HOME", "home"), ("XDG_DATA_HOME", "data"),
                ("XDG_CACHE_HOME", "cache"), ("XDG_CONFIG_HOME", "config"),
                ("KICAD_CONFIG_HOME", "config/kicad"),
            ):
                path = self.root / directory
                path.mkdir(parents=True, exist_ok=True)
                self.env[variable] = str(path)
            self.env.pop("DISPLAY", None)
            self.env.pop("WAYLAND_DISPLAY", None)
            self.socket = self.api_socket or self.root / "api.sock"
            if self.api_socket is None:
                self.log = (self.root / "server.log").open("w+")
                self.server = subprocess.Popen(
                    [str(self.cli), "api-server", "--socket", str(self.socket), *self.server_args],
                    env=self.env, stdout=self.log, stderr=subprocess.STDOUT,
                )
            deadline = time.monotonic() + 30
            while not self.socket.is_socket():
                if (self.server is not None and self.server.poll() is not None) or time.monotonic() > deadline:
                    status = self.server.poll() if self.server is not None else "attached socket missing"
                    raise RuntimeError(f"IPC server did not start: {status}")
                time.sleep(0.05)
            self.connection = pynng.Req0(
                dial=f"ipc://{self.socket}", block_on_dial=True,
                send_timeout=self.timeout, recv_timeout=self.timeout,
            )
            return self
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise

    def module(self, name):
        return importlib.import_module(name + "_pb2")

    def copy_project(self, source, name):
        destination = self.root / name
        shutil.copytree(self.source / source, destination)
        return destination

    def request_raw(self, command, *, type_url=None):
        if os.environ.get("BACKPLANE_IPC_TRACE"):
            print(f"IPC {command.DESCRIPTOR.full_name}", flush=True)
        request = self.envelope.ApiRequest()
        request.header.client_name = self.client_name
        request.header.kicad_token = self.token
        request.message.Pack(command)
        if type_url is not None:
            request.message.type_url = type_url
        try:
            self.connection.send(request.SerializeToString())
            response = self.envelope.ApiResponse.FromString(self.connection.recv())
        except pynng.exceptions.Timeout as error:
            status = self.server.poll() if self.server is not None else "attached GUI"
            raise TimeoutError(
                f"{command.DESCRIPTOR.full_name} timed out; "
                f"IPC server exit status: {status}"
            ) from error
        self.token = response.header.kicad_token
        return response

    def request(self, command, response_type, *, type_url=None):
        response = self.request_raw(command, type_url=type_url)
        if response.status.status != self.envelope.AS_OK:
            raise AssertionError(f"{command.DESCRIPTOR.full_name}: {response.status}")
        result = response_type()
        if not response.message.Unpack(result):
            raise AssertionError(f"Wrong reply type to {command.DESCRIPTOR.full_name}: "
                                 f"{response.message.type_url}")
        return result

    def expect_headless_unavailable(self, *commands):
        """Upstream master's headless server has no editor selection, view layers or actions."""
        for command in commands:
            response = self.request_raw(command)
            assert response.status.status == self.envelope.AS_UNIMPLEMENTED, (
                f"{command.DESCRIPTOR.name} should be unavailable headless: {response.status}")

    def stock_v10_check(self, stock_cli, path):
        """Export a KiCad 10 copy of a saved file and make stock KiCad 10.0.x re-save it.

        Native files from this runtime use upstream master's newer format, so stock
        KiCad only ever sees the output of `kicad-cli <pcb|sch> downgrade --target 10.0`.
        """
        path = Path(path)
        self.v10_exports = getattr(self, "v10_exports", 0) + 1
        out = self.root / f"v10-export-{self.v10_exports}"
        kind = "pcb" if path.suffix == ".kicad_pcb" else "sch"
        if kind == "pcb":
            out.mkdir()  # pcb downgrade writes a file into an existing folder
        # sch downgrade creates the output folder itself and refuses an existing one.
        # pcb downgrade writes a file; sch downgrade writes the hierarchy into a folder.
        output = out / path.name if kind == "pcb" else out
        command = [str(self.cli), kind, "downgrade", "--target", "10.0", "--force", "-o", str(output), str(path)]
        export = subprocess.run(command, env=self.env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=120)
        if export.returncode != 0 and "[block]" in export.stdout:
            # Some features cannot be approximated in 10.0 (e.g. styled line endings); the
            # export then needs --drop-approximations as well as --force.
            print(f"v10 export of {path.name} needs --drop-approximations: "
                  + " ".join(line.strip() for line in export.stdout.splitlines() if "[block]" in line),
                  flush=True)
            shutil.rmtree(out, ignore_errors=True)
            if kind == "pcb":
                out.mkdir()
            export = subprocess.run(command[:6] + ["--drop-approximations"] + command[6:],
                                    env=self.env, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=120)
        assert export.returncode == 0, f"downgrade to 10.0 failed: {export.stdout}"
        copy = out / path.name
        assert copy.is_file(), f"downgrade did not write {copy}: {export.stdout}"
        stock = subprocess.run([str(stock_cli), kind, "upgrade", "--force", str(copy)],
                               env=self.env, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=120)
        if stock.returncode != 0:
            kept = Path(os.environ.get("RUNNER_TEMP", tempfile.gettempdir())) / f"rejected-v10-{copy.name}"
            shutil.copy2(copy, kept)
            raise AssertionError(f"stock KiCad 10 rejected the exported copy (kept at {kept}): {stock.stdout}")
        return copy

    def __exit__(self, exc_type, exc, traceback):
        if self.connection is not None:
            self.connection.close()
        if self.server is not None and self.server.poll() is None:
            self.server.terminate()
            try:
                self.server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait(timeout=10)
        if self.log is not None:
            if exc is not None:
                self.log.seek(0)
                print(self.log.read()[-24000:], file=sys.stderr)
            self.log.close()
        if str(self.bindings) in sys.path:
            sys.path.remove(str(self.bindings))
        self.temporary.cleanup()

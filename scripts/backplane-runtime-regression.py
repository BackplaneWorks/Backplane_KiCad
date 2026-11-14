#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the Backplane runtime handshake against an isolated KiCad CLI.

Covers the fork patches agreed in bp-coord/kicad-fork/CONTRACT.md:
`api-server --describe`, `kiapi.backplane.GetCapabilities`, API-server lifecycle
commands, `api-server --parent-pid`, and Backplane companion files carried
through `project downgrade --target 10.0`.
"""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

from google.protobuf import descriptor_pb2
from google.protobuf.empty_pb2 import Empty

from backplane_ipc_test_support import IpcSession

CAPABILITIES = {"api-server", "headless-pcb", "headless-sch", "export-v10", "parent-pid",
                "capabilities", "sketch-route", "board-structure", "assembly", "idle-exit",
                "shutdown", "api-descriptor", "symbol-from-libid", "footprint-partial-update",
                "drc-zone-guard", "rules-insert-position", "plan-keys"}
if sys.platform == "win32":
    # Windows only: the prewarm never takes the keyboard (verified on Windows)
    CAPABILITIES.add("prewarm-keeps-focus")


def check_describe(cli, env):
    result = subprocess.run([str(cli), "api-server", "--describe"], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    assert result.returncode == 0, result.stderr
    manifest = json.loads(result.stdout)
    assert manifest["manifestVersion"] == 2, manifest
    assert manifest["apiVersion"] >= 1, manifest
    assert CAPABILITIES <= set(manifest["capabilities"]), manifest["capabilities"]
    assert "10.0" in manifest["downgradeTargets"], manifest
    assert manifest["fileFormats"]["board"] > 20260206, manifest
    assert manifest["fileFormats"]["schematic"] > 20260306, manifest
    assert manifest["kicadVersion"].startswith("10.99"), manifest
    assert manifest["apiDescriptor"] == "share/backplane/api.desc", manifest
    descriptor_path = cli.resolve().parent.parent / manifest["apiDescriptor"]
    assert descriptor_path.is_file(), descriptor_path
    descriptors = descriptor_pb2.FileDescriptorSet()
    descriptors.ParseFromString(descriptor_path.read_bytes())
    message_names = {
        f"{file.package}.{message.name}"
        for file in descriptors.file
        for message in file.message_type
    }
    assert "kiapi.backplane.Shutdown" in message_names, sorted(message_names)
    assert "kiapi.common.commands.GetVersion" in message_names, sorted(message_names)
    print(f"PASS --describe: {manifest['version']} (upstream {manifest['upstreamCommit']})", flush=True)
    print(f"PASS api.desc: {len(descriptors.file)} protobuf files", flush=True)
    return manifest


def check_capabilities(api, manifest):
    backplane = api.module("backplane.capabilities")
    reply = api.request(backplane.GetCapabilities(), backplane.GetCapabilitiesResponse)
    assert reply.fork_version == manifest["version"], reply
    assert reply.upstream_commit == manifest["upstreamCommit"], reply
    assert reply.api_version == manifest["apiVersion"], reply
    assert set(reply.capabilities) == set(manifest["capabilities"]), reply
    assert reply.board_file_version == manifest["fileFormats"]["board"], reply
    assert reply.schematic_file_version == manifest["fileFormats"]["schematic"], reply
    commands = set(reply.commands)
    for name in ("kiapi.backplane.GetCapabilities", "kiapi.common.commands.GetVersion"):
        assert name in commands, sorted(commands)
    print(f"PASS GetCapabilities: {len(commands)} registered commands", flush=True)


def check_invalid_idle_exit(cli, env):
    result = subprocess.run([str(cli), "api-server", "--idle-exit", "0"], env=env,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    assert result.returncode != 0, result.stdout
    print("PASS --idle-exit validation: non-positive value returns ERR_ARGS", flush=True)


def check_parent_pid(cli, root, env):
    parent = subprocess.Popen(["sleep", "600"])
    socket = root / "parent-pid" / "api.sock"
    server = subprocess.Popen([str(cli), "api-server", "--socket", str(socket),
                               "--parent-pid", str(parent.pid)],
                              env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        deadline = time.monotonic() + 30
        while not socket.is_socket():
            assert server.poll() is None, f"server exited early: {server.stdout.read()}"
            assert time.monotonic() < deadline, "server did not start"
            time.sleep(0.05)
        time.sleep(1.5)
        assert server.poll() is None, "server exited while its parent was alive"
        parent.kill()
        parent.wait()  # Reap it: a zombie still answers kill(pid, 0).
        server.wait(timeout=10)
        print(f"PASS --parent-pid: server exited ({server.returncode}) after its parent died",
              flush=True)
    finally:
        if parent.poll() is None:
            parent.kill()
            parent.wait()
        if server.poll() is None:
            server.kill()
            server.wait()


def check_idle_exit(cli):
    with IpcSession(cli, server_args=["--idle-exit", "2"]) as api:
        base = api.module("common.commands.base_commands")

        for _ in range(5):
            api.request(base.GetVersion(), base.GetVersionResponse)
            time.sleep(0.45)
            assert api.server.poll() is None, "idle-exit fired while requests were arriving"

        api.server.wait(timeout=5)
        assert api.server.returncode == 0, api.server.returncode
        print("PASS --idle-exit: active requests keep the server up, then idle shutdown exits", flush=True)


def check_idle_exit_keeps_unsaved_work(cli):
    with IpcSession(cli, server_args=["--idle-exit", "2"]) as api:
        project = api.module("common.commands.project_commands")
        commands = api.module("board.board_commands")
        types = api.module("common.types.base_types")
        design = api.copy_project("demos/ecc83", "idle-dirty")
        board_path = design / "ecc83-pp.kicad_pcb"
        document = api.request(project.OpenDocument(type=types.DOCTYPE_PCB, path=str(board_path)),
                               project.OpenDocumentResponse).document
        api.request(commands.SetBoardOrigin(
            board=document, type=commands.BOT_GRID,
            origin=types.Vector2(x_nm=13000000, y_nm=14000000)), Empty)

        # Several idle periods pass without a request; unsaved changes keep the server up.
        time.sleep(7)
        assert api.server.poll() is None, "idle-exit closed a server holding unsaved changes"

        api.request(project.SaveDocument(document=document), Empty)
        api.server.wait(timeout=8)
        assert api.server.returncode == 0, api.server.returncode
        print("PASS --idle-exit: unsaved changes keep the server up; it exits once saved", flush=True)


def check_shutdown(cli):
    with IpcSession(cli) as api:
        lifecycle = api.module("backplane.lifecycle")
        server = api.server
        api.request(lifecycle.Shutdown(force=True), lifecycle.ShutdownResponse)
        server.wait(timeout=5)
        assert server.returncode == 0, server.returncode
        print("PASS Shutdown: force request replies and exits the server", flush=True)


def check_shutdown_dirty_guard(cli):
    with IpcSession(cli) as api:
        project = api.module("common.commands.project_commands")
        commands = api.module("board.board_commands")
        lifecycle = api.module("backplane.lifecycle")
        types = api.module("common.types.base_types")
        design = api.copy_project("demos/ecc83", "shutdown-guard")
        board_path = design / "ecc83-pp.kicad_pcb"
        document = api.request(project.OpenDocument(type=types.DOCTYPE_PCB, path=str(board_path)),
                               project.OpenDocumentResponse).document
        api.request(commands.SetBoardOrigin(
            board=document, type=commands.BOT_GRID,
            origin=types.Vector2(x_nm=11000000, y_nm=12000000)), Empty)

        refused = api.request_raw(lifecycle.Shutdown(force=False))
        assert refused.status.status == api.envelope.AS_BAD_REQUEST, refused
        assert refused.status.error_message == "unsaved changes; save first or pass force", refused
        assert api.server.poll() is None, "rejected Shutdown stopped the server"

        api.request(lifecycle.Shutdown(force=True), lifecycle.ShutdownResponse)
        api.server.wait(timeout=5)
        assert api.server.returncode == 0, api.server.returncode
        print("PASS Shutdown guard: modified document refuses non-force shutdown", flush=True)


def check_downgrade_companions(cli, source, root, env):
    project = root / "companions" / "test_project"
    shutil.copytree(source, project)
    board_companion = project / "test_project.kicad_pcb.backplane.json"
    board_companion.write_text('{"format":"backplane-kicad-metadata","version":1,"items":{}}\n',
                               encoding="utf-8")
    assembly = project / "test_project.backplane-assembly.json"
    assembly.write_text('{"format":"backplane-assembly","version":1}\n', encoding="utf-8")
    # A project folder holds more than KiCad's files: a Python venv (a directory link, a link to
    # an interpreter that is not there) and Backplane's own folder must not stop the downgrade.
    venv = project / ".venv"
    (venv / "lib").mkdir(parents=True)
    (venv / "pyvenv.cfg").write_text("home = /nowhere\n", encoding="utf-8")
    (venv / "lib64").symlink_to("lib")
    (venv / "python").symlink_to("/nowhere/python3")
    (project / ".backplane" / "worktrees").mkdir(parents=True)
    (project / ".backplane" / "worktrees" / "loop").symlink_to("..")
    out = root / "companions" / "test_project-kicad10"
    report = root / "companions" / "report.json"
    result = subprocess.run([str(cli), "project", "downgrade", "--target", "10.0", "--force",
                             "-o", str(out), "--report-json", str(report),
                             str(project / "test_project.kicad_pro")],
                            env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=300)
    assert result.returncode == 0, result.stdout
    for companion in (board_companion, assembly):
        copied = out / companion.name
        assert copied.read_bytes() == companion.read_bytes(), f"{copied} missing or changed"
    assert not (out / ".venv").exists() and not (out / ".backplane").exists(), "tool folders copied"
    text = report.read_text(encoding="utf-8")
    assert board_companion.name in text and assembly.name in text, text
    print("PASS project downgrade: Backplane companions copied and reported; .venv and "
          ".backplane folders (with links) skipped", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    args = parser.parse_args()
    with IpcSession(args.kicad_cli) as api:
        manifest = check_describe(api.cli, api.env)
        check_capabilities(api, manifest)
        check_invalid_idle_exit(api.cli, api.env)
        check_idle_exit(api.cli)
        check_idle_exit_keeps_unsaved_work(api.cli)
        check_shutdown(api.cli)
        check_shutdown_dirty_guard(api.cli)
        check_parent_pid(api.cli, api.root, api.env)
        check_downgrade_companions(api.cli, api.source / "qa/data/libraries/test_project",
                                   api.root, api.env)


if __name__ == "__main__":
    main()

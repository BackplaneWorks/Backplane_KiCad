#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""bp.13.2 (capability headless-no-lock): `kicad-cli api-server` takes no project lock.

1. Opening a project, its board and its schematic headless creates no ~*.lck file in the project
   folder, and OpenDocumentResponse.foreign_lock is unset.
2. A KiCad GUI can take the project lock while the headless server has the project open (the same
   flock KiCad's LOCKFILE takes): a person opening their project gets no "already open" warning.
3. A lock another program holds (KiCad's format: {"username","hostname","token"}, flock held) is
   reported in foreign_lock {lock_file, user, host, held}; the open succeeds with no prompt, a save
   is not refused, and the lock file is left byte for byte and still held after the server ends.
   A stale lock file nobody holds is reported with held = false and also left in place.
4. A server killed with SIGKILL while it has documents open leaves no lock file behind.

    backplane-headless-lock-check.py <kicad-cli>
"""

import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_ipc_test_support import IpcSession  # noqa: E402

FAILURES = []


def check(condition, message):
    print(("PASS " if condition else "FAIL ") + message, flush=True)
    if not condition:
        FAILURES.append(message)


def locks(folder):
    return sorted(p.name for p in folder.rglob("~*.lck"))


def open_all(api, project_dir):
    project = api.module("common.commands.project_commands")
    types = api.module("common.types.base_types")
    pro = next(project_dir.glob("*.kicad_pro"))
    answers = {}
    for kind, path in ((types.DOCTYPE_PROJECT, pro), (types.DOCTYPE_PCB, pro.with_suffix(".kicad_pcb")),
                       (types.DOCTYPE_SCHEMATIC, pro.with_suffix(".kicad_sch"))):
        answers[kind] = api.request(project.OpenDocument(type=kind, path=str(path)), project.OpenDocumentResponse)
    return pro, answers, types


def hold_foreign(lock_path, held=True):
    lock_path.write_text(json.dumps({"username": "person", "hostname": "their-desk", "token": "t0"}))
    if not held:
        return None
    fd = os.open(str(lock_path), os.O_RDWR)
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    return fd


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    args = parser.parse_args()
    # The projects outlive each server session (the session's own folder goes with it)
    work = Path(tempfile.mkdtemp(prefix="headless-lock-"))
    demo = Path(__file__).resolve().parents[1] / "demos/pic_programmer"

    def copy(name):
        return Path(shutil.copytree(demo, work / name))

    # 1, 2: no lock taken; a GUI could take it meanwhile
    with IpcSession(args.kicad_cli) as api:
        folder = copy("plain")
        pro, answers, types = open_all(api, folder)
        check(not locks(folder), f"headless open of project, board and schematic creates no lock file ({locks(folder)})")
        check(all(not a.HasField("foreign_lock") for a in answers.values()),
              "OpenDocumentResponse.foreign_lock is unset with no other lock")
        lock_path = pro.with_name("~" + pro.name + ".lck")
        fd = os.open(str(lock_path), os.O_RDWR | os.O_CREAT, 0o644)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            gui_took = True
        except OSError:
            gui_took = False
        check(gui_took, "a KiCad GUI's project lock (flock) can be taken while the headless server has it open")
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)
        lock_path.unlink()

    # 3: a lock someone else holds, and a stale one
    for held in (True, False):
        label = "held" if held else "stale"
        with IpcSession(args.kicad_cli) as api:
            folder = copy(f"foreign-{label}")
            pro = next(folder.glob("*.kicad_pro"))
            lock_path = pro.with_name("~" + pro.name + ".lck")
            fd = hold_foreign(lock_path, held)
            before = lock_path.read_bytes()
            pro, answers, types = open_all(api, folder)
            got = answers[types.DOCTYPE_PROJECT]
            check(got.HasField("foreign_lock") and got.foreign_lock.lock_file == str(lock_path)
                  and got.foreign_lock.user == "person" and got.foreign_lock.host == "their-desk"
                  and got.foreign_lock.held == held,
                  f"{label}: OpenDocument reports the other lock ({got.foreign_lock})")
            project = api.module("common.commands.project_commands")
            save = api.request_raw(project.SaveDocument(document=answers[types.DOCTYPE_PCB].document))
            check(save.status.status == api.envelope.AS_OK,
                  f"{label}: a headless save is not refused for another program's lock "
                  f"({save.status.status} {save.status.error_message[:80]})")
            check(locks(folder) == [lock_path.name] and lock_path.read_bytes() == before,
                  f"{label}: the other lock file is left as it was while open ({locks(folder)})")
        check(lock_path.exists() and lock_path.read_bytes() == before,
              f"{label}: the server's exit leaves the other lock file untouched")
        if held:
            probe = os.open(str(lock_path), os.O_RDWR)
            try:
                fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
                still_held = False
                fcntl.flock(probe, fcntl.LOCK_UN)
            except OSError:
                still_held = True
            os.close(probe)
            check(still_held, "held: the other program still holds its lock after the server ends")
            os.close(fd)

    # 4: killed with documents open
    api = IpcSession(args.kicad_cli)
    api.__enter__()
    try:
        folder = copy("killed")
        open_all(api, folder)
        api.server.send_signal(signal.SIGKILL)
        api.server.wait(timeout=10)
        time.sleep(0.5)
        check(not locks(folder), f"a server killed with documents open leaves no lock file ({locks(folder)})")
    finally:
        api.__exit__(None, None, None)

    shutil.rmtree(work, ignore_errors=True)
    print(f"\n{len(FAILURES)} failure(s)")
    if not FAILURES:
        print("ALL PASSED")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())

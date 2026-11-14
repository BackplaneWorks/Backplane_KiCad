#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Implied junctions, added on loading a schematic older than the current format
(FixupJunctionsAfterImport), get the same ids on every load, as do wires split at them.

Two fresh copies of qa/data/eeschema/issue18346.kicad_sch (a label where two wires cross: one
needed junction, both wires split) are each opened and saved by a headless `kicad-cli api-server`.
The two saved files must be byte-identical; before, each load made the junction and the split
segments with random ids, so every load-and-save rewrote them differently.

    backplane-implied-junction-ids-check.py KICAD_CLI
"""

import argparse
from pathlib import Path
import re
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backplane_ipc_test_support import IpcSession  # noqa: E402

FAILURES = []


def check(condition, message):
    print(("PASS " if condition else "FAIL ") + message)
    if not condition:
        FAILURES.append(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "qa/data/eeschema/issue18346.kicad_sch"
    original = source.read_text()
    saved = []

    for run in range(2):
        with IpcSession(args.kicad_cli) as api:
            project = api.module("common.commands.project_commands")
            base = api.module("common.types.base_types")
            folder = api.root / f"run{run}"
            folder.mkdir()
            sch = folder / "issue18346.kicad_sch"
            shutil.copy2(source, sch)
            (folder / "issue18346.kicad_pro").write_text('{"meta": {"filename": "issue18346.kicad_pro", "version": 3}}\n')
            doc = api.request(project.OpenDocument(type=base.DOCTYPE_SCHEMATIC, path=str(sch)),
                              project.OpenDocumentResponse).document
            reply = api.request_raw(project.SaveDocument(document=doc))
            check(reply.status.status == api.envelope.AS_OK, f"run {run}: headless open and save "
                                                              f"({reply.status.error_message})")
            saved.append(sch.read_text())

    junctions = [len(re.findall(r"\n\t\(junction", text)) for text in [original, *saved]]
    check(junctions[1] > junctions[0], f"the load added the implied junction ({junctions[0]} -> {junctions[1]})")
    check(saved[0] == saved[1], "two load-and-saves of the same file write the same ids (byte-identical)")
    print("FAILED: %d" % len(FAILURES) if FAILURES else "ALL PASS")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())

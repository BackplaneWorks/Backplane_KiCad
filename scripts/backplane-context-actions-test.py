#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every context action with a default hotkey belongs to a tool of its own prefix.

A TOOL_ACTION with scope AS_CONTEXT (the default) is dispatched only to the tool whose name is
the action's name up to its last dot ("pcbnew.Plan.accept" -> tool "pcbnew.Plan").  If no tool
has that name, the action's hotkey is never the tool's and KiCad's global action on the same key
acts instead: bp.12.1's plan tool was "pcbnew.PlanTool", so A in plan view opened Add Footprint
and R rotated the selection.  This reads the source (no build) and fails on any such action.
"""
import argparse
from collections import defaultdict
from pathlib import Path
import re
import sys
import tempfile

DIRS = ("pcbnew", "eeschema", "common", "gerbview", "pagelayout_editor", "cvpcb", "3d-viewer",
        "pcb_calculator", "kicad")
ACTION = re.compile(r'TOOL_ACTION\s+\w+::\w+\(\s*TOOL_ACTION_ARGS\(\)(.*?)\);', re.S)
# A tool's name is the string its constructor passes to its base: ( "pcbnew.Plan" )
TOOL_NAME = re.compile(r'\(\s*"([a-z0-9]+\.[A-Za-z0-9_.]+)"\s*\)')


def orphans(root):
    actions, tools = [], set()
    for d in DIRS:
        for path in (root / d).rglob("*.cpp"):
            text = path.read_text(errors="ignore")
            for m in ACTION.finditer(text):
                body = m.group(1)
                name = re.search(r'\.Name\(\s*"([^"]+)"', body)
                scope = re.search(r'\.Scope\(\s*(\w+)', body)
                if name and re.search(r'\.DefaultHotkey\(', body) \
                        and (scope.group(1) if scope else "AS_CONTEXT") == "AS_CONTEXT":
                    actions.append(name.group(1))
            tools.update(TOOL_NAME.findall(text))
    found = defaultdict(list)
    for a in actions:
        prefix = a.rsplit(".", 1)[0]
        if prefix not in tools:
            found[prefix].append(a)
    return len(actions), found


def self_test():
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        (root / "pcbnew").mkdir()
        acts = ('TOOL_ACTION PCB_ACTIONS::planAccept( TOOL_ACTION_ARGS()\n .Name( "pcbnew.Plan.accept" )\n'
                ' .Scope( AS_CONTEXT )\n .DefaultHotkey( \'A\' ) );\n')
        (root / "pcbnew/a.cpp").write_text(acts + 'PLAN_TOOL::PLAN_TOOL() : PCB_TOOL_BASE( "pcbnew.PlanTool" ) {}\n')
        assert orphans(root)[1] == {"pcbnew.Plan": ["pcbnew.Plan.accept"]}, "a misnamed tool is found"
        (root / "pcbnew/a.cpp").write_text(acts + 'PLAN_TOOL::PLAN_TOOL() : PCB_TOOL_BASE( "pcbnew.Plan" ) {}\n')
        assert not orphans(root)[1], "a tool named for its actions passes"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, nargs="?", default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    self_test()
    count, found = orphans(args.source)
    for prefix, names in sorted(found.items()):
        print(f"FAIL no tool named {prefix} for its context actions with hotkeys: {', '.join(names)}")
    print(f"{count} context actions with hotkeys; " + ("ALL PASSED" if not found else f"{len(found)} orphan prefixes"))
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())

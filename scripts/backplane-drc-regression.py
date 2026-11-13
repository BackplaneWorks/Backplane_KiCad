#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DRC regressions for the Backplane runtime: zone clearances measured right, never missed.

Usage: backplane-drc-regression.py KICAD_CLI

1. Known gaps (qa/data/pcbnew/backplane_drc): a 1.44 mm PTH pad against a GND fill with a zone
   clearance of 0.508 mm, as a straight edge and as a circular knockout.  Gaps of 0.500 and
   0.507 mm must each be reported once with exactly that actual value; 0.5085 and 0.509 mm must
   not be reported.
2. Zone fills whose triangulation does not tile the fill (demos/complex_hierarchy, GND on
   B.Cu): DRC must check them against the fill outline ("drc-zone-guard").
   a. A foreign-net track lying inside the GND copper at (100.5, 100.9) mm, where the
      triangulation leaves a gap, is reported with actual 0 under the demo's rules, and still
      reported with every clearance rule at 0.1 mm (it used to pass: a missed short).
   b. RV301 pad 3 and its teardrop, 0.5085 and 0.5115 mm from the GND fill (rule 0.508), are not
      reported (they used to be, at 0.4695 and 0.4872 mm, from a triangle covering the knockout).
3. `kicad-cli version --format about` lists the drc-zone-guard capability.
"""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import uuid

ROOT = Path( __file__ ).resolve().parent.parent
FIXTURES = ROOT / "qa" / "data" / "pcbnew" / "backplane_drc"

failures = []


def check( condition, message ):
    print( ( "PASS " if condition else "FAIL " ) + message )

    if not condition:
        failures.append( message )


def run_drc( cli, board, work ):
    out = work / f"{board.stem}.json"
    subprocess.run( [ cli, "pcb", "drc", "--format", "json", "-o", str( out ), str( board ) ],
                    capture_output=True, text=True, timeout=900 )
    return json.loads( out.read_text() )


def clearance_actuals( report ):
    return [ v["description"] for v in report["violations"] if v["type"] == "clearance" ]


def complex_hierarchy_copy( work, name, tight ):
    board_dir = work / name
    shutil.copytree( ROOT / "demos" / "complex_hierarchy", board_dir )
    pcb = board_dir / "complex_hierarchy.kicad_pcb"

    if tight:
        pro = board_dir / "complex_hierarchy.kicad_pro"
        settings = json.loads( pro.read_text() )

        for netclass in settings["net_settings"]["classes"]:
            netclass["clearance"] = 0.1

        settings["board"]["design_settings"]["rules"]["min_clearance"] = 0.05
        pro.write_text( json.dumps( settings, indent=2 ) )

        text = pcb.read_text()
        gnd = text.index( '(net_name "GND")' )
        zone = text.rfind( "(zone", 0, gnd )
        at = text.index( "(clearance 0.508)", zone )
        pcb.write_text( text[:at] + "(clearance 0.1)" + text[at + len( "(clearance 0.508)" ):] )

    return pcb


def add_probe_track( pcb, x, y ):
    text = pcb.read_text()
    net = re.search( r'\(net (\d+) "Net-\(R313-Pad1\)"\)', text ).group( 1 )
    track = ( f'\n\t(segment (start {x} {y}) (end {x + 0.05} {y}) (width 0.1) (layer "B.Cu") '
              f'(net {net}) (uuid "{uuid.uuid4()}"))' )
    end = text.rindex( ")" )
    pcb.write_text( text[:end] + track + "\n" + text[end:] )


def probe_violations( report ):
    return [ v["description"] for v in report["violations"]
             if v["type"] in ( "clearance", "shorting_items" )
             and any( "Net-(R313" in item["description"] and "length 0.05" in item["description"]
                      for item in v["items"] )
             and any( "GND" in item["description"] for item in v["items"] ) ]


def main():
    cli = sys.argv[1]

    with tempfile.TemporaryDirectory( prefix="bp-drc-" ) as tmp:
        work = Path( tmp )

        # 1. Known gaps
        for shape in ( "edge", "ring" ):
            for gap, reported in ( ( "0.5000", True ), ( "0.5070", True ), ( "0.5085", False ),
                                   ( "0.5090", False ) ):
                board = work / f"gap{gap}-{shape}.kicad_pcb"
                shutil.copy( FIXTURES / board.name, board )
                found = clearance_actuals( run_drc( cli, board, work ) )

                if reported:
                    check( len( found ) == 1 and f"actual {gap} mm" in found[0],
                           f"{board.name}: one clearance violation with actual {gap} mm ({found})" )
                else:
                    check( not found, f"{board.name}: no clearance violation ({found})" )

        # 2a. The track inside GND copper where the triangulation has a gap
        for tight in ( False, True ):
            for x, y in ( ( 100.5, 100.9 ), ( 120, 90 ) ):
                pcb = complex_hierarchy_copy( work, f"probe-{tight}-{x}", tight )
                add_probe_track( pcb, x, y )
                found = probe_violations( run_drc( cli, pcb, work ) )
                rules = "0.1 mm rules" if tight else "demo rules"
                check( any( "actual 0.0000 mm" in d for d in found ),
                       f"complex_hierarchy, {rules}: foreign-net track inside GND at ({x}, {y}) reported "
                       f"with actual 0 ({found})" )

        # 2b. No false clearance errors at RV301 pad 3 and its teardrop
        pcb = complex_hierarchy_copy( work, "plain", False )
        report = run_drc( cli, pcb, work )
        wrong = [ v["description"] for v in report["violations"] if v["type"] == "clearance"
                  and any( "RV301" in item["description"] or "Teardrop" in item["description"]
                           for item in v["items"] ) ]
        check( not wrong, f"complex_hierarchy: no clearance error at RV301 pad 3 or its teardrop ({wrong})" )

    # 3. Capability on the command line
    about = subprocess.run( [ cli, "version", "--format", "about" ], capture_output=True, text=True,
                            timeout=60 ).stdout
    check( "drc-zone-guard" in about, "kicad-cli version --format about lists drc-zone-guard" )

    if failures:
        print( f"{len( failures )} DRC regression(s) failed" )
        return 1

    print( "all DRC regressions passed" )
    return 0


if __name__ == "__main__":
    sys.exit( main() )

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DRC regressions for the Backplane runtime: measured clearances, determinism, library parity.

Usage: backplane-drc-regression.py KICAD_CLI

1. Known gaps (qa/data/pcbnew/backplane_drc): a 1.44 mm PTH pad against a GND fill with a zone
   clearance of 0.508 mm, as a straight edge and as a circular knockout.  Gaps of 0.500 and
   0.507 mm must each be reported once with exactly that actual value; 0.5085 and 0.509 mm must
   not be reported.  Guards against DRC missing a real violation as much as inventing one.
2. Determinism: the report (date removed) must be byte-identical at 1 and 8 threads, on a demo
   board and on the parity fixture below.
3. Library parity: a copy of demos/video where one footprint names a library missing from the
   tables, one names a project-table library whose path does not exist, and three name a stock
   library.  The first two must give their lib_footprint_issues violations.
"""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path( __file__ ).resolve().parent.parent
FIXTURES = ROOT / "qa" / "data" / "pcbnew" / "backplane_drc"

failures = []


def check( condition, message ):
    print( ( "PASS " if condition else "FAIL " ) + message )

    if not condition:
        failures.append( message )


def run_drc( cli, board, threads, work ):
    config = work / f"config-t{threads}"
    ( config / "10.99" ).mkdir( parents=True, exist_ok=True )
    ( config / "10.99" / "kicad_advanced" ).write_text( f"MaximumThreads={threads}\n" )
    out = work / f"{board.stem}-t{threads}.json"
    env = dict( __import__( "os" ).environ, KICAD_CONFIG_HOME=str( config ) )
    subprocess.run( [ cli, "pcb", "drc", "--format", "json", "-o", str( out ), str( board ) ],
                    env=env, capture_output=True, text=True, timeout=600 )
    report = json.loads( out.read_text() )
    report.pop( "date", None )
    return report


def clearance_actuals( report ):
    return [ v["description"] for v in report["violations"] if v["type"] == "clearance" ]


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
                found = clearance_actuals( run_drc( cli, board, 8, work ) )

                if reported:
                    check( len( found ) == 1 and f"actual {gap} mm" in found[0],
                           f"{board.name}: one clearance violation with actual {gap} mm ({found})" )
                else:
                    check( not found, f"{board.name}: no clearance violation ({found})" )

        # 2 + 3. Determinism and library parity
        video = work / "video"
        shutil.copytree( ROOT / "demos" / "video", video )
        parity = work / "parity"
        shutil.copytree( ROOT / "demos" / "video", parity )
        pcb = parity / "video.kicad_pcb"
        text = pcb.read_text()
        text = text.replace( '(footprint "footprints:sim72"', '(footprint "NoSuchLib:sim72"', 1 )
        text = text.replace( '(footprint "footprints:subclick"', '(footprint "BrokenLib:subclick"', 1 )
        pcb.write_text( text )
        table = ( parity / "fp-lib-table" ).read_text().rstrip()
        assert table.endswith( ")" )
        table = table[:-1] + ( '  (lib (name "BrokenLib")(type "KiCad")'
                               '(uri "${KIPRJMOD}/nonexistent.pretty")(options "")(descr ""))\n)\n' )
        ( parity / "fp-lib-table" ).write_text( table )

        for board in ( video / "video.kicad_pcb", pcb ):
            one = run_drc( cli, board, 1, work )
            eight = run_drc( cli, board, 8, work )
            check( json.dumps( one ) == json.dumps( eight ),
                   f"{board.parent.name}: report identical at 1 and 8 threads" )

        issues = [ v["description"] for v in eight["violations"] if v["type"] == "lib_footprint_issues" ]
        check( any( "'NoSuchLib'" in d for d in issues ),
               "parity: footprint naming a library missing from the tables is reported" )
        check( any( "'BrokenLib'" in d for d in issues ),
               "parity: footprint naming a project library whose path does not exist is reported" )

    if failures:
        print( f"{len( failures )} DRC regression(s) failed" )
        return 1

    print( "all DRC regressions passed" )
    return 0


if __name__ == "__main__":
    sys.exit( main() )

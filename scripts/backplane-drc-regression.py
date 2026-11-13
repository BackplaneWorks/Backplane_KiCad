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
4. Zone fill triangulation, over every board in demos/ and qa/data/pcbnew: DRC measures zone
   clearances against the fill's triangles, so with KICAD_DRC_CHECK_TRIANGULATION the DRC cache
   reports, per zone layer, the parts of the fill its triangles miss and the parts they cover
   outside it (each eroded by 100 nm, below DRC's resolution and above the triangulator's own
   50 nm simplification), and outline vertices found inside a triangle.  Fill the triangles miss
   (under-cover, so DRC could miss copper) fails; over-cover (an ear that swallowed vertices, a
   source of false clearance errors, known upstream) is listed.  Pass --no-corpus to skip this part.
"""

import json
import os
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


def triangulation_checks( cli, board, work ):
    config = work / "config-t8"
    ( config / "10.99" ).mkdir( parents=True, exist_ok=True )
    ( config / "10.99" / "kicad_advanced" ).write_text( "MaximumThreads=8\n" )
    env = dict( os.environ, KICAD_CONFIG_HOME=str( config ), KICAD_DRC_CHECK_TRIANGULATION="1" )
    proc = subprocess.run( [ cli, "pcb", "drc", "--format", "json", "-o", str( work / "tri.json" ),
                             str( board ) ], env=env, capture_output=True, text=True, timeout=1800 )
    checks = []

    for line in proc.stderr.splitlines():
        if line.startswith( "TRICHECK" ):
            fields = dict( f.split( "=", 1 ) for f in line.split( "\t" )[1:] )
            checks.append( fields )

    return checks


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

        # 4. Zone fill triangulation over the corpus
        if "--no-corpus" not in sys.argv:
            boards = sorted( list( ( ROOT / "demos" ).rglob( "*.kicad_pcb" ) )
                             + list( ( ROOT / "qa" / "data" / "pcbnew" ).rglob( "*.kicad_pcb" ) ) )
            layers = 0
            under = []
            over = []

            for board in boards:
                copy = work / "corpus"
                copy.mkdir( parents=True, exist_ok=True )

                for sibling in board.parent.iterdir():
                    if sibling.is_file() and ( sibling.stem == board.stem
                                               or sibling.name in ( "fp-lib-table", "sym-lib-table" ) ):
                        shutil.copy( sibling, copy / sibling.name )

                for c in triangulation_checks( cli, copy / board.name, work ):
                    layers += 1
                    rel = float( c["rel"] )
                    name = f"{board.relative_to( ROOT )} zone {c['zone']} layer {c['layer']}"

                    # Regions more than 100 nm wide (the comparison erodes by 100 nm): copper the
                    # triangles miss, or triangles where there is no copper
                    if float( c["under_mm2"] ) > 0:
                        under.append( f"{name}: {c['under_mm2']} mm^2 of fill not covered by its triangles "
                                      f"(near {c['under_at']} nm; areas differ by {rel:.2e})" )
                    elif float( c["over_mm2"] ) > 0 or int( c["inside"] ) > 0:
                        over.append( f"{name}: {c['over_mm2']} mm^2 of triangles outside the fill, "
                                     f"{c['inside']} outline vertices inside triangles "
                                     f"(worst {c['worst_nm']} nm at {c['at']})" )

                shutil.rmtree( work / "corpus", ignore_errors=True )

            for line in over:
                print( "WARN " + line )

            check( layers > 0, f"triangulation checked on {layers} zone layers of {len( boards )} boards" )
            check( not under, f"no zone fill under-covered by its triangulation ({len( under )} found)" )

            for line in under:
                print( "FAIL " + line )

    if failures:
        print( f"{len( failures )} DRC regression(s) failed" )
        return 1

    print( "all DRC regressions passed" )
    return 0


if __name__ == "__main__":
    sys.exit( main() )

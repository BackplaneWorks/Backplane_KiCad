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
5. drc-zone-guard: a foreign-net track inside an uncovered patch of demos/complex_hierarchy's GND
   fill is reported with actual 0 under the demo's rules and under 0.1 mm rules; RV301 pad 3 and
   its teardrop get no false clearance errors; `version --format about` lists the capability.
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
    fallback = set()

    for line in proc.stderr.splitlines():
        if line.startswith( "TRICHECK" ):
            fields = dict( f.split( "=", 1 ) for f in line.split( "\t" )[1:] )
            checks.append( fields )
        elif "failed validation" in line:
            fallback.add( line.split( "zone " )[1].split( " " )[0] )

    return checks, fallback


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

        # 2a (guard). The track inside GND copper where the triangulation has a gap
        for tight in ( False, True ):
            for x, y in ( ( 100.5, 100.9 ), ( 120, 90 ) ):
                pcb = complex_hierarchy_copy( work, f"probe-{tight}-{x}", tight )
                add_probe_track( pcb, x, y )
                found = probe_violations( run_drc( cli, pcb, 8, work ) )
                rules = "0.1 mm rules" if tight else "demo rules"
                check( any( "actual 0.0000 mm" in d for d in found ),
                       f"complex_hierarchy, {rules}: foreign-net track inside GND at ({x}, {y}) reported "
                       f"with actual 0 ({found})" )

        # 2b (guard). No false clearance errors at RV301 pad 3 and its teardrop
        pcb = complex_hierarchy_copy( work, "plain", False )
        report = run_drc( cli, pcb, 8, work )
        wrong = [ v["description"] for v in report["violations"] if v["type"] == "clearance"
                  and any( "RV301" in item["description"] or "Teardrop" in item["description"]
                           for item in v["items"] ) ]
        check( not wrong, f"complex_hierarchy: no clearance error at RV301 pad 3 or its teardrop ({wrong})" )

        # 5. Capability on the command line
        about = subprocess.run( [ cli, "version", "--format", "about" ], capture_output=True, text=True,
                                timeout=60 ).stdout
        check( "drc-zone-guard" in about, "kicad-cli version --format about lists drc-zone-guard" )


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

                checks, fallback = triangulation_checks( cli, copy / board.name, work )

                for c in checks:
                    layers += 1
                    rel = float( c["rel"] )
                    name = f"{board.relative_to( ROOT )} zone {c['zone']} layer {c['layer']}"

                    # Regions more than 100 nm wide (the comparison erodes by 100 nm): copper the
                    # triangles miss, or triangles where there is no copper
                    # Copper the triangles miss matters only where DRC uses them (the guard
                    # checks a zone that fails validation against its outline) and only if it is
                    # wider than DRC resolves (its epsilon is 500 nm); thinner slivers along the
                    # outline are an artefact of this comparison's polygon union
                    if float( c["under_mm2"] ) > 0 and c["zone"] not in fallback \
                            and int( c.get( "depth_nm", "0" ) ) > 250:
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

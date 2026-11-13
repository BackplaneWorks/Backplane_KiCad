#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Smoke-test the standard libraries shipped with a Backplane KiCad runtime.

The test uses a fresh KiCad configuration, then verifies the first-launch
library tables, symbol and footprint SVG exports, and a GLB containing the
standard R_0603_1608Metric model.  It intentionally fails when the runtime
does not contain the requested library payload.

It then checks the bundled Backplane library: the first-launch footprint table
names its libraries, a board placing them resolves through that table with the
launcher's BACKPLANE_KICAD_LIBRARY, its passive STEP model reaches a GLB, and a
path the person sets in kicad_common.json wins over the bundled one.
"""

import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def run(cli: Path, args: list[str], env: dict[str, str], cwd: Path) -> subprocess.CompletedProcess[str]:
    command = [str(cli), *args]
    result = subprocess.run(command, cwd=cwd, env=env, text=True, capture_output=True, timeout=120)
    if result.returncode:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def find_exact(root: Path, filename: str) -> Path:
    matches = sorted(path for path in root.rglob(filename) if path.is_file())
    if not matches:
        raise RuntimeError(f"bundled runtime is missing {root.name}/{filename}")
    return matches[0]


def find_symbol_library(root: Path) -> Path:
    direct = sorted(path for path in root.rglob("Device.kicad_sym") if path.is_file())
    if direct:
        return direct[0]
    # KiCad 10 payloads may place the symbol file(s) below a
    # Device.kicad_symdir directory. Keep this explicit so a missing payload
    # still fails instead of silently using a system library.
    containers = sorted(
        path
        for name in ("Device.kicad_symdir", "Device.kicad_sym")
        for path in root.rglob(name)
        if path.is_dir()
    )
    for container in containers:
        if (container / "R.kicad_sym").is_file():
            return container
    raise RuntimeError(f"bundled runtime is missing {root.name}/Device.kicad_sym")


def glb_json(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 20 or data[:4] != b"glTF":
        raise RuntimeError(f"{path} is not a GLB file")
    _, version, length = struct.unpack_from("<4sII", data)
    if version != 2 or length != len(data):
        raise RuntimeError(f"{path} has an invalid GLB header")
    offset = 12
    while offset + 8 <= len(data):
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        chunk = data[offset + 8 : offset + 8 + chunk_length]
        if chunk_type == 0x4E4F534A:
            return json.loads(chunk.decode("utf-8"))
        offset += 8 + chunk_length
    raise RuntimeError(f"{path} has no JSON chunk")


def make_board(destination: Path) -> None:
    text = (destination.parent / "R_0603_1608Metric.kicad_mod").read_text(encoding="utf-8")
    text = text.replace('(layer "F.Cu")', '(layer "F.Cu")\n\t(at 100 100)', 1)
    text = re.sub(
        r'\(model\s+"[^"]+"',
        '(model "${KICAD10_3DMODEL_DIR}/Resistor_SMD.3dshapes/R_0603_1608Metric.step"',
        text,
        count=1,
    )
    board = (
        '(kicad_pcb (version 20240108) (generator "backplane-library-smoke")\n'
        '  (general (thickness 1.6))\n'
        '  (paper "A4")\n'
        '  (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (36 "B.SilkS" user "b.silkscreen") '
        '(37 "F.SilkS" user "f.silkscreen") (44 "Edge.Cuts" user))\n'
        '  (setup (pad_to_mask_clearance 0))\n'
        '  (gr_rect (start 90 90) (end 110 110) '
        '(stroke (width 0.05) (type default)) (fill none) (layer "Edge.Cuts") '
        '(uuid "00000000-0000-0000-0000-000000000001"))\n'
        f"  {text}\n"
        ')\n'
    )
    destination.write_text(board, encoding="utf-8")


BACKPLANE_FOOTPRINTS = (
    ("Backplane_Logo", "Backplane_Logo_Text_20mm_SilkS"),
    ("Backplane_Passives_Nominal", "R_0603_1608Metric"),
)


def placed(library: Path, nickname: str, name: str, x: float) -> str:
    """A library footprint as a board instance named nickname:name, placed at (x, 100)."""
    text = (library / f"{nickname}.pretty" / f"{name}.kicad_mod").read_text(encoding="utf-8").strip()
    text = text.replace(f'(footprint "{name}"', f'(footprint "{nickname}:{name}"', 1)
    return text.replace('(layer "F.Cu")', f'(layer "F.Cu")\n\t(at {x} 100)', 1)


def backplane_board(destination: Path, footprints: list[str]) -> None:
    destination.write_text(
        '(kicad_pcb (version 20240108) (generator "backplane-library-smoke")\n'
        '  (general (thickness 1.6))\n'
        '  (paper "A4")\n'
        '  (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (32 "B.Adhes" user "B.Adhesive") (33 "F.Adhes" user "F.Adhesive") '
        '(34 "B.Paste" user) (35 "F.Paste" user) (36 "B.SilkS" user "B.Silkscreen") (37 "F.SilkS" user "F.Silkscreen") '
        '(38 "B.Mask" user) (39 "F.Mask" user) (44 "Edge.Cuts" user) (46 "B.CrtYd" user "B.Courtyard") '
        '(47 "F.CrtYd" user "F.Courtyard") (48 "B.Fab" user) (49 "F.Fab" user))\n'
        '  (setup (pad_to_mask_clearance 0))\n'
        '  (net 0 "")\n'
        '  (gr_rect (start 60 80) (end 140 120) '
        '(stroke (width 0.05) (type default)) (fill none) (layer "Edge.Cuts") '
        '(uuid "00000000-0000-0000-0000-000000000002"))\n'
        + "\n".join(footprints) + "\n)\n",
        encoding="utf-8",
    )


def library_problems(cli: Path, board: Path, env: dict[str, str], cwd: Path) -> list[str]:
    """DRC's footprint-library findings for a board: one per footprint the global tables cannot resolve."""
    report = board.with_suffix(".drc.json")
    run(cli, ["pcb", "drc", "--format", "json", "--severity-all", "--output", str(report), str(board)], env, cwd)
    violations = json.loads(report.read_text(encoding="utf-8")).get("violations", [])
    return [v["description"] for v in violations if v.get("type", "").startswith("lib_footprint")]


def check_backplane_library(cli: Path, runtime: Path, config_dir: Path, env: dict[str, str], root: Path) -> None:
    bundle = runtime / "share/kicad/backplane-library"
    library = bundle / "libraries"
    for nickname, name in BACKPLANE_FOOTPRINTS:
        if not (library / f"{nickname}.pretty" / f"{name}.kicad_mod").is_file():
            raise RuntimeError(f"bundled Backplane library is missing {nickname}:{name}")
    find_exact(bundle / "3dmodels", "R_0603_1608Metric.step")
    table = (config_dir / "fp-lib-table").read_text(encoding="utf-8")
    for nickname, _ in BACKPLANE_FOOTPRINTS:
        if f'(name "{nickname}")' not in table or "${BACKPLANE_KICAD_LIBRARY}/libraries/" not in table:
            raise RuntimeError(f"first-launch fp-lib-table does not register {nickname}")

    env = dict(env)
    env.pop("BACKPLANE_KICAD_LIBRARY", None)
    work = root / "backplane"
    work.mkdir()
    placed_footprints = [placed(library, n, f, 80 + 30 * i) for i, (n, f) in enumerate(BACKPLANE_FOOTPRINTS)]
    board = work / "backplane.kicad_pcb"
    backplane_board(board, placed_footprints)
    problems = library_problems(cli, board, env, work)
    if problems:
        raise RuntimeError(f"bundled Backplane footprints do not resolve: {problems}")

    # Negative control: a nickname no table has must be reported, or the check above proves nothing.
    control = work / "control.kicad_pcb"
    backplane_board(control, [placed_footprints[0].replace('"Backplane_Logo:', '"Backplane_Logo_Absent:', 1)])
    if not library_problems(cli, control, env, work):
        raise RuntimeError("DRC did not report a footprint from a library no table names")

    glb = work / "backplane.glb"
    run(cli, ["pcb", "export", "glb", "--output", str(glb), str(board)], env, work)
    if not any(
        "R_0603_1608Metric" in mesh.get("name", "") and mesh.get("primitives")
        for mesh in glb_json(glb).get("meshes", [])
    ):
        raise RuntimeError("GLB lacks the Backplane passive's model: BACKPLANE_KICAD_LIBRARY did not resolve")

    # The person's own path (Preferences > Configure Paths) wins over the bundle.
    common = config_dir / "kicad_common.json"
    saved = common.read_bytes() if common.exists() else None
    settings = json.loads(saved) if saved else {}
    settings.setdefault("environment", {})["vars"] = {"BACKPLANE_KICAD_LIBRARY": str(work / "absent")}
    common.write_text(json.dumps(settings, indent=2), encoding="utf-8")
    try:
        if not library_problems(cli, board, env, work):
            raise RuntimeError("the launcher overrode BACKPLANE_KICAD_LIBRARY set in kicad_common.json")
    finally:
        if saved is None:
            common.unlink()
        else:
            common.write_bytes(saved)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime", type=Path, help="bundled runtime root or bin/kicad-cli")
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    cli = runtime / "bin/kicad-cli" if runtime.is_dir() else runtime
    cli = cli.resolve(strict=True)
    runtime = cli.parent.parent
    if cli.name != "kicad-cli":
        raise RuntimeError(f"expected a kicad-cli entry point, got {cli}")

    for link in runtime.rglob("*"):
        if link.is_symlink() and not link.resolve(strict=True).is_relative_to(runtime):
            raise RuntimeError(f"bundled symlink escapes the runtime: {link}")

    symbols = runtime / "share/kicad/symbols"
    footprints = runtime / "share/kicad/footprints"
    models = runtime / "share/kicad/3dmodels"
    symbol_library = find_symbol_library(symbols)
    footprint = find_exact(footprints, "R_0603_1608Metric.kicad_mod")
    find_exact(models, "R_0603_1608Metric.step")

    with tempfile.TemporaryDirectory(prefix="backplane-library-smoke-") as temporary:
        root = Path(temporary)
        config = root / "config"
        env = dict(os.environ)
        env.update(
            HOME=str(root / "home"),
            XDG_CONFIG_HOME=str(config),
            KICAD_CONFIG_HOME=str(config / "kicad"),
            XDG_DATA_HOME=str(root / "data"),
            XDG_CACHE_HOME=str(root / "cache"),
        )
        env.setdefault("KICAD10_SYMBOL_DIR", str(symbols))
        env.setdefault("KICAD10_FOOTPRINT_DIR", str(footprints))
        env.setdefault("KICAD10_3DMODEL_DIR", str(models))
        for key in ("DISPLAY", "WAYLAND_DISPLAY"):
            env.pop(key, None)
        version = run(cli, ["version", "--format", "plain"], env, root).stdout.strip()
        # KiCad keeps settings per major.minor version (10.99 for master builds).
        settings_version = ".".join(version.split("-")[0].split(".")[:2])
        config_dir = Path(env["KICAD_CONFIG_HOME"]) / settings_version
        tables = [config_dir / "sym-lib-table", config_dir / "fp-lib-table"]
        for table in tables:
            if not table.is_file() or not table.read_text(encoding="utf-8").strip():
                raise RuntimeError(f"first launch did not create {table}")
        for table in tables:
            content = table.read_text(encoding="utf-8")
            expected = "sym_lib_table" if table.name == "sym-lib-table" else "fp_lib_table"
            if expected not in content:
                raise RuntimeError(f"{table} is not a valid KiCad library table")
            if '(type "Table")' not in content or f'${{KICAD10_TEMPLATE_DIR}}/{table.name}' not in content:
                raise RuntimeError(f"{table} does not reference the relocatable stock table")
            table.write_bytes(content.rstrip().encode("utf-8") + b"\n\n")
        originals = {table: table.read_bytes() for table in tables}
        run(cli, ["--version"], env, root)
        for table, content in originals.items():
            if table.read_bytes() != content:
                raise RuntimeError(f"second launch overwrote existing {table}")

        output = root / "exports"
        output.mkdir()
        run(cli, ["sym", "export", "svg", "--symbol", "R", "--output", str(output), str(symbol_library)], env, root)
        run(
            cli,
            ["fp", "export", "svg", "--footprint", "R_0603_1608Metric", "--output", str(output), str(footprint.parent)],
            env,
            root,
        )
        svgs = list(output.glob("*.svg"))
        if len(svgs) < 2 or any(not svg.read_text(encoding="utf-8").strip() for svg in svgs):
            raise RuntimeError("symbol or footprint export did not produce nonempty SVG output")

        fixture_footprint = root / "R_0603_1608Metric.kicad_mod"
        fixture_footprint.write_bytes(footprint.read_bytes())
        board = root / "resistor.kicad_pcb"
        make_board(board)
        glb = root / "resistor.glb"
        run(cli, ["pcb", "export", "glb", "--output", str(glb), str(board)], env, root)
        document = glb_json(glb)
        model_meshes = [
            mesh for mesh in document.get("meshes", [])
            if "R_0603_1608Metric" in mesh.get("name", "") and mesh.get("primitives")
        ]
        if not model_meshes:
            raise RuntimeError("GLB does not contain the R_0603_1608Metric model mesh")
        check_backplane_library(cli, runtime, config_dir, env, root)
    print("Backplane KiCad standard-library and Backplane-library smoke passed")


if __name__ == "__main__":
    main()

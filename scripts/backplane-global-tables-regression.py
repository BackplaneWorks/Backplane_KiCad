#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Global library tables of a packaged runtime, from an empty config home (global-tables-seeded).

    backplane-global-tables-regression.py <runtime>/bin/kicad-cli[.exe]

1. Seeding: a first start in a config home with no tables writes the runtime's default
   sym-lib-table, fp-lib-table and design-block-lib-table, byte for byte, into this version's
   folder only.  A stock KiCad 10.0 folder beside it stays byte-identical; a table the person
   already has (their own, without KiCad's entries, or one that does not even parse) stays
   byte-identical; a second start writes nothing.  On Linux the launcher seeds first and KiCad's
   own seeding has nothing to do, which this proves as well.
   The seeded fp-lib-table names both KiCad's nested table and the Backplane library.
2. Resolution, over IPC from another empty config home: PlaceSymbolFromLibrary("Device:C") and
   GetItemsFromLibrary("Capacitor_SMD:C_0805_2012Metric") come from the global tables, which
   nest ${KICAD10_TEMPLATE_DIR}/<table>; Backplane_Passives_Nominal:C_0805_2012Metric resolves
   through ${BACKPLANE_KICAD_LIBRARY} (KiCad's built-in default; the launcher's on Linux); a
   nickname no table names does not resolve.
3. 3D models: ${KICAD10_3DMODEL_DIR}/Capacitor_SMD.3dshapes/C_0805_2012Metric.step and
   ${BACKPLANE_KICAD_LIBRARY}/3dmodels/Capacitor_SMD.3dshapes/C_0805_2012Metric.step are in the
   runtime, and a board placing Capacitor_SMD:C_0805_2012Metric (or
   Backplane_Passives_Nominal:C_0805_2012Metric) exports a GLB carrying that model's mesh, while
   the same board naming an absent model does not.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time

TABLES = ("sym-lib-table", "fp-lib-table", "design-block-lib-table")
BACKPLANE_LIBRARIES = ("Backplane_Logo", "Backplane_MountingHole", "Backplane_Passives_Least",
                       "Backplane_Passives_Most", "Backplane_Passives_Nominal", "Backplane_Shoggoth")
MODEL = "Capacitor_SMD.3dshapes/C_0805_2012Metric.step"
SOURCE = Path(__file__).resolve().parents[1]


def stock_data_dir(cli: Path) -> Path:
    root = cli.parent.parent
    for candidate in (root / "KiCad.app/Contents/SharedSupport", root / "share/kicad"):
        if (candidate / "fp-lib-table").is_file():
            return candidate
    raise SystemExit(f"FAIL no default global tables beside {cli}")


def isolated_env(home: Path) -> dict:
    env = dict(os.environ)
    for variable, directory in (("HOME", "home"), ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache"),
                                ("XDG_CONFIG_HOME", "config"), ("KICAD_CONFIG_HOME", "config/kicad")):
        path = home / directory
        if variable != "KICAD_CONFIG_HOME":
            path.mkdir(parents=True, exist_ok=True)
        env[variable] = str(path)
    for variable in ("DISPLAY", "WAYLAND_DISPLAY", "BACKPLANE_KICAD_LIBRARY", "KICAD10_TEMPLATE_DIR",
                     "KICAD10_SYMBOL_DIR", "KICAD10_FOOTPRINT_DIR", "KICAD10_3DMODEL_DIR"):
        env.pop(variable, None)
    return env


def run(cli: Path, args: list, env: dict, cwd: Path) -> subprocess.CompletedProcess:
    result = subprocess.run([str(cli), *args], env=env, cwd=cwd, text=True, capture_output=True, timeout=180)
    assert result.returncode == 0, f"{args}: {result.returncode}\n{result.stdout}\n{result.stderr}"
    return result


def check_seeding(cli: Path, stock: Path, root: Path) -> str:
    env = isolated_env(root / "seed")
    version = run(cli, ["version", "--format", "plain"], env, root).stdout.strip()
    settings_version = ".".join(version.split("-")[0].split(".")[:2])
    config = Path(env["KICAD_CONFIG_HOME"])
    # The `version` call above already started KiCad once: start over from nothing
    shutil.rmtree(config, ignore_errors=True)

    # A stock KiCad 10.0 folder beside the fork's
    stock10 = config / "10.0"
    stock10.mkdir(parents=True)
    stock10_files = {
        "sym-lib-table": b"(sym_lib_table\n\t(version 7)\n\t(lib (name \"Mine\")(type \"KiCad\")"
                         b"(uri \"/mine.kicad_sym\")(options \"\")(descr \"\"))\n)\n",
        "fp-lib-table": b"(fp_lib_table\n\t(version 7)\n)\n",
    }
    for name, data in stock10_files.items():
        (stock10 / name).write_bytes(data)

    first = run(cli, ["version"], env, root)
    mine = config / settings_version
    for table in TABLES:
        seeded = mine / table
        assert seeded.is_file(), f"first start did not write {seeded}\n{first.stderr}"
        assert seeded.read_bytes() == (stock / table).read_bytes(), f"{seeded} is not the runtime's default"
    by = "KiCad" if "Backplane: seeded global" in first.stderr else "the launcher"
    fp_table = (mine / "fp-lib-table").read_text(encoding="utf-8")
    assert '(name "KiCad")(type "Table")(uri "${KICAD10_TEMPLATE_DIR}/fp-lib-table")' in fp_table, fp_table
    for nickname in BACKPLANE_LIBRARIES:
        assert f'(name "{nickname}")(type "KiCad")(uri "${{BACKPLANE_KICAD_LIBRARY}}/libraries/{nickname}.pretty")' \
            in fp_table, f"the seeded fp-lib-table does not name {nickname}"
    for name, data in stock10_files.items():
        assert (stock10 / name).read_bytes() == data, f"10.0/{name} changed"
    assert sorted(p.name for p in stock10.iterdir()) == sorted(stock10_files), \
        f"10.0 gained files: {sorted(p.name for p in stock10.iterdir())}"
    assert not list(mine.glob("*.seed-*")), "a temporary seed file was left behind"

    # A second start writes nothing
    before = {table: (mine / table).read_bytes() for table in TABLES}
    second = run(cli, ["version"], env, root)
    assert "Backplane: seeded global" not in second.stderr, second.stderr
    assert {table: (mine / table).read_bytes() for table in TABLES} == before, "second start rewrote a table"

    # The person's own tables: one without KiCad's entries, one that does not parse
    own_env = isolated_env(root / "own")
    own = Path(own_env["KICAD_CONFIG_HOME"]) / settings_version
    own.mkdir(parents=True)
    own_fp = (b"(fp_lib_table\n\t(version 7)\n\t(lib (name \"amemb\")(type \"KiCad\")"
              b"(uri \"/home/someone/amemb.pretty\")(options \"\")(descr \"\"))\n)\n")
    broken_sym = b"(sym_lib_table (version 7) (lib (name"
    (own / "fp-lib-table").write_bytes(own_fp)
    (own / "sym-lib-table").write_bytes(broken_sym)
    run(cli, ["version"], own_env, root)
    assert (own / "fp-lib-table").read_bytes() == own_fp, "the person's fp-lib-table changed"
    assert (own / "sym-lib-table").read_bytes() == broken_sym, "the person's (broken) sym-lib-table changed"
    assert (own / "design-block-lib-table").read_bytes() == (stock / "design-block-lib-table").read_bytes(), \
        "the missing design-block-lib-table was not seeded beside the person's tables"
    print(f"PASS seeding ({settings_version}, by {by}): defaults written once, 10.0 and the person's "
          "tables byte-identical", flush=True)
    return settings_version


class Api:
    """A headless api-server in an empty config home (named pipes on Windows, sockets elsewhere)."""

    def __init__(self, cli: Path, root: Path):
        self.root = root
        bindings = root / "bindings"
        bindings.mkdir()
        proto = SOURCE / "api/proto"
        subprocess.run(["protoc", f"-I{proto}", f"--python_out={bindings}",
                        *map(str, sorted(proto.rglob("*.proto")))], check=True)
        sys.path.insert(0, str(bindings))
        import pynng
        from common import envelope_pb2
        self.envelope = envelope_pb2
        self.env = isolated_env(root / "ipc")
        self.socket = root / "api.sock"
        self.log = (root / "server.log").open("w+")
        self.server = subprocess.Popen([str(cli), "api-server", "--socket", str(self.socket),
                                        "--parent-pid", str(os.getpid())],
                                       env=self.env, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 60
        while (os.name != "nt" and not self.socket.is_socket()) or \
                (os.name == "nt" and "KiCad API server listening at " not in (root / "server.log").read_text()):
            if self.server.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError(f"api-server did not start ({self.server.poll()}):\n{self.tail()}")
            time.sleep(0.1)
        self.connection = pynng.Req0(dial=f"ipc://{self.socket}", block_on_dial=(os.name != "nt"),
                                     send_timeout=180000, recv_timeout=180000)
        self.token = ""

    def tail(self):
        self.log.flush()
        return (self.root / "server.log").read_text()[-6000:]

    def raw(self, command):
        request = self.envelope.ApiRequest()
        request.header.client_name = "works.backplane.global-tables"
        request.header.kicad_token = self.token
        request.message.Pack(command)
        self.connection.send(request.SerializeToString())
        response = self.envelope.ApiResponse.FromString(self.connection.recv())
        self.token = response.header.kicad_token
        return response

    def request(self, command, response_type):
        response = self.raw(command)
        assert response.status.status == self.envelope.AS_OK, f"{command.DESCRIPTOR.full_name}: {response.status}"
        result = response_type()
        assert response.message.Unpack(result), response.message.type_url
        return result

    def close(self):
        self.connection.close()
        if self.server.poll() is None:
            self.server.terminate()
            try:
                self.server.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait(timeout=20)
        self.log.close()


def check_resolution(cli: Path, stock: Path, settings_version: str, root: Path) -> None:
    api = Api(cli, root)
    try:
        from common.commands import editor_commands_pb2 as editor
        from common.commands import library_commands_pb2 as library
        from common.commands import project_commands_pb2 as project
        from common.types import base_types_pb2 as types
        from common.types import library_types_pb2 as library_types
        from schematic import schematic_commands_pb2 as sch_commands
        from schematic import schematic_types_pb2 as schematic

        config = Path(api.env["KICAD_CONFIG_HOME"]) / settings_version
        for table in TABLES:
            assert (config / table).is_file(), f"the api-server's first start did not seed {table}"

        # A project of its own, with no project library tables: only the global ones can answer
        design = root / "design"
        design.mkdir()
        for suffix in ("kicad_pro", "kicad_sch", "kicad_pcb"):
            shutil.copy(SOURCE / f"qa/data/libraries/test_project/test_project.{suffix}", design / f"gate.{suffix}")
        board = api.request(project.OpenDocument(type=types.DOCTYPE_PCB, path=str(design / "gate.kicad_pcb")),
                            project.OpenDocumentResponse).document
        sheet = api.request(project.OpenDocument(type=types.DOCTYPE_SCHEMATIC, path=str(design / "gate.kicad_sch")),
                            project.OpenDocumentResponse).document

        placed = api.request(sch_commands.PlaceSymbolFromLibrary(
            header=types.ItemHeader(document=sheet),
            lib_id=types.LibraryIdentifier(library_nickname="Device", entry_name="C"),
            position=types.Vector2(x_nm=50800000, y_nm=50800000)), library.PlaceFromLibraryResponse)
        symbol = schematic.SchematicSymbolInstance()
        assert placed.item.Unpack(symbol), placed.item.type_url
        assert any(child.item.type_url.endswith("SchematicPin") for child in symbol.definition.items), \
            "Device:C came back without pins"
        assert symbol.definition.id.library_nickname == "Device", symbol.definition.id
        print(f"PASS PlaceSymbolFromLibrary Device:C ({sum(c.item.type_url.endswith('SchematicPin') for c in symbol.definition.items)} pins)",
              flush=True)

        def footprint(nickname, name):
            response = api.raw(library.GetItemsFromLibrary(
                type=library_types.LT_FOOTPRINT, document=board,
                item_ids=[types.LibraryIdentifier(library_nickname=nickname, entry_name=name)]))
            if response.status.status != api.envelope.AS_OK:
                return []
            items = editor.GetItemsResponse()
            assert response.message.Unpack(items), response.message.type_url
            return list(items.items)

        items = footprint("Capacitor_SMD", "C_0805_2012Metric")
        assert items and "Footprint" in items[0].type_url, f"Capacitor_SMD:C_0805_2012Metric did not resolve: {items}"
        print(f"PASS GetItemsFromLibrary Capacitor_SMD:C_0805_2012Metric ({items[0].type_url.rsplit('/', 1)[-1]})",
              flush=True)
        items = footprint("Backplane_Passives_Nominal", "C_0805_2012Metric")
        assert items, "Backplane_Passives_Nominal:C_0805_2012Metric did not resolve through ${BACKPLANE_KICAD_LIBRARY}"
        print("PASS GetItemsFromLibrary Backplane_Passives_Nominal:C_0805_2012Metric", flush=True)
        # Negative control: a library no table names
        assert not footprint("Capacitor_SMD_Absent", "C_0805_2012Metric"), \
            "a nickname no table names resolved: the check above proves nothing"
        print("PASS negative control: an unnamed library does not resolve", flush=True)
    except BaseException:
        print(api.tail(), file=sys.stderr)
        raise
    finally:
        api.close()


def glb_meshes(path: Path) -> list:
    data = path.read_bytes()
    assert data[:4] == b"glTF", f"{path} is not a GLB"
    offset = 12
    while offset + 8 <= len(data):
        length, kind = struct.unpack_from("<II", data, offset)
        if kind == 0x4E4F534A:
            document = json.loads(data[offset + 8:offset + 8 + length].decode("utf-8"))
            return [mesh.get("name", "") for mesh in document.get("meshes", []) if mesh.get("primitives")]
        offset += 8 + length
    raise AssertionError(f"{path} has no JSON chunk")


def board_with(footprint: str) -> str:
    return ('(kicad_pcb (version 20240108) (generator "backplane-global-tables")\n'
            '  (general (thickness 1.6))\n  (paper "A4")\n'
            '  (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (34 "B.Paste" user) (35 "F.Paste" user) '
            '(36 "B.SilkS" user "B.Silkscreen") (37 "F.SilkS" user "F.Silkscreen") (38 "B.Mask" user) '
            '(39 "F.Mask" user) (44 "Edge.Cuts" user) (46 "B.CrtYd" user "B.Courtyard") '
            '(47 "F.CrtYd" user "F.Courtyard") (48 "B.Fab" user) (49 "F.Fab" user))\n'
            '  (setup (pad_to_mask_clearance 0))\n  (net 0 "")\n'
            '  (gr_rect (start 90 90) (end 110 110) (stroke (width 0.05) (type default)) (fill none) '
            '(layer "Edge.Cuts") (uuid "00000000-0000-0000-0000-0000000000b1"))\n'
            f"{footprint}\n)\n")


def check_model(cli: Path, stock: Path, root: Path, nickname: str, footprint_file: Path, variable: str,
                model: Path, relative: str = MODEL) -> None:
    """A board placing <nickname>:C_0805_2012Metric exports a GLB with its model; an absent model does not."""
    assert model.is_file() and model.stat().st_size > 1000, f"the runtime lacks {model}"
    source = footprint_file.read_text(encoding="utf-8")
    # <relative> is the model's path under the variable: KiCad's own libraries keep their models in
    # <dir>/<lib>.3dshapes, the Backplane library in <dir>/3dmodels/<lib>.3dshapes
    reference = f'(model "${{{variable}}}/{relative}"'
    assert reference in source, f"{footprint_file.name} no longer names {reference}"
    placed = source.strip().replace('(footprint "C_0805_2012Metric"', f'(footprint "{nickname}:C_0805_2012Metric"', 1)
    placed = placed.replace('(layer "F.Cu")', '(layer "F.Cu")\n\t(at 100 100)', 1)
    env = isolated_env(root / f"model-{nickname}")
    work = root / f"model-work-{nickname}"
    work.mkdir()
    meshes = {}
    for name, text in (("with", placed), ("control", placed.replace(MODEL, "Capacitor_SMD.3dshapes/Absent.step"))):
        board = work / f"{name}.kicad_pcb"
        board.write_text(board_with(text), encoding="utf-8")
        glb = work / f"{name}.glb"
        run(cli, ["pcb", "export", "glb", "--output", str(glb), str(board)], env, work)
        meshes[name] = ([m for m in glb_meshes(glb) if "C_0805_2012Metric" in m], glb.stat().st_size)
    assert meshes["with"][0], f"{nickname}: the GLB lacks the C_0805_2012Metric model: ${{{variable}}} did not resolve"
    assert not meshes["control"][0], f"{nickname}: the control (absent model) still has the mesh: the check proves nothing"
    print(f"PASS model: {nickname}:C_0805_2012Metric's ${{{variable}}}/{relative} in the GLB "
          f"({meshes['with'][1]} bytes, {meshes['control'][1]} without it)", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("cli", type=Path)
    args = parser.parse_args()
    cli = args.cli.resolve(strict=True)
    stock = stock_data_dir(cli)
    with tempfile.TemporaryDirectory(prefix="bp-global-tables-") as temporary:
        root = Path(temporary)
        settings_version = check_seeding(cli, stock, root)
        check_resolution(cli, stock, settings_version, root)
        check_model(cli, stock, root, "Capacitor_SMD",
                    stock / "footprints/Capacitor_SMD.pretty/C_0805_2012Metric.kicad_mod",
                    "KICAD10_3DMODEL_DIR", stock / "3dmodels" / MODEL)
        check_model(cli, stock, root, "Backplane_Passives_Nominal",
                    stock / "backplane-library/libraries/Backplane_Passives_Nominal.pretty/C_0805_2012Metric.kicad_mod",
                    "BACKPLANE_KICAD_LIBRARY", stock / "backplane-library/3dmodels" / MODEL, "3dmodels/" + MODEL)
    print("PASS global tables: seeded from the runtime; Device:C, Capacitor_SMD and Backplane_Passives_Nominal C_0805_2012Metric and both models resolve", flush=True)


if __name__ == "__main__":
    main()

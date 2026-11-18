#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A KiCad HTTP library's part placed through the headless KiCad (kicad-cli api-server) has its symbol.

Backplane issue #1192: an HTTP library part names the symbol it is drawn with (symbolIdStr,
"Device:R").  The headless KiCad loads a library only when it is asked for, so when the HTTP
library built its parts the symbol's own library had not loaded: each part became a symbol of fields
alone (no pins, no body), was cached so, and was placed so for the rest of the session.

A REST stub serves two parts; the global symbol table (an empty config home of this check's own)
holds that HTTP library and Device.  On a fresh server, with the HTTP part the first thing placed:

1. PlaceSymbolFromLibrary("My_Parts:470R ...") gives the part with Device:R's pins and body, and the
   part's own fields (value, footprint, MPN).
2. A part whose symbol cannot be had ("Device:NoSuchSymbol") is refused: the API answers with an
   error naming the part (the library adapter turns the plugin's error into "not found"); it is not
   placed as a symbol of fields alone, and no caller gets an exception for it.
3. The first part placed again in the same session is still whole.

    backplane-httplib-headless-check.py <runtime>/bin/kicad-cli
"""

import argparse
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

SOURCE = Path(__file__).resolve().parents[1]
PART = "470R 1206 1% 250mW 200V"
BROKEN = "Missing symbol part"
PARTS = {
    "1": {"id": "1", "name": PART, "symbolIdStr": "Device:R",
          "exclude_from_bom": "False", "exclude_from_board": "False", "exclude_from_sim": "True",
          "fields": {"footprint": {"value": "Resistor_SMD:R_1206_3216Metric", "visible": "False"},
                     "value": {"value": "470R"}, "reference": {"value": "R"},
                     "MPN": {"value": "RC1206FR-07470RL", "visible": "False"}}},
    "2": {"id": "2", "name": BROKEN, "symbolIdStr": "Device:NoSuchSymbol",
          "exclude_from_bom": "False", "exclude_from_board": "False", "exclude_from_sim": "True",
          "fields": {"value": {"value": "X"}, "reference": {"value": "U"}}},
}
FAILURES = []


def check(condition, message):
    print(("PASS " if condition else "FAIL ") + message, flush=True)
    if not condition:
        FAILURES.append(message)


class Stub(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        path = self.path.split("?")[0].rstrip("/")
        body = {"/v1": {"categories": "", "parts": ""},
                "/v1/categories.json": [{"id": "16", "name": "Resistors", "description": ""}],
                "/v1/parts/category/16.json": [{"id": p["id"], "name": p["name"], "description": ""}
                                               for p in PARTS.values()]}.get(path)
        if body is None and path.startswith("/v1/parts/") and path.endswith(".json"):
            body = PARTS.get(path[len("/v1/parts/"):-len(".json")])
        if body is None:
            self.send_response(404)
            self.end_headers()
            return
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def library_dir(cli: Path) -> Path:
    root = cli.resolve().parent.parent
    for candidate in (root / "KiCad.app/Contents/SharedSupport/symbols", root / "share/kicad/symbols"):
        if candidate.is_dir():
            return candidate
    raise SystemExit(f"FAIL no symbol libraries beside {cli}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("kicad_cli", type=Path)
    args = parser.parse_args()
    cli = args.kicad_cli.resolve(strict=True)
    root = Path(tempfile.mkdtemp(prefix="httplib-headless-"))
    stub = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub)
    threading.Thread(target=stub.serve_forever, daemon=True).start()
    server = None
    try:
        bindings = root / "bindings"
        bindings.mkdir()
        proto = SOURCE / "api/proto"
        subprocess.run(["protoc", f"-I{proto}", f"--python_out={bindings}",
                        *map(str, sorted(proto.rglob("*.proto")))], check=True)
        sys.path.insert(0, str(bindings))
        import pynng
        from common import envelope_pb2 as envelope
        from common.commands import project_commands_pb2 as project
        from common.commands import library_commands_pb2 as library
        from common.types import base_types_pb2 as types
        from schematic import schematic_commands_pb2 as sch_commands
        from schematic import schematic_types_pb2 as schematic

        env = dict(os.environ)
        for variable, directory in (("HOME", "home"), ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache"),
                                    ("XDG_CONFIG_HOME", "config"), ("KICAD_CONFIG_HOME", "config/kicad")):
            (root / directory).mkdir(parents=True, exist_ok=True)
            env[variable] = str(root / directory)
        for variable in ("DISPLAY", "WAYLAND_DISPLAY"):
            env.pop(variable, None)

        # the HTTP library and a global table of Device and it, in this version's settings folder
        httplib = root / "My_Parts.kicad_httplib"
        httplib.write_text(json.dumps({"meta": {"version": 1.0}, "name": "My_Parts", "description": "",
                                       "source": {"type": "REST_API", "api_version": "v1",
                                                  "root_url": f"http://127.0.0.1:{stub.server_address[1]}",
                                                  "token": "check", "timeout_parts_seconds": 60,
                                                  "timeout_categories_seconds": 600}}))
        symbols = library_dir(cli)
        device = symbols / "Device.kicad_symdir"
        if not device.exists():
            device = symbols / "Device.kicad_sym"
        version = subprocess.check_output([str(cli), "--version"], text=True, env=env).strip()
        settings_version = ".".join(version.split("-")[0].split(".")[:2])
        table = ("(sym_lib_table\n  (version 7)\n"
                 f"  (lib (name \"Device\")(type \"KiCad\")(uri \"{device.as_posix()}\")(options \"\")(descr \"\"))\n"
                 f"  (lib (name \"My_Parts\")(type \"HTTP\")(uri \"{httplib.as_posix()}\")(options \"\")(descr \"\"))\n)\n")
        config = root / "config/kicad" / settings_version
        config.mkdir(parents=True, exist_ok=True)
        (config / "sym-lib-table").write_text(table)
        (config / "fp-lib-table").write_text("(fp_lib_table\n  (version 7)\n)\n")

        design = root / "design"
        design.mkdir()
        shutil.copy(SOURCE / "qa/data/libraries/test_project/test_project.kicad_sch", design / "check.kicad_sch")
        shutil.copy(SOURCE / "qa/data/libraries/test_project/test_project.kicad_pro", design / "check.kicad_pro")

        socket = root / "api.sock"
        log = (root / "server.log").open("w+")
        server = subprocess.Popen([str(cli), "api-server", "--socket", str(socket), "--parent-pid", str(os.getpid())],
                                  env=env, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 60
        while (os.name != "nt" and not socket.is_socket()) or \
                (os.name == "nt" and "KiCad API server listening at " not in (root / "server.log").read_text()):
            if server.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError(f"api-server did not start ({server.poll()})")
            time.sleep(0.1)
        connection = pynng.Req0(dial=f"ipc://{socket}", block_on_dial=(os.name != "nt"),
                                send_timeout=180000, recv_timeout=180000)
        token = [""]

        def raw(command):
            request = envelope.ApiRequest()
            request.header.client_name = "works.backplane.httplib-headless"
            request.header.kicad_token = token[0]
            request.message.Pack(command)
            connection.send(request.SerializeToString())
            response = envelope.ApiResponse.FromString(connection.recv())
            token[0] = response.header.kicad_token
            return response

        opened = raw(project.OpenDocument(type=types.DOCTYPE_SCHEMATIC, path=str(design / "check.kicad_sch")))
        assert opened.status.status == envelope.AS_OK, opened.status
        answer = project.OpenDocumentResponse()
        opened.message.Unpack(answer)
        sheet = answer.document

        def place(entry, x):
            response = raw(sch_commands.PlaceSymbolFromLibrary(
                header=types.ItemHeader(document=sheet),
                lib_id=types.LibraryIdentifier(library_nickname="My_Parts", entry_name=entry),
                position=types.Vector2(x_nm=x, y_nm=50800000)))
            if response.status.status != envelope.AS_OK:
                return None, response.status.error_message
            placed = library.PlaceFromLibraryResponse()
            response.message.Unpack(placed)
            symbol = schematic.SchematicSymbolInstance()
            assert placed.item.Unpack(symbol), placed.item.type_url
            return symbol, ""

        def kinds(symbol):
            return [child.item.type_url.rsplit(".", 1)[-1] for child in symbol.definition.items]

        def fields(symbol):
            names = {}
            for child in symbol.definition.items:
                if child.item.type_url.endswith("SchematicField"):
                    field = schematic.SchematicField()
                    child.item.Unpack(field)
                    names[field.name] = field.text.text
            return names

        # 1. cold: nothing has loaded Device yet, the HTTP part is the first thing placed
        symbol, error = place(PART, 50800000)
        check(symbol is not None, f"the HTTP part is placed ({error})")
        if symbol is not None:
            got = kinds(symbol)
            check(got.count("SchematicPin") == 2 and any(k != "SchematicField" and k != "SchematicPin" for k in got),
                  f"cold, first placement: the part has Device:R's two pins and its body ({got})")
            check(symbol.definition.value_field.text.text == "470R"
                  and symbol.definition.footprint_field.text.text == "Resistor_SMD:R_1206_3216Metric",
                  "the part's own value and footprint")
            check(fields(symbol).get("MPN") == "RC1206FR-07470RL", f"the part's own fields ({fields(symbol)})")

        # 2. a part whose symbol cannot be had: an error, not a symbol of fields alone
        broken, error = place(BROKEN, 76200000)
        check(broken is None, f"a part whose symbol is missing is not placed as fields alone ({kinds(broken) if broken else ''})")
        check(broken is not None or ("not found" in error and BROKEN in error),
              f"the API answers with a clear error naming the part ({error})")

        # 3. the same session: the first part again
        again, error = place(PART, 101600000)
        check(again is not None and kinds(again).count("SchematicPin") == 2,
              f"placed again in the same session it is still whole ({error or kinds(again)})")
        connection.close()
    finally:
        stub.shutdown()
        if server is not None and server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=20)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=20)
        if FAILURES:
            print(f"server log: {root / 'server.log'}")
        else:
            shutil.rmtree(root, ignore_errors=True)
    print(f"\n{len(FAILURES)} failure(s)")
    if not FAILURES:
        print("ALL PASSED")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())

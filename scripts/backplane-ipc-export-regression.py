#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise every PCB and schematic IPC export command on disposable fixtures."""

import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET
import zipfile

from backplane_ipc_test_support import IpcSession


def validate_output(path):
    data = path.read_bytes()
    assert data, f"Empty output: {path}"
    suffix = path.suffix.lower()
    if suffix == ".pdf":
        assert data.startswith(b"%PDF-"), path
    elif suffix in (".svg", ".xml"):
        ET.fromstring(data)
    elif suffix == ".glb":
        assert data[:4] == b"glTF", path
        assert int.from_bytes(data[8:12], "little") == len(data), path
    elif suffix == ".png":
        assert data.startswith(b"\x89PNG\r\n\x1a\n"), path
    elif suffix == ".json":
        json.loads(data)
    elif suffix in (".step", ".stp"):
        assert b"ISO-10303-21" in data, path
    elif suffix == ".zip":
        with zipfile.ZipFile(path) as archive:
            assert archive.namelist() and archive.testzip() is None, path
    elif suffix == ".dsn":
        assert data.lstrip().startswith(b"(pcb"), path
        for section in (b"(structure", b"(placement", b"(library", b"(network"):
            assert section in data, (path, section)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    args = parser.parse_args()
    with IpcSession(args.kicad_cli) as api:
        types = api.module("common.types.base_types")
        enums = api.module("common.types.enums")
        jobs = api.module("common.types.jobs")
        project = api.module("common.commands.project_commands")
        sch = api.module("schematic.schematic_jobs")
        pcb = api.module("board.board_jobs")
        board = api.module("board.board_types")
        design = api.copy_project("demos/ecc83", "export-project")
        documents = {}
        for kind, extension in ((types.DOCTYPE_SCHEMATIC, "kicad_sch"),
                                (types.DOCTYPE_PCB, "kicad_pcb")):
            path = design / f"ecc83-pp.{extension}"
            if not path.exists():
                path = next(design.glob(f"*.{extension}"))
            documents[kind] = api.request(
                project.OpenDocument(type=kind, path=str(path)),
                project.OpenDocumentResponse,
            ).document

        cases = []
        for format_name, extension in (("Svg", "svg"), ("Dxf", "dxf"),
                                        ("Pdf", "pdf"), ("Ps", "ps")):
            command = getattr(sch, f"RunSchematicJobExport{format_name}")()
            command.plot_settings.plot_all = True
            command.plot_settings.black_and_white = True
            cases.append((types.DOCTYPE_SCHEMATIC, command, extension))
        cases.extend([
            (types.DOCTYPE_SCHEMATIC, sch.RunSchematicJobExportNetlist(
                format=sch.SNF_KICAD_XML), "xml"),
            (types.DOCTYPE_SCHEMATIC, sch.RunSchematicJobExportPng(dpi=150), "directory"),
            # Explicit fields: upstream fe761fb867's "Grouped By Value" field preset throws
            # std::bad_alloc in the BOM job (kicad-cli and IPC alike).
            (types.DOCTYPE_SCHEMATIC, sch.RunSchematicJobExportBOM(
                format=sch.BOMFormatSettings(preset_name="CSV"),
                fields=sch.BOMFieldSettings(fields=[
                    sch.BOMField(name="Reference", label="Reference"),
                    sch.BOMField(name="Value", label="Value", group_by=True)])), "csv"),
        ])
        plot = pcb.BoardPlotSettings(
            layers=[board.BL_F_Cu, board.BL_Edge_Cuts], scale=1.0,
            black_and_white=True, plot_footprint_values=True,
            plot_reference_designators=True,
        )
        for format_name, extension in (("Svg", "svg"), ("Dxf", "dxf"),
                                        ("Pdf", "pdf"), ("Ps", "ps")):
            command = getattr(pcb, f"RunBoardJobExport{format_name}")(
                plot_settings=plot, page_mode=pcb.BJPM_ALL_LAYERS_ONE_PAGE)
            if format_name == "Svg":
                command.precision = 6
            if format_name == "Dxf":
                command.units = enums.U_MM
            cases.append((types.DOCTYPE_PCB, command, extension))
        for format_value, extension in ((pcb.B3D_STEP, "step"), (pcb.B3D_GLB, "glb")):
            cases.append((types.DOCTYPE_PCB, pcb.RunBoardJobExport3D(
                format=format_value, overwrite=True, board_only=True,
                export_board_body=True, board_outlines_chaining_epsilon=0.01), extension))
        cases.extend([
            (types.DOCTYPE_PCB, pcb.RunBoardJobExportRender(
                format=pcb.RF_PNG, quality=pcb.RQ_BASIC, width=256, height=256,
                side=pcb.RS_TOP, zoom=1.0,
                background_style=pcb.RBS_OPAQUE), "png"),
            (types.DOCTYPE_PCB, pcb.RunBoardJobExportGerbers(
                plot_settings=pcb.BoardPlotSettings(
                    layers=[board.BL_F_Cu, board.BL_B_Cu, board.BL_Edge_Cuts])), "directory"),
            (types.DOCTYPE_PCB, pcb.RunBoardJobExportDrill(
                format=pcb.DF_EXCELLON, units=enums.U_MM,
                origin=pcb.DO_ABSOLUTE, zeros_format=pcb.DZF_DECIMAL), "directory"),
            (types.DOCTYPE_PCB, pcb.RunBoardJobExportPosition(
                format=pcb.PF_CSV, units=enums.U_MM, single_file=True,
                side=pcb.PS_BOTH), "csv"),
            (types.DOCTYPE_PCB, pcb.RunBoardJobExportGencad(), "cad"),
            (types.DOCTYPE_PCB, pcb.RunBoardJobExportSpecctraDsn(), "dsn"),
            (types.DOCTYPE_PCB, pcb.RunBoardJobExportIdf(units=enums.U_MM), "emn"),
            (types.DOCTYPE_PCB, pcb.RunBoardJobExportPng(
                plot_settings=pcb.BoardPlotSettings(layers=[board.BL_F_Cu, board.BL_Edge_Cuts]),
                dpi=150), "directory"),
            (types.DOCTYPE_PCB, pcb.RunBoardJobExportIpc2581(
                version=pcb.IPC2581V_C, units=enums.U_MM, precision=6), "xml"),
            (types.DOCTYPE_PCB, pcb.RunBoardJobExportIpcD356(), "d356"),
            (types.DOCTYPE_PCB, pcb.RunBoardJobExportODB(
                units=enums.U_MM, precision=6, compression=pcb.ODBC_ZIP), "zip"),
            (types.DOCTYPE_PCB, pcb.RunBoardJobExportStats(
                format=pcb.SOF_JSON, units=enums.U_MM), "json"),
        ])
        tested = set()
        for index, (kind, command, extension) in enumerate(cases):
            name = command.DESCRIPTOR.name
            destination = api.root / "outputs" / f"{index}-{name}"
            destination.mkdir(parents=True)
            output = destination if extension == "directory" else destination / f"result.{extension}"
            command.job_settings.document.CopyFrom(documents[kind])
            command.job_settings.output_path = str(output)
            result = api.request(command, jobs.RunJobResponse)
            assert result.status == jobs.JS_SUCCESS, f"{name}: {result}"
            generated = [path for path in destination.rglob("*") if path.is_file()]
            assert generated, f"{name} succeeded without producing files: {result}"
            for path in generated:
                validate_output(path)
            tested.add(command.DESCRIPTOR.full_name)
            print(f"PASS {name} ({extension}): {len(generated)} files", flush=True)

        # Fail when a new job is added without an explicit test case.
        expected = {descriptor.full_name for module in (sch, pcb)
                    for descriptor in module.DESCRIPTOR.message_types_by_name.values()
                    if descriptor.name.startswith(("RunBoardJob", "RunSchematicJob"))}
        assert tested == expected, f"Untested exports: {sorted(expected - tested)}"


if __name__ == "__main__":
    main()

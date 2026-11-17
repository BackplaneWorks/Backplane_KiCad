# SPDX-License-Identifier: GPL-3.0-or-later
"""Small generated schematics for the simulation checks (kiapi.backplane.sim).

A circuit is a list of parts, each with the net of every pin.  Each part is a generic symbol
with its pins in a row; a label on each pin gives it its net, so nothing depends on a symbol
library or on wires.  A net called GND is ngspice's node 0.
"""

import json
from pathlib import Path
import uuid

ROOT_UUID = "5b0d3a1e-0000-4000-8000-000000000001"


def _uuid(seed):
    return str(uuid.uuid5(uuid.UUID(ROOT_UUID), seed))


def _lib_symbol(prefix, pins):
    name = f"simchk:{prefix}{len(pins)}"
    pin_text = "".join(
        f'''
        (pin passive line (at {2.54 * index:.2f} 0 90) (length 1.27)
          (name "{pin_name}" (effects (font (size 1.27 1.27))))
          (number "{index + 1}" (effects (font (size 1.27 1.27))))
        )''' for index, pin_name in enumerate(pins))
    return name, f'''
    (symbol "{name}" (in_bom yes) (on_board yes)
      (property "Reference" "{prefix}" (at 0 2.54 0) (effects (font (size 1.27 1.27))))
      (property "Value" "{prefix}" (at 0 5.08 0) (effects (font (size 1.27 1.27))))
      (property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "{prefix}{len(pins)}_1_1"{pin_text}
      )
    )'''


class Part:
    def __init__(self, ref, value, nets, fields=None, pins=None, exclude=False):
        self.ref = ref
        self.value = value
        self.nets = list(nets)
        self.fields = dict(fields or {})
        self.pins = list(pins or [str(index + 1) for index in range(len(self.nets))])
        self.exclude = exclude

    @property
    def prefix(self):
        return "".join(ch for ch in self.ref if not ch.isdigit())


def write_project(folder, name, parts, texts=()):
    """Write <folder>/<name>.kicad_sch and .kicad_pro; returns the schematic's path."""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    libs = {}
    body = []
    for row, part in enumerate(parts):
        lib_name, lib_text = _lib_symbol(part.prefix, part.pins)
        libs[lib_name] = lib_text
        x = 50.8
        y = 25.4 + 12.7 * row
        properties = [("Reference", part.ref), ("Value", part.value), ("Footprint", ""),
                      ("Datasheet", ""), *part.fields.items()]
        property_text = "".join(
            f'''
    (property "{key}" "{str(value).replace(chr(34), chr(92) + chr(34))}" (at {x:.2f} {y - 2.54:.2f} 0)
      (effects (font (size 1.27 1.27)){"" if key in ("Reference", "Value") else " hide"})
    )''' for key, value in properties)
        pin_text = "".join(
            f'\n    (pin "{index + 1}" (uuid {_uuid(f"{name}/{part.ref}/pin{index}")}))'
            for index in range(len(part.nets)))
        body.append(f'''
  (symbol (lib_id "{lib_name}") (at {x:.2f} {y:.2f} 0) (unit 1)
    (exclude_from_sim {"yes" if part.exclude else "no"}) (in_bom yes) (on_board yes) (dnp no)
    (uuid {_uuid(f"{name}/{part.ref}")}){property_text}{pin_text}
    (instances (project "{name}" (path "/{ROOT_UUID}" (reference "{part.ref}") (unit 1))))
  )''')
        for index, net in enumerate(part.nets):
            if net is None:
                continue
            body.append(f'''
  (label "{net}" (at {x + 2.54 * index:.2f} {y:.2f} 90)
    (effects (font (size 1.27 1.27)) (justify left bottom))
    (uuid {_uuid(f"{name}/{part.ref}/label{index}")})
  )''')
    for index, text in enumerate(texts):
        escaped = text.replace("\n", "\\n").replace('"', '\\"')
        body.append(f'''
  (text "{escaped}" (at 25.4 {150 + 10 * index:.2f} 0)
    (effects (font (size 1.27 1.27)) (justify left bottom))
    (uuid {_uuid(f"{name}/text{index}")})
  )''')
    schematic = folder / f"{name}.kicad_sch"
    schematic.write_text(f'''(kicad_sch (version 20231120) (generator "backplane_sim_check")
  (uuid {ROOT_UUID})
  (paper "A4")
  (lib_symbols{"".join(libs.values())}
  )
{"".join(body)}
  (sheet_instances (path "/" (page "1")))
)
''')
    (folder / f"{name}.kicad_pro").write_text(json.dumps(
        {"meta": {"filename": f"{name}.kicad_pro", "version": 1}}, indent=2) + "\n")
    return schematic

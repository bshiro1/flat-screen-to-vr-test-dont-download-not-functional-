#!/usr/bin/env python3
"""Offline tests for the Outlast authorized-symbol ingestion pipeline."""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ingest_outlast_symbols import ingest  # noqa: E402
from verify_outlast_pdb import MSF7_MAGIC, pe_codeview, verify  # noqa: E402

EXE = Path(
    r"C:\Program Files (x86)\Steam\steamapps\common\The Outlast Trials\OPP\Binaries\Win64\TOTClient-Win64-Shipping.exe"
)
STATIC = Path(__file__).resolve().parent.parent / "outlast_verb_map.json"
SHA = hashlib.sha256(EXE.read_bytes()).hexdigest().casefold() if EXE.exists() else ""


def make_static(path: Path) -> None:
    path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "status": "static-unvalidated",
                "dispatch_allowed": False,
                "build": {"exe": EXE.name, "sha256": SHA},
                "process_event": {"rva": "0x1A2C750", "vtable_index": "0x44"},
                "candidates": [
                    {
                        "role": "generic_interact_start",
                        "tier": "tier1",
                        "class": "URBInteractFocusable",
                        "function": "StartInteraction",
                    }
                ],
            },
            indent=2,
        ),
        encoding="utf-8",
    )


def make_symbols(path: Path) -> None:
    path.write_text(
        "\n".join(
            [
                "0x01A2C750 ?ProcessEvent@UObject@@QEAAXPEAVUFunction@@PEAX@Z",
                "0x01481663 Z_Construct_UFunction_URBInteractFocusable_StartInteraction",
                "matches=2",
            ]
        )
        + "\n",
        encoding="utf-8",
    )


def make_reflection(path: Path, *, with_params: bool = True) -> None:
    func = {
        "name": "StartInteraction",
        "flags": "0x00000400",
        "fname_index": 12345,
        "rva": "0x01A2C750",
        "params_size": 8 if with_params else 0,
    }
    if with_params:
        func["parameters"] = [
            {"name": "OtherPlayer", "type": "URBPlayer*", "offset": 0, "size": 8}
        ]
    path.write_text(
        json.dumps(
            {
                "build": {"exe_sha256": SHA},
                "process_event": {"rva": "0x1A2C750", "vtable_index": "0x44"},
                "globals": {"GUObjectArray": "0x12345678"},
                "offsets": {"UObject::NamePrivate": "0x18"},
                "classes": {"URBInteractFocusable": {"functions": [func]}},
            },
            indent=2,
        ),
        encoding="utf-8",
    )


class IngestTests(unittest.TestCase):
    @unittest.skipUnless(EXE.exists(), "requires installed target EXE")
    def test_reflection_validates_and_enables_dispatch_only_with_opt_in(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            static = root / "static.json"
            symbols = root / "symbols.txt"
            reflection = root / "reflection.json"
            out = root / "out.json"
            make_static(static)
            make_symbols(symbols)
            make_reflection(reflection)

            args = argparse.Namespace(
                exe=EXE,
                static_map=static,
                out=out,
                symbols=symbols,
                pdb=None,
                reflection=reflection,
                allow_dispatch=False,
                assume_build_match=False,
                required_roles="generic_interact_start",
            )
            result = ingest(args)
            self.assertEqual(result["status"], "reflection-validated")
            self.assertFalse(result["dispatch_allowed"])
            self.assertTrue(result["candidates"][0]["validated"])
            self.assertEqual(
                result["candidates"][0]["reflection"]["reflected_class"],
                "URBInteractFocusable",
            )

            args.allow_dispatch = True
            result = ingest(args)
            self.assertTrue(result["dispatch_allowed"])
            self.assertNotIn(
                "UFunction FunctionFlags for every candidate.",
                result["unresolved_gaps"],
            )
            self.assertIn(
                "Runtime dispatch opt-ins are implemented but not exercised in a solo/private session.",
                result["unresolved_gaps"],
            )
            self.assertEqual(
                result["validation"]["globals"]["GUObjectArray"], "0x12345678"
            )
            self.assertEqual(
                result["validation"]["offsets"]["UObject::NamePrivate"], "0x18"
            )

    @unittest.skipUnless(EXE.exists(), "requires installed target EXE")
    def test_missing_parameter_layout_blocks_dispatch(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            static = root / "static.json"
            symbols = root / "symbols.txt"
            reflection = root / "reflection.json"
            out = root / "out.json"
            make_static(static)
            make_symbols(symbols)
            make_reflection(reflection, with_params=False)

            args = argparse.Namespace(
                exe=EXE,
                static_map=static,
                out=out,
                symbols=symbols,
                pdb=None,
                reflection=reflection,
                allow_dispatch=True,
                assume_build_match=False,
                required_roles="generic_interact_start",
            )
            result = ingest(args)
            self.assertFalse(result["dispatch_allowed"])
            self.assertIn(
                "generic_interact_start", result["validation"]["missing_required_roles"]
            )

    @unittest.skipUnless(EXE.exists(), "requires installed target EXE")
    def test_pdb_guid_and_age_verification(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            pdb = Path(tmp) / "synthetic.pdb"
            cv = pe_codeview(EXE)
            block_size = 512
            info = struct.pack(
                "<III", 20000404, 0x6AE8F846, cv["age"]
            ) + bytes.fromhex(cv["guid_bytes"])
            directory = struct.pack("<II", 2, 0) + struct.pack("<I", len(info))
            directory += struct.pack("<I", 1)  # stream 1 lives in block 1

            blocks = [bytearray(block_size) for _ in range(4)]
            blocks[0][: len(MSF7_MAGIC)] = MSF7_MAGIC
            struct.pack_into("<IIIIII", blocks[0], 32, block_size, 1, 4, len(directory), 0, 3)
            blocks[1][: len(info)] = info
            blocks[2][: len(directory)] = directory
            struct.pack_into("<I", blocks[3], 0, 2)
            pdb.write_bytes(b"".join(bytes(b) for b in blocks))

            result = verify(EXE, pdb)
            self.assertTrue(result["guid_match"])
            self.assertTrue(result["age_match"])
            self.assertTrue(result["match"])

    @unittest.skipUnless(EXE.exists(), "requires installed target EXE")
    def test_shipped_static_map_never_enables_dispatch(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out.json"
            args = argparse.Namespace(
                exe=EXE,
                static_map=STATIC,
                out=out,
                symbols=None,
                pdb=None,
                reflection=None,
                allow_dispatch=True,
                assume_build_match=False,
                required_roles="generic_interact_start",
            )
            result = ingest(args)
            self.assertEqual(result["status"], "static-unvalidated")
            self.assertFalse(result["dispatch_allowed"])


if __name__ == "__main__":
    unittest.main()

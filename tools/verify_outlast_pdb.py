#!/usr/bin/env python3
"""Verify that a supplied PDB matches the installed TOTClient executable.

This is offline and read-only. It parses the PE CodeView debug directory and the
PDB 7.0 MSF info stream, then compares the RSDS GUID + age. A match is required
before any symbol-derived address is trusted by the Outlast interaction mod.

Usage:
  python tools/verify_outlast_pdb.py --exe <TOTClient-Win64-Shipping.exe> --pdb <file.pdb>
  python tools/verify_outlast_pdb.py --exe <exe> --print-only
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path
from typing import Optional

MSF7_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"


def _u16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def _u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def _guid(data: bytes, off: int) -> str:
    d1, d2, d3 = struct.unpack_from("<IHH", data, off)
    tail = data[off + 8 : off + 16]
    return f"{d1:08x}-{d2:04x}-{d3:04x}-{tail[0]:02x}{tail[1]:02x}-{tail[2:].hex()}"


def pe_codeview(exe_path: Path) -> dict:
    data = exe_path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError("not a PE file (missing MZ)")
    pe = _u32(data, 0x3C)
    if data[pe : pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file (missing PE signature)")

    coff = pe + 4
    opt = coff + 20
    magic = _u16(data, opt)
    if magic == 0x10B:  # PE32
        dd = opt + 96
    elif magic == 0x20B:  # PE32+
        dd = opt + 112
    else:
        raise ValueError(f"unknown optional-header magic 0x{magic:X}")

    debug_rva, debug_size = struct.unpack_from("<II", data, dd + 6 * 8)
    if not debug_rva or not debug_size:
        raise ValueError("PE has no debug directory")

    # Locate the RVAs in raw sections. This target has a straightforward
    # User-Size layout, so no alignment adjustment is needed.
    sections = []
    sec_off = opt + _u16(data, coff + 16)
    for i in range(_u16(data, coff + 2)):
        s = sec_off + 40 * i
        name = data[s : s + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, s + 8)
        sections.append((vaddr, vsize, rawptr, rawsize, name))

    def rva_to_raw(rva: int) -> int:
        for vaddr, vsize, rawptr, rawsize, _ in sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return rawptr + (rva - vaddr)
        raise ValueError(f"RVA 0x{rva:X} is not in a section")

    entries = []
    raw = rva_to_raw(debug_rva)
    for off in range(debug_rva, debug_rva + debug_size, 28):
        ptr = rva_to_raw(off)
        _characteristics, _stamp, _maj, _min, kind, size, _addr, data_ptr = struct.unpack_from(
            "<IIHHIIII", data, ptr
        )
        if kind != 2 or size < 24:  # IMAGE_DEBUG_TYPE_CODEVIEW
            continue
        cv = data_ptr  # IMAGE_DEBUG_DIRECTORY::PointerToRawData is a file offset.
        if cv + size > len(data) or data[cv : cv + 4] != b"RSDS":
            raise ValueError("CodeView entry is not PDB 7.0 (RSDS)")
        guid = _guid(data, cv + 4)
        age = _u32(data, cv + 20)
        raw_guid = data[cv + 4 : cv + 20]
        name_end = data.find(b"\0", cv + 24, cv + size)
        pdb_path = data[cv + 24 : name_end if name_end >= 0 else cv + size]
        entries.append(
            {
                "guid": guid,
                "age": age,
                "guid_bytes": raw_guid.hex(),
                "pdb_path": pdb_path.decode("utf-8", "replace"),
            }
        )

    if not entries:
        raise ValueError("PE has no CodeView/PDB 7.0 debug entry")
    return entries[0]


def pdb_info(pdb_path: Path) -> dict:
    data = pdb_path.read_bytes()
    if len(data) < 56 or data[:32] != MSF7_MAGIC:
        raise ValueError("not an MSF 7.00 PDB")

    block_size = _u32(data, 32)
    num_blocks = _u32(data, 40)
    directory_size = _u32(data, 44)
    block_map_addr = _u32(data, 52)
    if block_size < 512 or block_size > (1 << 20):
        raise ValueError(f"implausible PDB block size {block_size}")
    if block_map_addr >= num_blocks:
        raise ValueError("PDB block map is out of range")

    map_bytes = block_size * (((directory_size + block_size - 1) // block_size + 7) // 8)
    map_raw = data[block_map_addr * block_size : block_map_addr * block_size + map_bytes]
    directory_blocks = [
        struct.unpack_from("<I", map_raw, i)[0]
        for i in range(0, min(len(map_raw), ((directory_size + block_size - 1) // block_size) * 4), 4)
    ]

    directory = bytearray()
    for block in directory_blocks:
        directory += data[block * block_size : (block + 1) * block_size]
    directory = bytes(directory[:directory_size])

    stream_count = _u32(directory, 0)
    sizes = [_u32(directory, 4 + 4 * i) for i in range(stream_count)]
    if len(sizes) <= 1:
        raise ValueError("PDB has no stream 1 (PDB info)")

    cursor = 4 + 4 * stream_count

    def stream_blocks(index: int) -> list[int]:
        nonlocal cursor
        for i in range(index + 1):
            count = (sizes[i] + block_size - 1) // block_size
            blocks = list(struct.unpack_from(f"<{count}I", directory, cursor)) if count else []
            cursor += 4 * count
            if i == index:
                return blocks
        return []

    info_blocks = stream_blocks(1)
    info = bytearray()
    for block in info_blocks:
        info += data[block * block_size : (block + 1) * block_size]
    info = bytes(info[: sizes[1]])
    if len(info) < 28:
        raise ValueError("PDB info stream is truncated")

    version = _u32(info, 0)
    signature = _u32(info, 4)
    age = _u32(info, 8)
    raw_guid = info[12:28]
    return {
        "version": version,
        "signature": signature,
        "age": age,
        "guid": _guid(info, 12),
        "guid_bytes": raw_guid.hex(),
    }


def verify(exe: Path, pdb: Path) -> dict:
    codeview = pe_codeview(exe)
    info = pdb_info(pdb)
    guid_match = codeview["guid_bytes"] == info["guid_bytes"]
    age_match = codeview["age"] == info["age"]
    return {
        "exe": str(exe),
        "pdb": str(pdb),
        "exe_guid": codeview["guid"],
        "exe_age": codeview["age"],
        "pdb_guid": info["guid"],
        "pdb_age": info["age"],
        "pdb_path_recorded_in_exe": codeview["pdb_path"],
        "guid_match": guid_match,
        "age_match": age_match,
        "match": bool(guid_match and age_match),
    }


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--pdb", type=Path)
    parser.add_argument("--print-only", action="store_true")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)

    try:
        codeview = pe_codeview(args.exe)
        result = {
            "exe": str(args.exe),
            "exe_guid": codeview["guid"],
            "exe_age": codeview["age"],
            "pdb_path_recorded_in_exe": codeview["pdb_path"],
        }
        if args.pdb and not args.print_only:
            result.update(verify(args.exe, args.pdb))
    except Exception as exc:  # noqa: BLE001 - CLI diagnostics
        print(f"error: {exc}", file=sys.stderr)
        return 2

    text = json.dumps(result, indent=2)
    print(text)
    if args.json:
        args.json.write_text(text + "\n", encoding="utf-8")
    if args.pdb and not args.print_only and not result.get("match"):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

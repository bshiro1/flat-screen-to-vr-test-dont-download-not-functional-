#!/usr/bin/env python3
"""Build a build-fresh reflection JSON from a Dumper-7 SDK dump.

This tool is offline. It reads only downloaded SDK text and the installed EXE.
It refuses to emit a map unless the EXE hash and three SDK global qwords match.
The resulting JSON is suitable for tools/ingest_outlast_symbols.py --reflection.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
from typing import Any

CLASS_ALIASES = {
    # Static FName registration put these verbs near URBInteractFocusable, but the
    # generated SDK binds Start/End/IsInteractionPossible to the RBInteractable
    # interface that the focusable object implements.
    "URBInteractFocusable": "RBInteractable",
}

EXPECTED_QWORDS = {
    "GObjects": 0x4FB53D704FB5390,
    "GNames": 0x4523F6006A748D0,
    "GWorld": 0xA758A750A748A740,
}

TOKEN_ALIASES = {
    "reliable": "netreliable",
    "server": "netserver",
    "client": "netclient",
    "multicast": "netmulticast",
    "authorityonly": "blueprintauthorityonly",
    "cosmetic": "blueprintcosmetic",
    "callable": "blueprintcallable",
    "event": "event",
    "pure": "blueprintpure",
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_pe(data: bytes) -> tuple[int, list[dict[str, Any]]]:
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off : pe_off + 4] != b"PE\0\0":
        raise ValueError("invalid PE signature")
    coff = pe_off + 4
    machine, num_sections = struct.unpack_from("<HH", data, coff)
    if machine != 0x8664:
        raise ValueError(f"expected x64 PE, got machine 0x{machine:X}")
    optional_size = struct.unpack_from("<H", data, coff + 16)[0]
    optional = coff + 20
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic != 0x20B:
        raise ValueError(f"expected PE32+, got magic 0x{magic:X}")
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    sections: list[dict[str, Any]] = []
    sec = optional + optional_size
    for _ in range(num_sections):
        name = data[sec : sec + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, sec + 8)
        sections.append(
            {"name": name, "vsize": vsize, "vaddr": vaddr, "rawsize": rawsize, "rawptr": rawptr}
        )
        sec += 40
    return image_base, sections


def qword_at_rva(data: bytes, sections: list[dict[str, Any]], rva: int) -> int:
    for section in sections:
        start = section["vaddr"]
        span = max(section["vsize"], section["rawsize"])
        if start <= rva < start + span:
            offset = section["rawptr"] + (rva - start)
            return struct.unpack_from("<Q", data, offset)[0]
    raise ValueError(f"RVA 0x{rva:X} is not in any PE section")


def parse_offsets(basic_text: str) -> dict[str, int]:
    match = re.search(r"namespace\s+Offsets\s*\{(.*?)\n\}", basic_text, re.S)
    if not match:
        raise ValueError("Offsets namespace not found in Basic.hpp")
    result: dict[str, int] = {}
    for name, value in re.findall(
        r"constexpr\s+int32\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+)", match.group(1)
    ):
        result[name] = int(value, 16)
    return result


def parse_function_flag_enum(basic_text: str) -> dict[str, int]:
    match = re.search(r"enum\s+class\s+EFunctionFlags\s*:\s*uint32\s*\{(.*?)\n\};", basic_text, re.S)
    if not match:
        raise ValueError("EFunctionFlags enum not found in Basic.hpp")
    result: dict[str, int] = {}
    for name, value in re.findall(r"^\s*(\w+)\s*=\s*(0x[0-9A-Fa-f]+)", match.group(1), re.M):
        if name == "AllFlags":
            continue
        result[name.casefold()] = int(value, 16)
    return result


def sdk_class_for(static_class: str) -> str:
    if static_class in CLASS_ALIASES:
        return CLASS_ALIASES[static_class]
    for prefix in ("U", "A"):
        if static_class.startswith(prefix) and len(static_class) > len(prefix):
            return static_class[len(prefix) :]
    return static_class


def function_flags(
    functions_text: str, sdk_class: str, function: str, enum_map: dict[str, int]
) -> tuple[int, list[str]]:
    needle = f"// Function OPP.{sdk_class}.{function}\n"
    idx = functions_text.find(needle)
    if idx < 0:
        raise ValueError(f"function comment not found: OPP.{sdk_class}.{function}")
    window = functions_text[idx : idx + len(needle) + 600]
    match = re.search(r"^//\s*\(([^\n]*)\)\s*$", window, re.M)
    if not match:
        raise ValueError(f"function flags not found: OPP.{sdk_class}.{function}")
    tokens = [token.strip() for token in match.group(1).split(",") if token.strip()]
    value = 0
    unknown: list[str] = []
    for token in tokens:
        key = token.casefold()
        key = TOKEN_ALIASES.get(key, key)
        bit = enum_map.get(key)
        if bit is None:
            unknown.append(token)
        else:
            value |= bit
    return value, unknown


def parameter_struct(
    params_text: str, functions_text: str, sdk_class: str, function: str
) -> tuple[int, list[dict[str, Any]]]:
    needle = f"// Function OPP.{sdk_class}.{function}\n"
    idx = params_text.find(needle)
    if idx < 0:
        # A genuinely parameterless UFunction has no Params:: struct. Confirm this
        # from the generated body before accepting an empty layout.
        fidx = functions_text.find(needle)
        body = ""
        if fidx >= 0:
            brace = functions_text.find("{", fidx)
            close = functions_text.find("\n}", brace) if brace >= 0 else -1
            if close >= 0:
                body = functions_text[brace:close]
        if "ProcessEvent(Func, nullptr);" in body and "Params::" not in body:
            return 0, []
        raise ValueError(f"parameter comment not found: OPP.{sdk_class}.{function}")
    segment = params_text[idx : idx + 12000]
    size_match = re.search(r"^//\s*(0x[0-9A-Fa-f]+)\s*\(", segment, re.M)
    struct_match = re.search(r"^struct\s+(\w+)\s+final\s*\{", segment, re.M)
    if not size_match or not struct_match:
        raise ValueError(f"parameter struct not found: OPP.{sdk_class}.{function}")
    brace = segment.find("{", struct_match.start())
    end = segment.find("\n};", brace)
    if end < 0:
        raise ValueError(f"unterminated parameter struct: OPP.{sdk_class}.{function}")
    body = segment[brace + 1 : end]

    params: list[dict[str, Any]] = []
    for line in body.splitlines():
        meta = re.search(r"//\s*(0x[0-9A-Fa-f]+)\((0x[0-9A-Fa-f]+)\)\(([^)]*)\)", line)
        if not meta:
            continue
        properties = [part.strip() for part in meta.group(3).split(",")]
        if "Parm" not in properties:
            continue
        declaration = line.split("//", 1)[0].strip().rstrip(";").strip()
        decl_match = re.match(r"^(.*?)\s+([A-Za-z_]\w*)$", declaration)
        if decl_match:
            cpp_type, name = decl_match.group(1).strip(), decl_match.group(2)
        else:
            cpp_type, name = declaration, ""
        if name.startswith("Pad_"):
            continue
        params.append(
            {
                "name": name,
                "type": cpp_type,
                "offset": int(meta.group(1), 16),
                "size": int(meta.group(2), 16),
                "properties": properties,
            }
        )
    return int(size_match.group(1), 16), params


def build(args: argparse.Namespace) -> dict[str, Any]:
    exe_data = args.exe.read_bytes()
    actual_sha = hashlib.sha256(exe_data).hexdigest()
    static = json.loads(args.static_map.read_text(encoding="utf-8"))
    expected_sha = str(static["build"]["sha256"]).casefold()
    if actual_sha != expected_sha:
        raise ValueError(f"EXE hash mismatch: {actual_sha} != {expected_sha}")

    image_base, sections = parse_pe(exe_data)
    if image_base != int(str(static["build"]["image_base"]), 16):
        raise ValueError(f"image base mismatch: 0x{image_base:X}")

    basic_text = (args.sdk / "Basic.hpp").read_text(encoding="utf-8", errors="replace")
    functions_text = (args.sdk / "OPP_functions.cpp").read_text(encoding="utf-8", errors="replace")
    params_text = (args.sdk / "OPP_parameters.hpp").read_text(encoding="utf-8", errors="replace")
    offsets = parse_offsets(basic_text)
    flag_enum = parse_function_flag_enum(basic_text)

    actual_qwords: dict[str, int] = {}
    for name, expected in EXPECTED_QWORDS.items():
        rva = offsets[name]
        actual = qword_at_rva(exe_data, sections, rva)
        if actual != expected:
            raise ValueError(
                f"{name} qword mismatch at RVA 0x{rva:X}: 0x{actual:X} != 0x{expected:X}"
            )
        actual_qwords[name] = actual

    scan = json.loads(args.scan.read_text(encoding="utf-8"))
    derived = scan["static_symbols"]
    if int(derived["process_event_rva"]) != int(str(static["process_event"]["rva"]), 16):
        raise ValueError("scanner ProcessEvent RVA disagrees with static map")
    if int(derived["process_event_index"]) != int(str(static["process_event"]["vtable_index"]), 16):
        raise ValueError("scanner ProcessEvent vtable index disagrees with static map")

    classes: dict[str, dict[str, list[dict[str, Any]]]] = {}
    missing: list[str] = []
    for candidate in static["candidates"]:
        static_class = str(candidate["class"])
        function = str(candidate["function"])
        sdk_class = sdk_class_for(static_class)
        try:
            flags, unknown = function_flags(functions_text, sdk_class, function, flag_enum)
            params_size, params = parameter_struct(params_text, functions_text, sdk_class, function)
        except ValueError as exc:
            missing.append(f"{static_class}.{function}: {exc}")
            continue
        if unknown:
            missing.append(f"{static_class}.{function}: unknown flag tokens {unknown}")
            continue
        classes.setdefault(static_class, {}).setdefault("functions", []).append(
            {
                "name": function,
                "role": candidate.get("role"),
                "tier": candidate.get("tier"),
                "reflected_class": sdk_class,
                "flags": f"0x{flags:08X}",
                "name_rva": candidate.get("name_rva"),
                "emit_rva": candidate.get("emit_rva"),
                "fname_index": None,
                "rva": None,
                "params_size": params_size,
                "parameters": params,
            }
        )

    doc: dict[str, Any] = {
        "_provenance": {
            "method": "offline parse of build-fresh Dumper-7 SDK text plus exact installed EXE",
            "sdk_repository": args.sdk_repo,
            "sdk_commit": args.sdk_commit,
            "sdk_files": [
                "Basic.hpp",
                "OPP_functions.cpp",
                "OPP_parameters.hpp",
            ],
            "build_fingerprint": {
                "exe_size": len(exe_data),
                "exe_sha256": actual_sha,
                "global_qwords": {k: f"0x{v:016X}" for k, v in actual_qwords.items()},
            },
            "note": (
                "Only reflection data was used. No anti-cheat bypass, injector, or online "
                "material from the source repository was downloaded or used."
            ),
        },
        "build": {
            "exe": Path(str(args.exe)).name,
            "exe_sha256": actual_sha,
            "image_base": f"0x{image_base:X}",
            "codeview_guid": "f9f4c1c0-2ec3-bcd6-4c4c-44205044422e",
            "codeview_age": 1,
        },
        "process_event": {
            "rva": static["process_event"]["rva"],
            "va": static["process_event"]["va"],
            "vtable_index": static["process_event"]["vtable_index"],
            "vtable_offset": static["process_event"]["vtable_offset"],
            "vtable_rva": f"0x{int(derived['uobject_vtable_rva']):X}",
            "function_size": f"0x{int(derived['process_event_size']):X}",
            "status": "build-fresh static derivation plus exact-build SDK fingerprint",
        },
        "globals": {
            "GUObjectArray": f"0x{image_base + offsets['GObjects']:X}",
            "GNames": f"0x{image_base + offsets['GNames']:X}",
            "GWorld": f"0x{image_base + offsets['GWorld']:X}",
            "FName::ToString": None,
        },
        "offsets": {
            "UObject::NamePrivate": "0x18",
            "UObject::Class": "0x10",
            "UObject::VTable": "0x0",
            "UField::Next": "0x28",
            "UStruct::Children": "0x48",
            "UFunction::FName": "0x18",
            "UFunction::FunctionFlags": "0xB0",
            "UFunction::NumParms": "0xB4",
            "UFunction::ParmsSize": "0xB6",
            "TUObjectArray::Objects": "0x0",
            "TUObjectArray::NumElements": "0x14",
            "TUObjectArray::NumChunks": "0x1C",
            "FUObjectItem::Object": "0x0",
            "FUObjectItem::Size": "0x18",
            "FNamePool::Blocks": "0x10",
            "FNameEntry::Name": "0x2",
        },
        "classes": classes,
    }
    if missing:
        doc["_provenance"]["unresolved"] = missing
    return doc


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--static-map", type=Path, required=True)
    parser.add_argument("--scan", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--sdk-repo", default="https://github.com/AIN-sch/outlast-trials-cheat"
    )
    parser.add_argument(
        "--sdk-commit", default="2dce12c20a95b84c0b44638ee5aa1f9d71fa747b"
    )
    args = parser.parse_args(argv)
    doc = build(args)
    args.out.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    unresolved = doc["_provenance"].get("unresolved", [])
    functions = sum(len(c["functions"]) for c in doc["classes"].values())
    print(f"wrote {args.out}")
    print(f"functions={functions} unresolved={len(unresolved)}")
    for item in unresolved:
        print(f"  - {item}")
    return 0 if not unresolved else 2


if __name__ == "__main__":
    raise SystemExit(main())

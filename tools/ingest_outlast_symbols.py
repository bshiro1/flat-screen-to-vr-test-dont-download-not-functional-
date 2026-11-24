#!/usr/bin/env python3
"""Convert an authorized matching PDB/reflection dump into a validated verb map.

The shipped outlast_verb_map.json is static evidence and has dispatch_allowed=false.
This tool consumes any combination of:

  --symbols   text from tools/vrc_pdb_dump.exe (RVA + mangled symbol)
  --pdb       matching TOTClient-Win64-Shipping.pdb (GUID/age verified against EXE)
  --reflection JSON dump with UFunction flags and parameter layouts

It writes a new map beside the DLL. Dispatch is enabled only when the exact EXE
hash matches, ProcessEvent is confirmed, and every required verb has a known
parameter layout. No game process is touched.

Example reflection JSON shape (extra keys are tolerated):
{
  "build": {"exe_sha256": "d62..."},
  "process_event": {"rva": "0x1A2C750", "vtable_index": "0x44"},
  "globals": {"GUObjectArray": "0x...", "GNames": "0x..."},
  "offsets": {"UObject::NamePrivate": "0x18", "UFunction::FunctionFlags": "0x90"},
  "classes": {
    "URBInteractFocusable": {
      "functions": [{
        "name": "StartInteraction",
        "flags": "0x00000400",
        "fname_index": 12345,
        "rva": "0x...",
        "params_size": 8,
        "parameters": [
          {"name": "Interactor", "type": "URBInteractFocusable*", "offset": 0, "size": 8}
        ]
      }]
    }
  }
}
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify_outlast_pdb import verify  # noqa: E402

SYMBOL_LINE = re.compile(r"^\s*0x([0-9a-fA-F]+)\s+(.+?)\s*$")
KNOWN_EXE_SHA = "d62df76ce8cb1c58391fbc8a1649945bc91178d27dfad2f98eaab5c907d6f27e"
DEFAULT_REQUIRED_ROLES = (
    "generic_interact_start,generic_interact_end,item_pickup,item_drop,door_operation_rpc"
)


def parse_int(value: Any) -> Optional[int]:
    if value is None:
        return None
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    text = str(value).strip()
    if not text:
        return None
    try:
        return int(text, 0)
    except ValueError:
        try:
            return int(text, 16)
        except ValueError:
            return None


def hex_str(value: Optional[int], width: int = 8) -> Optional[str]:
    return None if value is None else f"0x{value:0{width}X}"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def demangle(name: str) -> str:
    if not hasattr(ctypes, "windll"):
        return name
    try:
        dbghelp = ctypes.windll.dbghelp
        dbghelp.UnDecorateSymbolName.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_ulong,
            ctypes.c_ulong,
        ]
        dbghelp.UnDecorateSymbolName.restype = ctypes.c_ulong
        out = ctypes.create_string_buffer(4096)
        written = dbghelp.UnDecorateSymbolName(
            name.encode("utf-8", "replace"), out, len(out), 0
        )
        if written:
            return out.value.decode("utf-8", "replace")
    except Exception:  # noqa: BLE001 - demangling is cosmetic
        pass
    return name


def load_symbols(path: Optional[Path]) -> list[dict]:
    if not path:
        return []
    symbols: list[dict] = []
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = SYMBOL_LINE.match(raw)
        if not match:
            continue
        rva = int(match.group(1), 16)
        mangled = match.group(2)
        symbols.append(
            {"rva": hex_str(rva), "mangled": mangled, "demangled": demangle(mangled)}
        )
    return symbols


def symbol_matches(sym: dict, class_name: str, function: str) -> bool:
    demangled = sym["demangled"]
    mangled = sym["mangled"]
    qualified = f"{class_name}::{function}"
    if qualified.casefold() in demangled.casefold():
        return True
    if f"{function}@{class_name}" in mangled or (
        function.casefold() in mangled.casefold() and class_name.casefold() in mangled.casefold()
    ):
        return True
    construct = f"Z_Construct_UFunction_{class_name}_{function}"
    return construct.casefold() in mangled.casefold()


def find_process_event(symbols: Iterable[dict]) -> list[dict]:
    found = []
    for sym in symbols:
        text = f"{sym['mangled']} {sym['demangled']}".casefold()
        if "processevent@uobject" in text or "uobject::processevent" in text:
            found.append(sym)
    return found[:8]


def find_verb_symbols(
    symbols: Iterable[dict], class_name: str, function: str
) -> list[dict]:
    return [s for s in symbols if symbol_matches(s, class_name, function)][:8]


def normalize_functions(node: dict) -> Iterable[tuple[str, dict]]:
    functions = node.get("functions", node.get("Functions"))
    if functions is None:
        return []
    if isinstance(functions, dict):
        return list(functions.items())
    if isinstance(functions, list):
        out = []
        for item in functions:
            if isinstance(item, str):
                out.append((item, {}))
            elif isinstance(item, dict):
                name = item.get("name") or item.get("function") or item.get("function_name")
                if name:
                    out.append((str(name), item))
        return out
    return []


def load_reflection(path: Optional[Path]) -> dict:
    if not path:
        return {
            "build": {},
            "process_event": {},
            "globals": {},
            "offsets": {},
            "classes": {},
        }
    doc = json.loads(path.read_text(encoding="utf-8"))

    classes: dict[str, dict[str, dict]] = {}
    raw_classes = doc.get("classes", doc.get("Classes", {}))
    if isinstance(raw_classes, dict):
        iterable = raw_classes.items()
    elif isinstance(raw_classes, list):
        iterable = []
        for item in raw_classes:
            if isinstance(item, dict):
                name = item.get("class") or item.get("class_name") or item.get("name")
                if name:
                    iterable.append((str(name), item))
    else:
        iterable = []

    for class_name, class_node in iterable:
        if not isinstance(class_node, dict):
            class_node = {}
        for func_name, func_node in normalize_functions(class_node):
            classes.setdefault(class_name, {})[func_name] = func_node or {}

    # Also accept a flat function list.
    flat = doc.get("functions", [])
    if isinstance(flat, list):
        for item in flat:
            if not isinstance(item, dict):
                continue
            class_name = item.get("class") or item.get("class_name") or item.get("owner")
            func_name = item.get("name") or item.get("function") or item.get("function_name")
            if class_name and func_name:
                classes.setdefault(str(class_name), {})[str(func_name)] = item

    pe = doc.get("process_event", doc.get("ProcessEvent", {}))
    if not isinstance(pe, dict):
        pe = {}
    globals_node = doc.get("globals", doc.get("Globals", {}))
    if not isinstance(globals_node, dict):
        globals_node = {}
    offsets_node = doc.get("offsets", doc.get("Offsets", {}))
    if not isinstance(offsets_node, dict):
        offsets_node = {}

    return {
        "build": doc.get("build", doc.get("Build", {})),
        "process_event": pe,
        "globals": globals_node,
        "offsets": offsets_node,
        "classes": classes,
    }


def normalize_parameter(item: Any) -> Optional[dict]:
    if isinstance(item, str):
        return {"name": item, "type": "unknown", "offset": None, "size": None}
    if not isinstance(item, dict):
        return None
    return {
        "name": item.get("name") or item.get("Name"),
        "type": item.get("type") or item.get("Type") or "unknown",
        "offset": parse_int(item.get("offset", item.get("Offset"))),
        "size": parse_int(item.get("size", item.get("Size"))),
    }


def reflection_for(reflection: dict, class_name: str, function: str) -> Optional[dict]:
    func = reflection["classes"].get(class_name, {}).get(function)
    if func is None:
        return None

    raw_params = func.get("parameters", func.get("params", func.get("fields", func.get("Properties"))))
    params_present = raw_params is not None
    params = []
    if isinstance(raw_params, dict):
        params = [normalize_parameter(v) for v in raw_params.values()]
    elif isinstance(raw_params, list):
        params = [normalize_parameter(v) for v in raw_params]
    params = [p for p in params if p]

    flags = parse_int(func.get("flags", func.get("function_flags", func.get("FunctionFlags"))))
    fname_index = parse_int(func.get("fname_index", func.get("name_index", func.get("FNameIndex"))))
    rva = parse_int(func.get("rva", func.get("function_rva", func.get("address_rva"))))
    params_size = parse_int(
        func.get("params_size", func.get("parms_size", func.get("parmsSize", func.get("size"))))
    )

    return {
        "reflected_class": func.get("reflected_class", func.get("owner", class_name)),
        "flags": hex_str(flags),
        "fname_index": fname_index,
        "function_rva": hex_str(rva),
        "params_size": params_size,
        "parameters": params,
        "layout_known": bool(params_present and params_size is not None),
        "flags_known": flags is not None,
    }


def ingest(args: argparse.Namespace) -> dict:
    static = json.loads(args.static_map.read_text(encoding="utf-8"))
    actual_sha = sha256_file(args.exe).casefold()
    expected_sha = str(static.get("build", {}).get("sha256", KNOWN_EXE_SHA)).casefold()
    build_match = actual_sha == expected_sha == KNOWN_EXE_SHA

    symbols = load_symbols(args.symbols)
    process_symbols = find_process_event(symbols)
    pdb_result = None
    if args.pdb:
        try:
            pdb_result = verify(args.exe, args.pdb)
        except Exception as exc:  # noqa: BLE001 - report rather than crash
            pdb_result = {"match": False, "error": str(exc)}

    reflection = load_reflection(args.reflection)
    reflection_sha = None
    if isinstance(reflection.get("build"), dict):
        raw_sha = (
            reflection["build"].get("exe_sha256")
            or reflection["build"].get("sha256")
            or reflection["build"].get("build_sha256")
        )
        reflection_sha = str(raw_sha).casefold() if raw_sha else None
    reflection_build_match = reflection_sha == actual_sha if reflection_sha else args.assume_build_match

    static_pe = static.get("process_event", {})
    static_pe_rva = parse_int(static_pe.get("rva"))
    static_pe_index = parse_int(static_pe.get("vtable_index"))
    reflected_pe_rva = parse_int(reflection["process_event"].get("rva"))
    reflected_pe_index = parse_int(
        reflection["process_event"].get("vtable_index", reflection["process_event"].get("slot"))
    )
    pe_rva_ok = reflected_pe_rva is None or reflected_pe_rva == static_pe_rva
    pe_index_ok = reflected_pe_index is None or reflected_pe_index == static_pe_index
    pe_symbol_ok = bool(process_symbols) and bool(pdb_result and pdb_result.get("match"))
    process_event_confirmed = pe_rva_ok and pe_index_ok and (pe_symbol_ok or reflection_build_match)

    output = dict(static)
    output["status"] = "static-unvalidated"
    output["dispatch_allowed"] = False
    output["validation"] = {
        "exe_sha256": actual_sha,
        "expected_sha256": expected_sha,
        "build_match": build_match,
        "pdb": pdb_result,
        "process_event_symbols": process_symbols,
        "process_event_static_rva": hex_str(static_pe_rva),
        "process_event_reflected_rva": hex_str(reflected_pe_rva),
        "process_event_vtable_index": hex_str(static_pe_index),
        "process_event_confirmed": process_event_confirmed,
        "reflection_build_sha256": reflection_sha,
        "reflection_build_match": bool(reflection_build_match),
        "globals": dict(reflection.get("globals", {})),
        "offsets": dict(reflection.get("offsets", {})),
        "reasons": [],
    }

    reasons: list[str] = output["validation"]["reasons"]
    if not build_match:
        reasons.append("EXE hash does not match the static verb map")
    if reflection and not reflection_build_match:
        reasons.append("reflection dump does not identify the exact EXE hash")
    if not pe_rva_ok:
        reasons.append("reflection ProcessEvent RVA disagrees with build-fresh scanner")
    if not pe_index_ok:
        reasons.append("reflection vtable index disagrees with build-fresh scanner")
    if not process_event_confirmed:
        reasons.append("ProcessEvent not confirmed by matching PDB symbol or reflection dump")

    required = {
        role.strip()
        for role in (args.required_roles or DEFAULT_REQUIRED_ROLES).split(",")
        if role.strip()
    }
    if not required:
        required = {str(c.get("role")) for c in static.get("candidates", []) if c.get("tier") == "tier1"}

    candidates = []
    for candidate in static.get("candidates", []):
        entry = dict(candidate)
        class_name = str(entry.get("class", ""))
        function = str(entry.get("function", ""))
        entry["symbols"] = find_verb_symbols(symbols, class_name, function)
        reflected = reflection_for(reflection, class_name, function)
        if reflected:
            entry["reflection"] = reflected
            entry["validated"] = bool(reflected["layout_known"] and reflected["flags_known"])
        else:
            entry["validated"] = False
        candidates.append(entry)
    output["candidates"] = candidates

    missing = [
        c.get("role")
        for c in candidates
        if c.get("role") in required and not c.get("validated")
    ]
    output["validation"]["required_roles"] = sorted(required)
    output["validation"]["missing_required_roles"] = missing
    if missing:
        reasons.append("missing parameter layout/flags for required roles: " + ", ".join(missing))

    ready = (
        build_match
        and bool(reflection)
        and reflection_build_match
        and process_event_confirmed
        and not missing
    )
    if ready:
        output["status"] = "reflection-validated"
        if args.allow_dispatch:
            output["dispatch_allowed"] = True
        else:
            reasons.append("reflection validated but --allow-dispatch was not supplied")
        output["unresolved_gaps"] = [
            "Any live ProcessEvent/vtable-shadow hook validation under EAC; injection attempt failed with VirtualAllocEx error 5.",
            "Runtime dispatch opt-ins are implemented but not exercised in a solo/private session.",
            "Generic interaction targets must be supplied with VRC_OUTLAST_TARGET_HINT during live validation.",
        ]
    elif pdb_result and pdb_result.get("match") and process_symbols:
        output["status"] = "symbol-validated"
        reasons.append("PDB symbols only; UFunction parameter layouts still unresolved")

    args.out.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    return output


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--static-map", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--symbols", type=Path)
    parser.add_argument("--pdb", type=Path)
    parser.add_argument("--reflection", type=Path)
    parser.add_argument("--allow-dispatch", action="store_true")
    parser.add_argument("--assume-build-match", action="store_true")
    parser.add_argument(
        "--required-roles",
        default=DEFAULT_REQUIRED_ROLES,
        help="comma-separated roles that must be reflection-validated before dispatch",
    )
    args = parser.parse_args(argv)

    result = ingest(args)
    print(
        f"status={result['status']} dispatch_allowed={result['dispatch_allowed']} "
        f"reasons={len(result['validation']['reasons'])}"
    )
    for reason in result["validation"]["reasons"]:
        print(f"  - {reason}")
    print(f"wrote {args.out}")
    return 0 if result["status"] != "build-mismatch" else 1


if __name__ == "__main__":
    raise SystemExit(main())

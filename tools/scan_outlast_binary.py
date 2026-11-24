#!/usr/bin/env python3
"""Build-fresh, PDB-free static discovery for The Outlast Trials.

Steam does not publish a separate debug-symbols depot for this game, and the
public install does not contain TOTClient-Win64-Shipping.pdb. This tool scans
the *installed* PE instead: every section is parsed, UTF-16 reflection/log
strings are mapped back to RVAs, common UE4 RIP-relative xrefs are indexed, and
public UE4.27 AOBs are evaluated against this exact build.

Nothing printed here is a confirmed symbol. Candidates are inputs for
disassembly and, preferably, a runtime reflection dump. The tool deliberately
prints both RVA and VA because confusing those two invalidated earlier checks.

Usage:
    python tools/scan_outlast_binary.py [path-to-TOTClient-Win64-Shipping.exe]
    python tools/scan_outlast_binary.py --disasm --json findings.json
"""

from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import re
import struct
import sys
from pathlib import Path
from typing import Any


DEFAULT_EXE = Path(
    r"C:\Program Files (x86)\Steam\steamapps\common\The Outlast Trials"
    r"\OPP\Binaries\Win64\TOTClient-Win64-Shipping.exe"
)

UTF16_RE = re.compile(rb"(?:[\x20-\x7e]\x00){4,}")


class PEImage:
    def __init__(self, data: bytes) -> None:
        if data[:2] != b"MZ":
            raise ValueError("not a PE/MZ file")
        pe_off = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe_off : pe_off + 4] != b"PE\0\0":
            raise ValueError("missing PE signature")

        coff = pe_off + 4
        num_sections = struct.unpack_from("<H", data, coff + 2)[0]
        opt_size = struct.unpack_from("<H", data, coff + 16)[0]
        opt = coff + 20
        magic = struct.unpack_from("<H", data, opt)[0]
        if magic != 0x20B:
            raise ValueError(f"unsupported optional header magic 0x{magic:04x}")

        self.data = data
        self.image_base = struct.unpack_from("<Q", data, opt + 24)[0]
        self.image_size = struct.unpack_from("<I", data, opt + 56)[0]
        self.entry_rva = struct.unpack_from("<I", data, opt + 16)[0]

        self.sections: list[dict[str, Any]] = []
        sec = opt + opt_size
        for i in range(num_sections):
            off = sec + i * 40
            name = data[off : off + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
            characteristics = struct.unpack_from("<I", data, off + 36)[0]
            self.sections.append(
                {
                    "name": name,
                    "vaddr": vaddr,
                    "vsize": vsize,
                    "rawptr": rawptr,
                    "rawsize": rawsize,
                    "characteristics": characteristics,
                }
            )

    @property
    def text(self) -> dict[str, Any]:
        section = next((s for s in self.sections if s["name"] == ".text"), None)
        if section is None:
            raise ValueError(".text section not found")
        return section

    def rva_to_offset(self, rva: int) -> int | None:
        for s in self.sections:
            delta = rva - int(s["vaddr"])
            if 0 <= delta < int(s["rawsize"]):
                return int(s["rawptr"]) + delta
        return None

    def offset_to_rva(self, offset: int) -> tuple[int, str] | None:
        for s in self.sections:
            delta = offset - int(s["rawptr"])
            if 0 <= delta < int(s["rawsize"]):
                return int(s["vaddr"]) + delta, str(s["name"])
        return None

    def section_at_rva(self, rva: int) -> str:
        for s in self.sections:
            span = max(int(s["vsize"]), int(s["rawsize"]))
            if int(s["vaddr"]) <= rva < int(s["vaddr"]) + span:
                return str(s["name"])
        return "<unmapped>"

    def read(self, rva: int, size: int) -> bytes:
        off = self.rva_to_offset(rva)
        if off is None:
            return b""
        return self.data[off : off + size]

    def u32(self, rva: int) -> int:
        raw = self.read(rva, 4)
        return struct.unpack("<I", raw)[0] if len(raw) == 4 else 0

    def i32(self, rva: int) -> int:
        raw = self.read(rva, 4)
        return struct.unpack("<i", raw)[0] if len(raw) == 4 else 0

    def fmt(self, rva: int) -> str:
        return f"RVA 0x{rva:X} / VA 0x{self.image_base + rva:X}"


def compile_pattern(text: str) -> tuple[bytes, bytes]:
    """Compile `AA BB ?? CC`. `??`, `?`, and upstream `FF` are wildcards."""
    pat = bytearray()
    mask = bytearray()
    for tok in text.split():
        if tok in ("?", "??", "ff", "FF"):
            pat.append(0)
            mask.append(0)
        elif len(tok) == 2 and all(c in "0123456789abcdefABCDEF" for c in tok):
            pat.append(int(tok, 16))
            mask.append(0xFF)
        else:
            raise ValueError(f"bad pattern token: {tok}")
    return bytes(pat), bytes(mask)


def scan_all(blob: bytes, pattern: str | tuple[bytes, bytes]) -> list[int]:
    pat, mask = compile_pattern(pattern) if isinstance(pattern, str) else pattern
    n = len(pat)
    if not n:
        return []
    anchor = next((i for i, m in enumerate(mask) if m), None)
    if anchor is None:
        return []
    needle = pat[anchor]
    hits: list[int] = []
    limit = len(blob) - n + 1
    for i in range(limit):
        if blob[i + anchor] != needle:
            continue
        if all((blob[i + j] & mask[j]) == pat[j] for j in range(n)):
            hits.append(i)
    return hits


def scan_sections(
    image: PEImage, pattern: str, section_names: set[str] | None = None
) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for s in image.sections:
        if section_names and str(s["name"]) not in section_names:
            continue
        rawptr = int(s["rawptr"])
        rawsize = int(s["rawsize"])
        if not rawsize:
            continue
        blob = image.data[rawptr : rawptr + rawsize]
        for idx in scan_all(blob, pattern):
            out.append({"rva": int(s["vaddr"]) + idx, "section": str(s["name"])})
    return out


def rva_of_file_offset(image: PEImage, offset: int) -> int | None:
    mapped = image.offset_to_rva(offset)
    return mapped[0] if mapped else None


def find_utf16(image: PEImage, needle: str) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for match in UTF16_RE.finditer(image.data):
        try:
            text = match.group().decode("utf-16le")
        except UnicodeDecodeError:
            continue
        if needle not in text:
            continue
        rva = rva_of_file_offset(image, match.start())
        if rva is None:
            continue
        out.append(
            {
                "rva": rva,
                "file_offset": match.start(),
                "section": image.section_at_rva(rva),
                "text": text,
            }
        )
    return out


def build_xrefs(image: PEImage) -> dict[int, list[dict[str, Any]]]:
    """Index common x86-64 RIP-relative loads/stores and direct calls/jmps.

    The OPP build puts some generated thunks in .rdata, so all raw sections are
    indexed rather than .text only. Section names are retained to filter noise.
    """
    xrefs: dict[int, list[dict[str, Any]]] = {}

    def add(target: int, at: int, kind: str, length: int, section: str) -> None:
        if target <= 0 or target >= image.image_size:
            return
        xrefs.setdefault(target, []).append(
            {"rva": at, "kind": kind, "length": length, "section": section}
        )

    for section in image.sections:
        rawptr = int(section["rawptr"])
        rawsize = int(section["rawsize"])
        if not rawsize:
            continue
        name = str(section["name"])
        blob = image.data[rawptr : rawptr + rawsize]
        base = int(section["vaddr"])

        i = 0
        n = len(blob)
        while i < n - 8:
            b0 = blob[i]
            if b0 in (0x48, 0x4C) and blob[i + 1] in (0x8D, 0x8B, 0x89):
                modrm = blob[i + 2]
                if modrm & 0xC7 == 0x05:
                    rel = struct.unpack_from("<i", blob, i + 3)[0]
                    add(
                        base + i + 7 + rel,
                        base + i,
                        f"{b0:02x}{blob[i+1]:02x}-rip",
                        7,
                        name,
                    )
                    i += 1
                    continue
            if b0 in (0x8B, 0x89) and blob[i + 1] & 0xC7 == 0x05:
                rel = struct.unpack_from("<i", blob, i + 2)[0]
                add(base + i + 6 + rel, base + i, f"{b0:02x}-rip", 6, name)
                i += 1
                continue
            if b0 == 0x44 and blob[i + 1] == 0x8B and blob[i + 2] & 0xC7 == 0x05:
                rel = struct.unpack_from("<i", blob, i + 3)[0]
                add(base + i + 7 + rel, base + i, "448b-rip", 7, name)
                i += 7
                continue
            if b0 in (0xE8, 0xE9):
                rel = struct.unpack_from("<i", blob, i + 1)[0]
                add(
                    base + i + 5 + rel,
                    base + i,
                    "call" if b0 == 0xE8 else "jmp",
                    5,
                    name,
                )
                i += 1
                continue
            i += 1
    return xrefs


def first_pattern_after(
    image: PEImage, start_rva: int, pattern: str, window: int
) -> int | None:
    off = image.rva_to_offset(start_rva)
    if off is None:
        return None
    blob = image.data[off : off + window]
    hits = scan_all(blob, pattern)
    return start_rva + hits[0] if hits else None


def last_pattern_before(
    image: PEImage, end_rva: int, pattern: str, window: int
) -> int | None:
    start_rva = max(0, end_rva - window)
    off = image.rva_to_offset(start_rva)
    end_off = image.rva_to_offset(end_rva)
    if off is None or end_off is None:
        return None
    hits = scan_all(image.data[off:end_off], pattern)
    return start_rva + hits[-1] if hits else None


def resolve_call(image: PEImage, rva: int) -> int:
    """Resolve a direct E8 call at `rva` (RVA of the E8 byte)."""
    if image.read(rva, 1) != b"\xe8":
        return 0
    rel = image.i32(rva + 1)
    return rva + 5 + rel


def skip_jump(image: PEImage, rva: int) -> int:
    raw = image.read(rva, 2)
    if not raw:
        return rva
    if raw[0] == 0xE9 and len(image.read(rva + 1, 4)) == 4:
        rel = image.i32(rva + 1)
        return rva + 5 + rel
    if raw[0] == 0xEB:
        rel = struct.unpack("<b", raw[1:2])[0]
        return rva + 2 + rel
    return rva


def prologue_hint(image: PEImage, rva: int) -> str:
    raw = image.read(rva, 24)
    if not raw:
        return "<unmapped>"
    return raw[:16].hex(" ")


def pdata_functions(image: PEImage) -> list[tuple[int, int]]:
    """Return sorted (begin, end) RVA function ranges from .pdata."""
    section = next((s for s in image.sections if s["name"] == ".pdata"), None)
    if section is None:
        return []
    out: list[tuple[int, int]] = []
    begin = int(section["rawptr"])
    end = begin + int(section["rawsize"])
    for off in range(begin, end, 12):
        b, e, _ = struct.unpack_from("<III", image.data, off)
        if b and e > b:
            out.append((b, e))
    out.sort()
    return out


def function_for(
    rva: int, ranges: list[tuple[int, int]], starts: list[int]
) -> tuple[int, int] | None:
    i = bisect.bisect_right(starts, rva) - 1
    if i >= 0 and rva < ranges[i][1]:
        return ranges[i]
    return None


def u64_at(image: PEImage, rva: int) -> int:
    raw = image.read(rva, 8)
    return struct.unpack("<Q", raw)[0] if len(raw) == 8 else 0


def rip_relative_targets(image: PEImage, start_rva: int, end_rva: int) -> set[int]:
    """Collect RIP-relative targets from 48/4C 8D/8B/89 instructions."""
    blob = image.read(start_rva, end_rva - start_rva)
    targets: set[int] = set()
    i = 0
    while i + 6 <= len(blob):
        j = i
        if 0x40 <= blob[j] <= 0x4F:
            j += 1
        if (
            j + 6 <= len(blob)
            and blob[j] in (0x8D, 0x8B, 0x89)
            and blob[j + 1] & 0xC7 == 0x05
        ):
            rel = struct.unpack_from("<i", blob, j + 2)[0]
            targets.add(start_rva + j + 6 + rel)
            i = j + 6
            continue
        i += 1
    return targets


def has_function_flag_test(image: PEImage, start_rva: int, end_rva: int) -> bool:
    """Look for `test reg/mem, 0x400` (FUNC_Native) inside one function."""
    blob = image.read(start_rva, end_rva - start_rva)
    for i in range(len(blob) - 6):
        if blob[i] != 0xF7:
            continue
        modrm = blob[i + 1]
        if (modrm >> 3) & 7 != 0:  # must be TEST r/m, imm32
            continue
        mod = modrm >> 6
        rm = modrm & 7
        if mod == 3:
            imm = blob[i + 2 : i + 6]
        elif mod == 0 and rm == 5:
            imm = blob[i + 6 : i + 10]
        elif mod == 1:
            imm = blob[i + 6 : i + 10]
        elif mod == 2:
            imm = blob[i + 10 : i + 14]
        else:
            continue
        if len(imm) == 4 and imm == b"\x00\x04\x00\x00":
            return True
    return False


def derive_process_event(image: PEImage) -> dict[str, Any] | None:
    """Statically identify UObject::ProcessEvent from vtable stores.

    The OPP fork has no matching PDB and rejects the public AOBs, so this
    follows the same structural facts a runtime dumper would verify:

      1. a UObject constructor stores a .rdata vtable into the object,
      2. slot 0x44 (+0x220) of that vtable is a .pdata function start,
      3. that function tests FUNC_Native (0x400) and references reflection
         strings, and
      4. CallFunctionByNameWithArguments calls through [reg+0x220].

    This is static identification, not a substitute for a solo/private runtime
    reflection check before installing a hook.
    """
    ranges = pdata_functions(image)
    if not ranges:
        return None
    starts = [r[0] for r in ranges]

    reflection_rvas: set[int] = set()
    for needle in (
        "/Script/CoreUObject",
        "FArrayProperty",
        "Attempted to access missing property",
    ):
        for a in find_utf16(image, needle):
            reflection_rvas.add(a["rva"])

    store_patterns = (
        "48 8D 05 ?? ?? ?? ?? 48 89",
        "48 8D 0D ?? ?? ?? ?? 48 89",
        "48 8D 15 ?? ?? ?? ?? 48 89",
        "4C 8D 05 ?? ?? ?? ?? 4C 89",
        "4C 8D 0D ?? ?? ?? ?? 4C 89",
        "4C 8D 15 ?? ?? ?? ?? 4C 89",
    )
    for pattern in store_patterns:
        for hit in scan_sections(image, pattern, {".text"}):
            store_rva = hit["rva"]
            vtable_rva = store_rva + 7 + image.i32(store_rva + 3)
            if image.section_at_rva(vtable_rva) != ".rdata":
                continue
            slot_value = u64_at(image, vtable_rva + 0x220)
            if not slot_value or slot_value < image.image_base:
                continue
            pe_rva = slot_value - image.image_base
            if image.section_at_rva(pe_rva) != ".text":
                continue
            fn = function_for(pe_rva, ranges, starts)
            if fn is None or fn[0] != pe_rva or fn[1] - fn[0] < 0x400:
                continue
            if not has_function_flag_test(image, fn[0], fn[1]):
                continue
            targets = rip_relative_targets(image, fn[0], fn[1])
            if not (targets & reflection_rvas):
                continue
            ctor = function_for(store_rva, ranges, starts)
            return {
                "uobject_vtable_rva": vtable_rva,
                "uobject_constructor_rva": ctor[0] if ctor else store_rva,
                "process_event_rva": pe_rva,
                "process_event_index": 0x44,
                "vtable_offset": 0x220,
                "process_event_size": fn[1] - fn[0],
                "reflection_strings": sorted(
                    image.section_at_rva(t)
                    + ":0x"
                    + f"{t:X}"
                    for t in (targets & reflection_rvas)
                ),
                "confidence": "static-identified; runtime reflection still required",
            }
    return None


def count_pe_indirect_calls(image: PEImage) -> int:
    """Count FF /[rax-rdi+0x220] calls used for ProcessEvent dispatch."""
    section = image.text
    blob = image.data[
        int(section["rawptr"]) : int(section["rawptr"]) + int(section["rawsize"])
    ]
    total = 0
    for modrm in range(0x90, 0x98):
        pat = bytes([0xFF, modrm, 0x20, 0x02, 0x00, 0x00])
        total += len(scan_all(blob, (pat, b"\xff" * len(pat))))
    return total


def add_candidate(
    out: list[dict[str, Any]],
    label: str,
    rva: int,
    source: str,
    note: str = "",
) -> None:
    if not rva:
        return
    out.append(
        {
            "label": label,
            "rva": rva,
            "source": source,
            "note": note,
            "prologue": prologue_hint(
                CURRENT_IMAGE, skip_jump(CURRENT_IMAGE, rva)
            )
            if CURRENT_IMAGE is not None
            else "",
        }
    )


CURRENT_IMAGE: PEImage | None = None


def derive_candidates(
    image: PEImage,
    xrefs: dict[int, list[dict[str, Any]]],
    pattern_hits: dict[str, list[dict[str, Any]]],
    anchors: dict[str, list[dict[str, Any]]],
) -> list[dict[str, Any]]:
    global CURRENT_IMAGE
    CURRENT_IMAGE = image
    out: list[dict[str, Any]] = []

    # UObject::ProcessEvent from the public UE4 function finder's callsite AOB.
    for hit in pattern_hits.get("process_event_callsite", []):
        rva = hit["rva"]
        off = image.rva_to_offset(rva)
        if off is None:
            continue
        tail = image.data[off : off + 20]
        e8 = scan_all(tail, "E8")
        if not e8:
            continue
        target = skip_jump(image, resolve_call(image, rva + e8[0]))
        add_candidate(
            out,
            "UObject::ProcessEvent (UE4FF callsite)",
            target,
            f"callsite {image.fmt(rva)}",
        )

    # Legacy broad prologue patterns retained for comparison.
    for label in ("process_event_full", "process_event_header"):
        for hit in pattern_hits.get(label, []):
            target = skip_jump(image, hit["rva"])
            add_candidate(out, f"ProcessEvent candidate ({label})", target, label)

    # StaticLoadObject: log-string xref -> later push prologue -> 40 55.
    for anchor in anchors.get("static_load_object", []):
        for xref in xrefs.get(anchor["rva"], []):
            if not xref["kind"].startswith(("48", "4c")):
                continue
            push = last_pattern_before(image, xref["rva"], "41 56 41 57 48", 0xBB8)
            if push is None:
                continue
            start = last_pattern_before(image, push, "40 55", 0x40)
            add_candidate(
                out,
                "UObject::StaticLoadObject",
                skip_jump(image, start or push),
                f"string xref {image.fmt(xref['rva'])}",
            )

    # CallFunctionByNameWithArguments: same backward prologue walk.
    for anchor in anchors.get("call_function_by_name", []):
        for xref in xrefs.get(anchor["rva"], []):
            if not xref["kind"].startswith(("48", "4c")):
                continue
            push = last_pattern_before(image, xref["rva"], "41 56 41 57 48", 0xDAC)
            if push is None:
                continue
            start = last_pattern_before(image, push, "40 55", 0x40)
            add_candidate(
                out,
                "UObject::CallFunctionByNameWithArguments",
                skip_jump(image, start or push),
                f"string xref {image.fmt(xref['rva'])}",
            )

    # GWorld direct RIP-relative pattern.
    for hit in pattern_hits.get("gworld", []):
        rva = hit["rva"]
        rel = image.i32(rva + 8)
        add_candidate(
            out,
            "GWorld",
            rva + 12 + rel,
            f"UE4FF pattern at {image.fmt(rva)}",
            "global pointer, not a function",
        )

    # GWorld fallback from the load-package log string.
    for anchor in anchors.get("gworld_fallback", []):
        for xref in xrefs.get(anchor["rva"], []):
            for store in ("48 89 1D", "48 89 2D", "4C 89 2D", "4C 89 1D", "48 89 15"):
                found = first_pattern_after(image, xref["rva"], store, 300)
                if found is None:
                    continue
                rel = image.i32(found + 3)
                add_candidate(
                    out,
                    "GWorld (fallback store)",
                    found + 7 + rel,
                    f"string xref {image.fmt(xref['rva'])}",
                    "global pointer, not a function",
                )

    # GUObjectArray direct RIP-relative pattern.
    for hit in pattern_hits.get("gobject", []):
        rva = hit["rva"]
        rel = image.i32(rva + 14)
        add_candidate(
            out,
            "GUObjectArray",
            rva + 18 + rel,
            f"UE4FF pattern at {image.fmt(rva)}",
            "global pointer, not a function",
        )

    # GUObjectArray fallback via SpawnActor -> object-array globals.
    for anchor in anchors.get("spawn_actor", []):
        for xref in xrefs.get(anchor["rva"], []):
            call = last_pattern_before(image, xref["rva"], "E8", 20)
            if call is None:
                continue
            spawn = skip_jump(image, resolve_call(image, call))
            add_candidate(
                out,
                "UWorld::SpawnActor",
                spawn,
                f"string xref {image.fmt(xref['rva'])}",
            )
            cmp = first_pattern_after(image, spawn, "3B 05 ?? ?? ?? ?? 7D ??", 0x2000)
            if cmp is None:
                continue
            rel_num = image.i32(cmp + 2)
            num_elems = cmp + 6 + rel_num
            lea_mov = first_pattern_after(
                image, cmp, "48 8D ?? 40 48 8B 05 ?? ?? ?? ??", 50
            )
            if lea_mov is None:
                continue
            rel_flags = image.i32(lea_mov + 7)
            flags = lea_mov + 11 + rel_flags
            layout = {0x14: "FChunkedFixedUObjectArray", 0xC: "FUObjectArray"}.get(
                num_elems - flags, "unknown layout"
            )
            add_candidate(
                out,
                "GUObjectArray (SpawnActor fallback)",
                flags - 0x10,
                f"SpawnActor {image.fmt(spawn)}",
                f"NumElements-Objects delta 0x{num_elems - flags:X}: {layout}",
            )

    for label, target_label in (
        ("init_game_state", "AGameModeBase::InitGameState"),
        ("actor_begin_play", "AActor::BeginPlay (call target)"),
    ):
        for hit in pattern_hits.get(label, []):
            rva = hit["rva"]
            if label == "actor_begin_play":
                target = skip_jump(image, resolve_call(image, rva + 3))
                source = f"callsite {image.fmt(rva)}"
            else:
                target = skip_jump(image, rva)
                source = f"prologue {image.fmt(rva)}"
            add_candidate(out, target_label, target, source)

    # Reflection anchors that should exist but need runtime validation.
    for label in ("fname_get_names", "engine_version"):
        for anchor in anchors.get(label, []):
            for xref in xrefs.get(anchor["rva"], []):
                add_candidate(
                    out,
                    f"{label} xref",
                    xref["rva"],
                    f"string {image.fmt(anchor['rva'])}",
                    "anchor only; not a resolved symbol",
                )

    deduped: list[dict[str, Any]] = []
    seen: set[tuple[str, int]] = set()
    for c in out:
        key = (c["label"], c["rva"])
        if key in seen:
            continue
        seen.add(key)
        deduped.append(c)
    return deduped


def maybe_disassemble(image: PEImage, candidates: list[dict[str, Any]], enabled: bool) -> None:
    if not enabled:
        return
    try:
        import capstone  # type: ignore
    except ImportError:
        print("\ndisassembly requested but capstone is not installed")
        return

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = False
    for c in candidates[:40]:
        rva = skip_jump(image, c["rva"])
        data = image.read(rva, 0x180)
        if not data:
            continue
        print(f"\n--- {c['label']} {image.fmt(rva)} ---")
        for insn in md.disasm(data, image.image_base + rva):
            print(
                f"  {insn.address:016X}  {insn.bytes.hex():<24} "
                f"{insn.mnemonic} {insn.op_str}"
            )


def print_section_table(image: PEImage) -> None:
    print(f"image base: 0x{image.image_base:X}")
    print(f"image size: 0x{image.image_size:X}")
    print(f"entry RVA: 0x{image.entry_rva:X}")
    print("\nsections:")
    for s in image.sections:
        print(
            f"  {s['name']:<8} RVA 0x{s['vaddr']:08X} "
            f"virtual 0x{s['vsize']:08X} raw 0x{s['rawsize']:08X} "
            f"@ file 0x{s['rawptr']:08X}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", nargs="?", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--disasm", action="store_true", help="disassemble candidates (needs capstone)")
    parser.add_argument("--json", type=Path, help="write all findings as JSON")
    parser.add_argument("--max", type=int, default=12, help="max items printed per group")
    args = parser.parse_args()

    exe = args.exe
    if not exe.is_file():
        print(f"error: executable not found: {exe}", file=sys.stderr)
        return 2

    data = exe.read_bytes()
    image = PEImage(data)
    print_section_table(image)
    print(f"\nfile size: {len(data)} bytes")
    print(f"sha256: {hashlib.sha256(data).hexdigest()}")

    pattern_specs = {
        "process_event_callsite": (
            "75 0E ?? ?? ?? 48 ?? ?? 48 ?? ?? E8 ?? ?? ?? ?? "
            "48 8B ?? 24 ?? 48 8B ?? 24 38 48 8B ?? 24 40",
            {".text"},
        ),
        "process_event_full": (
            "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? "
            "48 8D 6C 24 ?? 48 89 9D ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? "
            "48 33 C5 48 89 85 ?? ?? ?? ??",
            {".text"},
        ),
        "process_event_header": (
            "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? "
            "48 8D 6C 24 ??",
            {".text"},
        ),
        "gobject": (
            "8B 46 10 3B 46 3C 75 0F 48 8B D6 48 8D 0D ?? ?? ?? ?? E8",
            None,
        ),
        "gworld": (
            "0F 2E ?? 74 ?? 48 8B 1D ?? ?? ?? ?? 48 85 DB 74",
            None,
        ),
        "init_game_state": (
            "40 53 48 83 EC 20 48 8B 41 10 48 8B D9 48 8B 91",
            None,
        ),
        "actor_begin_play": (
            "48 8B D9 E8 ?? ?? ?? ?? F6 83 ?? ?? ?? ?? ?? 74 12 48 8B 03",
            None,
        ),
        "call_function_by_name_bytes": ("E8 ?? ?? ?? ?? 44 0A E8 FF C3", None),
        "uobject_constructor_bytes": ("E8 ?? ?? ?? ?? 33 F6 C6 43 28 01", None),
        # Additional public patterns. They are tested against this exact OPP
        # build rather than assumed valid: the custom fork has already made the
        # older UE4FF/Hogwarts/Dumper-7 prologue signatures fail.
        "pe_object_hogwarts": (
            "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? "
            "48 8D 6C 24 ?? 48 89 9D ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? "
            "48 33 C5 48 89 85 ?? ?? ?? ?? 8B 41",
            {".text"},
        ),
        "pe_actor_hogwarts": (
            "48 89 5C 24 ?? 48 89 6C 24 ?? 57 48 83 EC ?? "
            "F7 82 ?? ?? ?? ?? ?? ?? ?? ?? 49 8B E8 48 8B FA 48 8B D9 "
            "75 ?? 83 7A ?? ?? 74",
            {".text"},
        ),
        "pe_dumper7": (
            "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ?? ?? ?? ?? "
            "48 8D AC 24 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 31 E0 "
            "48 89 84 24 ?? ?? ?? ?? 8B 19",
            {".text"},
        ),
        "gobjects_hogwarts": (
            "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8D 04 D1 48 85 C0 74 12",
            None,
        ),
        "gnames_hogwarts": (
            "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B C0 C6",
            None,
        ),
        "gworld_hogwarts": (
            "48 8B 05 ?? ?? ?? ?? 48 83 C4 20 5B C3 8B 41 10",
            None,
        ),
    }

    pattern_hits: dict[str, list[dict[str, Any]]] = {}
    print("\nAOB patterns:")
    for label, (pattern, sections) in pattern_specs.items():
        hits = scan_sections(image, pattern, sections)
        pattern_hits[label] = hits
        shown = ", ".join(
            f"0x{h['rva']:X} ({h['section']})" for h in hits[: args.max]
        )
        suffix = " ..." if len(hits) > args.max else ""
        print(f"  {label:<28} {len(hits):>4} match(es): {shown}{suffix}")

    anchor_specs = {
        "static_load_object": "Failed to find object '{ClassName} {OuterName}.{ObjectName}'",
        "call_function_by_name": "'{Message}': Bad or missing property '{PropertyName}' when trying to call {FunctionName}",
        "gworld_fallback": "Failed to load package '%s' into a new game world.",
        "spawn_actor": "SpawnActor failed.",
        "fname_get_names": "Hardcoded name '%s' at index %i was duplicated (or unexpected concurrency). Existing entry is '%s'.",
        "engine_version": "%sunreal-v%i-%s.dmp",
    }
    anchors: dict[str, list[dict[str, Any]]] = {}
    print("\nUTF-16 anchors:")
    for label, needle in anchor_specs.items():
        found = find_utf16(image, needle)
        anchors[label] = found
        print(f"  {label:<28} {len(found):>4} hit(s)")
        for a in found[: args.max]:
            print(f"      0x{a['rva']:X} file 0x{a['file_offset']:X} ({a['section']})")

    print("\nIndexing .text xrefs ...")
    xrefs = build_xrefs(image)
    print(f"  indexed {sum(len(v) for v in xrefs.values())} xrefs to {len(xrefs)} targets")

    print("\nAnchor xrefs:")
    for label, found in anchors.items():
        if not found:
            continue
        for a in found[: args.max]:
            hits = xrefs.get(a["rva"], [])
            shown = ", ".join(f"0x{x['rva']:X} [{x['kind']}]" for x in hits[: args.max])
            print(f"  {label:<28} target 0x{a['rva']:X}: {len(hits)} xref(s) {shown}")

    candidates = derive_candidates(image, xrefs, pattern_hits, anchors)
    static_symbols = derive_process_event(image)
    if static_symbols is not None:
        candidates.insert(
            0,
            {
                "label": "UObject::ProcessEvent (UObject vtable +0x220)",
                "rva": static_symbols["process_event_rva"],
                "source": (
                    f"UObject ctor 0x{static_symbols['uobject_constructor_rva']:X} "
                    f"stores vtable 0x{static_symbols['uobject_vtable_rva']:X}"
                ),
                "note": static_symbols["confidence"],
                "prologue": prologue_hint(image, static_symbols["process_event_rva"]),
            },
        )

    print("\nStatic symbol validation:")
    if static_symbols is None:
        print("  UObject::ProcessEvent: not identified from this build")
    else:
        print(
            f"  UObject::ProcessEvent: RVA 0x{static_symbols['process_event_rva']:X} "
            f"(VA 0x{image.image_base + static_symbols['process_event_rva']:X})"
        )
        print(
            f"    UObject vtable 0x{static_symbols['uobject_vtable_rva']:X}, "
            f"slot 0x{static_symbols['process_event_index']:X} "
            f"(+0x{static_symbols['vtable_offset']:X}), "
            f"function size 0x{static_symbols['process_event_size']:X}"
        )
        print(
            f"    constructor 0x{static_symbols['uobject_constructor_rva']:X}; "
            f"reflection strings: {', '.join(static_symbols['reflection_strings']) or 'none'}"
        )
        print(
            f"    [reg+0x220] dispatch calls: {count_pe_indirect_calls(image)} "
            "(structural support, not runtime proof)"
        )
        print(f"    status: {static_symbols['confidence']}")

    print("\nDerived candidates (UNVERIFIED until disassembly/reflection confirms):")
    for c in candidates:
        print(f"  {c['label']}")
        print(f"    {image.fmt(c['rva'])}  source: {c['source']}")
        print(f"    bytes: {c['prologue']}")
        if c["note"]:
            print(f"    note: {c['note']}")

    print("\nRelevant UTF-16 strings:")
    keywords = (
        "interact", "valve", "revive", "door", "inventory", "grip", "hand",
        "camera", "useobject", "accessed none", "byteproperty",
    )
    out_strings: list[dict[str, Any]] = []
    for match in UTF16_RE.finditer(data):
        try:
            text = match.group().decode("utf-16le")
        except UnicodeDecodeError:
            continue
        if not any(k in text.lower() for k in keywords):
            continue
        rva = rva_of_file_offset(image, match.start())
        if rva is None:
            continue
        out_strings.append(
            {"rva": rva, "file_offset": match.start(), "text": text}
        )
    strings = out_strings
    print(f"  {len(strings)} hit(s); first {min(args.max, len(strings))}:")
    for s in strings[: args.max]:
        preview = s["text"].replace("\n", "\\n")[:140]
        print(f"    0x{s['rva']:X} file 0x{s['file_offset']:X}  {preview}")

    maybe_disassemble(image, candidates, args.disasm)

    if args.json:
        payload: dict[str, Any] = {
            "exe": str(exe),
            "file_size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "image_base": image.image_base,
            "image_size": image.image_size,
            "entry_rva": image.entry_rva,
            "sections": image.sections,
            "patterns": pattern_hits,
            "anchors": anchors,
            "xref_count": sum(len(v) for v in xrefs.values()),
            "static_symbols": static_symbols,
            "pe_indirect_call_count": count_pe_indirect_calls(image),
            "candidates": candidates,
            "strings": strings,
        }
        args.json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print(f"\nwrote {args.json}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

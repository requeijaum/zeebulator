#!/usr/bin/env python3
"""Summarize *executed* ARM opcodes from Zeebulator ZEEB_TRACE output.

Why this exists
---------------
A .mod is code mixed with strings, tables and compressed assets. Linear
Capstone/objdump decoding of its raw bytes calls arbitrary data "opcodes" and
produces false CP15/SVC/DSP findings. This tool accepts the trace written by
ZEEB_TRACE, whose lines are emitted by DebugHooks::OnExec immediately before
an instruction actually executes.

Example
-------
DISPLAY=:0 ZEEB_CPU=interp \
  ZEEB_TRACE=0x00100000-0x001a0000,200000,/tmp/tectoy.trace \
  build/tools/zeebulator_game_probe /path/tectoy.mod - - 17237912
python3 tools/opcode_stats.py /tmp/tectoy.trace --top 25

The default parser is intentionally strict: malformed lines are reported and
never treated as evidence. See zeebo-lle/notes/STATS_TECHNIQUES.md.
"""
from __future__ import annotations

import argparse
import collections
import math
import pathlib
import re
import sys

try:
    import capstone
except ImportError as exc:
    raise SystemExit("capstone is required: uv pip install capstone") from exc

TRACE = re.compile(r"^([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})\b")


def classify(mnemonic: str) -> str:
    m = mnemonic.lower()
    if m in {"ldr", "str", "ldrb", "strb", "ldrh", "strh", "ldrsb", "ldrsh", "ldm", "stm", "push", "pop"}:
        return "load/store"
    if m in {"b", "bl", "bx", "blx", "cbz", "cbnz"}:
        return "control-flow"
    if m in {"mul", "mla", "umull", "umlal", "smull", "smlal", "smlabb", "smlabt", "smulbb", "smultb"}:
        return "multiply/dsp"
    if m in {"mcr", "mrc", "mcrr", "mrrc", "cdp", "ldc", "stc"}:
        return "coprocessor"
    if m in {"svc", "swi", "bkpt", "udf", "clz", "qadd", "qsub", "qdadd", "qdsub"}:
        return "special/exception"
    return "data/other"


def decode(md: capstone.Cs, pc: int, opcode: int) -> tuple[str, str]:
    # The trace word is guest little-endian. Capstone gets exactly four bytes.
    insns = list(md.disasm(opcode.to_bytes(4, "little"), pc, count=1))
    return (insns[0].mnemonic, insns[0].op_str) if insns else ("<undecoded>", "")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace", type=pathlib.Path, help="ZEEB_TRACE output file")
    ap.add_argument("--top", type=int, default=20, help="number of hot PCs/opcodes (default: 20)")
    ap.add_argument("--json", type=pathlib.Path, help="write machine-readable report")
    args = ap.parse_args()

    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM | capstone.CS_MODE_LITTLE_ENDIAN)
    pcs: collections.Counter[int] = collections.Counter()
    words: collections.Counter[int] = collections.Counter()
    mnem: collections.Counter[str] = collections.Counter()
    cats: collections.Counter[str] = collections.Counter()
    malformed = 0
    total = 0
    hi = [0] * 16

    with args.trace.open("r", errors="replace") as fh:
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            match = TRACE.match(line)
            if not match:
                malformed += 1
                continue
            pc, opcode = (int(match.group(1), 16), int(match.group(2), 16))
            mnemonic, _ = decode(md, pc, opcode)
            total += 1
            pcs[pc] += 1
            words[opcode] += 1
            mnem[mnemonic] += 1
            cats[classify(mnemonic)] += 1
            hi[pc >> 28] += 1

    if not total:
        raise SystemExit("no valid executed-opcode records; check ZEEB_TRACE range/path")
    entropy = -sum((n / total) * math.log2(n / total) for n in pcs.values())
    report = {
        "trace": str(args.trace), "executed": total, "distinct_pcs": len(pcs),
        "pc_entropy_bits": entropy, "malformed_lines": malformed,
        "address_nibble_histogram": {f"{i:x}": n for i, n in enumerate(hi)},
        "categories": dict(cats),
        "mnemonics": [{"mnemonic": k, "hits": v} for k, v in mnem.most_common(args.top)],
        "hot_pcs": [],
    }
    # Keep one observed word per PC for human-readable hot-PC output.
    opcode_for_pc: dict[int, int] = {}
    with args.trace.open("r", errors="replace") as fh:
        for line in fh:
            match = TRACE.match(line)
            if match:
                opcode_for_pc.setdefault(int(match.group(1), 16), int(match.group(2), 16))
    for pc, hits in pcs.most_common(args.top):
        opcode = opcode_for_pc[pc]
        mnemonic, op_str = decode(md, pc, opcode)
        report["hot_pcs"].append({"pc": f"0x{pc:08x}", "opcode": f"0x{opcode:08x}",
                                  "instruction": f"{mnemonic} {op_str}".strip(), "hits": hits,
                                  "share": hits / total})

    print(f"executed instructions: {total:,}; distinct PCs: {len(pcs):,}; PC entropy: {entropy:.2f} bits")
    print(f"malformed trace lines ignored: {malformed}")
    print("PC high-nibble coverage: " + " ".join(f"{i:x}:{n}" for i, n in enumerate(hi)))
    print("\nOpcode categories:")
    for name, hits in cats.most_common():
        print(f"  {name:20s} {hits:10d}  {hits / total:6.2%}")
    print("\nHot executed PCs (not static byte guesses):")
    for row in report["hot_pcs"]:
        print(f"  {row['pc']} {row['opcode']}  {row['instruction']:<42s} {row['hits']:9d}  {row['share']:6.2%}")
    if args.json:
        import json
        args.json.write_text(json.dumps(report, indent=2) + "\n")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())

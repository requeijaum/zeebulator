#!/usr/bin/env python3
"""Cheap, evidence-preserving survey of a Zeebo binary or asset.

Runs ASCII/UTF-16 string extraction and optionally binwalk. It deliberately
does not call arbitrary bytes instructions. Use tools/opcode_stats.py on a
ZEEB_TRACE file for executed-opcode statistics instead.

Example:
  python3 tools/binary_survey.py /path/tectoy.mod --binwalk --json /tmp/tectoy.json
"""
from __future__ import annotations
import argparse, json, pathlib, re, shutil, subprocess

ASCII = re.compile(rb"[\x20-\x7e]{5,}")
UTF16 = re.compile(rb"(?:[\x20-\x7e]\x00){4,}")

def strings(data: bytes):
    ascii_s = [{"offset": f"0x{m.start():x}", "text": m.group().decode("ascii")} for m in ASCII.finditer(data)]
    utf16_s = []
    for m in UTF16.finditer(data):
        try:
            utf16_s.append({"offset": f"0x{m.start():x}", "text": m.group().decode("utf-16-le")})
        except UnicodeDecodeError:
            pass
    return ascii_s, utf16_s

def main():
    ap=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file",type=pathlib.Path)
    ap.add_argument("--grep",help="case-insensitive substring filter")
    ap.add_argument("--limit",type=int,default=100)
    ap.add_argument("--binwalk",action="store_true")
    ap.add_argument("--json",type=pathlib.Path)
    ns=ap.parse_args()
    data=ns.file.read_bytes()
    asc,u16=strings(data)
    if ns.grep:
        needle=ns.grep.lower()
        asc=[x for x in asc if needle in x["text"].lower()]
        u16=[x for x in u16 if needle in x["text"].lower()]
    out={"file":str(ns.file),"bytes":len(data),"ascii":asc,"utf16le":u16}
    print(f"{ns.file}: {len(data):,} bytes; ASCII={len(asc):,}; UTF-16LE={len(u16):,}")
    for label,rows in (("ASCII",asc),("UTF-16LE",u16)):
        print(f"\n{label} strings:")
        for row in rows[:ns.limit]: print(f"  {row['offset']:>10}  {row['text']}")
    if ns.binwalk:
        if not shutil.which("binwalk"):
            print("\nbinwalk unavailable")
        else:
            proc=subprocess.run(["binwalk",str(ns.file)],capture_output=True,text=True,check=False)
            out["binwalk"]={"exit":proc.returncode,"stdout":proc.stdout,"stderr":proc.stderr}
            print("\nbinwalk:\n"+proc.stdout.rstrip())
    if ns.json: ns.json.write_text(json.dumps(out,ensure_ascii=False,indent=2)+"\n")
if __name__=="__main__": main()

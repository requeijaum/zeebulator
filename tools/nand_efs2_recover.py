#!/usr/bin/env python3
"""Recover what is provable from a Zeebo EFS2 NAND image.

Two independent outputs, both evidence-based:

1. Directory tree: parse EFS2 dirent records
   `0x69 | inode u32 | reclen u8 | type u8 | parent_ref u32 | pad 0x00 | name`
   and print parent/child paths. This is metadata only and is byte-exact.

2. Payload candidates: EFS2 stores file data in 512-byte clusters listed in
   tables terminated by 0xFFFFFFFF. This tool enumerates those tables, rebuilds
   each chain and *identifies* payloads by content (text/config, PNG/BMP/GIF/
   TrueType/ARM code/BAR). Matching a payload to a file NAME is NOT solved:
   the gnode table that links inode -> chain has not been reversed yet, so
   association is reported as "unresolved". See the printed manifest.

Usage:
  python3 tools/nand_efs2_recover.py 1.1.2.bin --out DIR [--chain-scan]
"""
from __future__ import annotations
import argparse, pathlib, struct, sys
from collections import defaultdict

EFS2APPS_OFF = 0x3220000     # partition start proven in the 1.1.2 dump
CLUSTER = 512
DIRENT_MARKER = 0x69
CLUSTER_NONE = 0xFFFFFFFF

def iter_dirents(data: bytes, part_off: int):
    i = part_off
    end = len(data)
    while i + 12 <= end:
        if data[i] != DIRENT_MARKER:
            i += 1
            continue
        inode, = struct.unpack_from('<I', data, i + 1)
        reclen = data[i + 5]
        dtype = data[i + 6]
        parent_ref, = struct.unpack_from('<I', data, i + 7)
        if not (6 <= reclen <= 200) or data[i + 11] != 0:
            i += 1
            continue
        name_len = reclen - 5
        if i + 12 + name_len > end:
            i += 1
            continue
        raw = data[i + 12:i + 12 + name_len]
        if not all(32 <= c < 127 for c in raw):
            i += 1
            continue
        yield i, inode, dtype, parent_ref >> 8, raw.decode('ascii')
        i += 12 + name_len

def build_tree(dirents):
    by_inode = {}
    for off, inode, dtype, parent, name in dirents:
        by_inode.setdefault(inode, (name, parent, dtype))
    def path_of(inode, seen=()):
        if inode in seen or len(seen) > 40:
            return ''
        node = by_inode.get(inode)
        if node is None:
            return ''
        name, parent, _ = node
        if inode == parent:
            return '/'
        head = path_of(parent, seen + (inode,))
        return (head.rstrip('/') + '/' + name) if head else '/' + name
    out = []
    for inode, (name, parent, dtype) in by_inode.items():
        out.append((path_of(inode), inode, dtype))
    return out

def classify(payload: bytes) -> str:
    if payload[:8] == b'\x89PNG\r\n\x1a\n': return 'png'
    if payload[:2] == b'BM': return 'bmp'
    if payload[:6] in (b'GIF87a', b'GIF89a'): return 'gif'
    if payload[:4] in (b'OTTO', b'true', b'\x00\x01\x00\x00', b'ttcf'): return 'truetype'
    if payload[:4] == b'BAR\x00' or payload[:2] == b'PK': return 'container'
    if payload[:4] in (b'\x1f\x8b\x08\x00', b'\x1f\x8b\x08\x08'): return 'gzip'
    printable = sum(32 <= c < 127 or c in (9, 10, 13) for c in payload[:512])
    if printable > 480: return 'text'
    word, = struct.unpack_from('<I', payload, 0) if len(payload) >= 4 else (0,)
    if (word & 0x0f000000) in (0x0a000000, 0x0b000000, 0x0e000000, 0x0f000000, 0x01000000):
        return 'arm-code?'
    return 'unknown'

def chain_scan(data: bytes, part_off: int, limit: int | None = None):
    part_blocks = (len(data) - part_off) // CLUSTER
    found = []
    for blk in range(part_blocks):
        base = part_off + blk * CLUSTER
        words = struct.unpack_from('<%dI' % (CLUSTER // 4), data, base)
        k = 0
        while k < len(words):
            j = k
            while j < len(words) and words[j] not in (0, CLUSTER_NONE) and words[j] < part_blocks:
                j += 1
            run = words[k:j]
            if j < len(words) and words[j] == CLUSTER_NONE and len(run) >= 4:
                found.append((base, run))
                if limit and len(found) >= limit:
                    return found
                k = j + 1
            else:
                k = max(j + 1, k + 1)
    return found

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('nand', type=pathlib.Path)
    ap.add_argument('--out', type=pathlib.Path, default=pathlib.Path('efs2_recovered'))
    ap.add_argument('--tree-only', action='store_true')
    ap.add_argument('--tree-filter', default='sys|shared|font|lct|config|cfg')
    ap.add_argument('--chain-limit', type=int, default=0)
    ns = ap.parse_args()

    data = ns.nand.read_bytes()
    if len(data) <= EFS2APPS_OFF:
        print('image smaller than the EFS2APPS partition', file=sys.stderr)
        return 1
    ns.out.mkdir(parents=True, exist_ok=True)

    dirents = list(iter_dirents(data, EFS2APPS_OFF))
    tree = build_tree(dirents)
    uniq = {}
    for path, inode, dtype in tree:
        uniq.setdefault(path, (inode, dtype))
    print(f'dirents={len(dirents)} unique-paths={len(uniq)}')
    (ns.out / 'efs2_tree.txt').write_text(
        '\n'.join(f'{p}\tinode=0x{i:x}\ttype={t}' for p, (i, t) in sorted(uniq.items())) + '\n')
    print(f'wrote {ns.out/"efs2_tree.txt"}')

    if not ns.tree_only:
        chains = chain_scan(data, EFS2APPS_OFF, ns.chain_limit or None)
        print(f'cluster chains={len(chains)}')
        manifest = []
        for n, (base, run) in enumerate(chains):
            payload = b''.join(data[EFS2APPS_OFF + c * CLUSTER:EFS2APPS_OFF + (c + 1) * CLUSTER]
                               for c in run)
            kind = classify(payload)
            manifest.append((base, run[0], len(run), kind))
            if kind in ('text', 'truetype'):
                (ns.out / f'chain_{n:05d}_{kind}.bin').write_bytes(payload)
        (ns.out / 'chain_manifest.tsv').write_text(
            '\n'.join(f'0x{b:x}\tfirst_cluster=0x{c:x}\tclusters={k}\tkind={t}'
                      for b, c, k, t in manifest) + '\n')
        print(f'wrote {ns.out/"chain_manifest.tsv"} ({len(manifest)} chains)')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())

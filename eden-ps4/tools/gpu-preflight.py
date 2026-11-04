#!/usr/bin/env python3
"""(04.46) Where the GPU was when the PS4 went down, from Eden for PS4's pre-flight record.

gpu/preflight.txt holds the big submissions after a loading screen, written before they went to the
GPU (every IB's dwords, the code of the shaders they set); gpu/progress.txt says, every 2 ms, which
submission the command processor (CP) had started and which trace id it passed last. This walks the
submissions as the CP does and prints the packets from that trace id on, decoded, with the shaders
and registers in effect there.
  gpu-preflight.py <eden-ps4-logs.txt>            (the bundle: its gpu/preflight.txt and progress.txt)
  gpu-preflight.py <preflight.txt> [progress.txt]
  options: --trace N (another trace id), --after N (packets shown after it, default 60), --previous
           (the bundle's preflight-previous.txt and progress-previous.txt)"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

OPS = {
    0x10: "NOP", 0x11: "SET_BASE", 0x12: "CLEAR_STATE", 0x13: "INDEX_BUFFER_SIZE", 0x15: "DISPATCH_DIRECT",
    0x16: "DISPATCH_INDIRECT", 0x1D: "ATOMIC_GDS", 0x1E: "ATOMIC_MEM", 0x1F: "OCCLUSION_QUERY",
    0x20: "SET_PREDICATION", 0x21: "REG_RMW", 0x22: "COND_EXEC", 0x23: "PRED_EXEC", 0x24: "DRAW_INDIRECT",
    0x25: "DRAW_INDEX_INDIRECT", 0x26: "INDEX_BASE", 0x27: "DRAW_INDEX_2", 0x28: "CONTEXT_CONTROL",
    0x2A: "INDEX_TYPE", 0x2C: "DRAW_INDIRECT_MULTI", 0x2D: "DRAW_INDEX_AUTO", 0x2F: "NUM_INSTANCES",
    0x30: "DRAW_INDEX_MULTI_AUTO", 0x33: "INDIRECT_BUFFER_CONST", 0x34: "STRMOUT_BUFFER_UPDATE",
    0x35: "DRAW_INDEX_OFFSET_2", 0x36: "DRAW_PREAMBLE", 0x37: "WRITE_DATA", 0x38: "DRAW_INDEX_INDIRECT_MULTI",
    0x39: "MEM_SEMAPHORE", 0x3B: "COPY_DW", 0x3C: "WAIT_REG_MEM", 0x3F: "INDIRECT_BUFFER", 0x40: "COPY_DATA",
    0x41: "CP_DMA", 0x42: "PFP_SYNC_ME", 0x43: "SURFACE_SYNC", 0x44: "ME_INITIALIZE", 0x45: "COND_WRITE",
    0x46: "EVENT_WRITE", 0x47: "EVENT_WRITE_EOP", 0x48: "EVENT_WRITE_EOS", 0x49: "RELEASE_MEM",
    0x4A: "PREAMBLE_CNTL", 0x50: "DMA_DATA", 0x51: "CONTEXT_REG_RMW", 0x58: "ACQUIRE_MEM", 0x59: "REWIND",
    0x5F: "LOAD_SH_REG", 0x60: "LOAD_CONFIG_REG", 0x61: "LOAD_CONTEXT_REG", 0x68: "SET_CONFIG_REG",
    0x69: "SET_CONTEXT_REG", 0x6A: "SET_CONTEXT_REG_INDIRECT", 0x73: "SET_CONTEXT_REG_INDIRECT", 0x76: "SET_SH_REG",
    0x77: "SET_SH_REG_OFFSET", 0x78: "SET_QUEUE_REG", 0x79: "SET_UCONFIG_REG", 0x80: "LOAD_CONST_RAM",
    0x81: "WRITE_CONST_RAM", 0x83: "DUMP_CONST_RAM", 0x84: "INCREMENT_CE_COUNTER", 0x85: "INCREMENT_DE_COUNTER",
    0x86: "WAIT_ON_CE_COUNTER", 0x88: "WAIT_ON_DE_COUNTER_DIFF", 0x8E: "GET_LOD_STATS",
    0x9C: "DRAW_INDIRECT_COUNT_MULTI", 0x9D: "DRAW_INDEX_INDIRECT_COUNT_MULTI",
}
EVENTS = {
    0x04: "CACHE_FLUSH_TS", 0x06: "CACHE_FLUSH", 0x07: "CS_PARTIAL_FLUSH", 0x08: "VGT_STREAMOUT_SYNC",
    0x0F: "VS_PARTIAL_FLUSH", 0x10: "PS_PARTIAL_FLUSH", 0x14: "CACHE_FLUSH_AND_INV_TS", 0x15: "ZPASS_DONE",
    0x16: "CACHE_FLUSH_AND_INV", 0x19: "PIPELINESTAT_START", 0x1A: "PIPELINESTAT_STOP", 0x1E: "SAMPLE_PIPELINESTAT",
    0x24: "VGT_FLUSH", 0x28: "BOTTOM_OF_PIPE_TS", 0x2C: "FLUSH_AND_INV_DB_META", 0x2D: "FLUSH_AND_INV_CB_DATA_TS",
    0x2E: "FLUSH_AND_INV_CB_META", 0x2F: "CS_DONE", 0x30: "PS_DONE",
}
WORK = {0x15, 0x16, 0x24, 0x25, 0x27, 0x2C, 0x2D, 0x35, 0x38, 0x9C, 0x9D}
WAITS_EVENTS = {0x07, 0x0F, 0x10}
# shader address registers (SH offsets from 0xB000, in dwords) per stage
PGM = {"PS": 0x008, "VS": 0x048, "GS": 0x088, "ES": 0x0C8, "HS": 0x108, "LS": 0x148, "CS": 0x20C}


def load_registers():
    names = {}
    path = os.path.join(HERE, "gfx7-registers.txt")
    if os.path.exists(path):
        for line in open(path):
            if line[:1] != "#" and line.strip():
                at, name = line.split()
                names[int(at, 16)] = name
    return names


REGS = load_registers()


def reg_name(addr):
    return REGS.get(addr, "%#x" % addr)


def sections(lines, wanted):
    """The lines of a bundle's section `wanted` (e.g. gpu/preflight.txt), or None."""
    out, inside, found = [], False, False
    for l in lines:
        if l.startswith("===== "):
            inside = l.startswith("===== " + wanted + " ")
            found |= inside
            continue
        if inside:
            out.append(l)
    return out if found else None


class Submission:
    def __init__(self, seq, first, last):
        self.seq, self.first, self.last = seq, first, last
        self.top = []      # top-level IB addresses
        self.ibs = {}      # address -> dwords
        self.mem = {}      # (04.48) address -> 16 dwords at a user data pointer
        self.buffers = []  # (04.48) (start, size, handle, kind) of the live GPU buffers


WINDOWS = [0x4000000000, 0x4400000000]  # the 16 GiB and the 32-bit window (the header says, from 04.48)


def parse_preflight(lines):
    subs, shaders, trace_va = [], {}, None
    sub, ib, top_next, shader = None, None, False, None
    for line in lines:
        m = re.search(r"Trace ids at (0x[0-9a-f]+)", line)
        if m and trace_va is None:
            trace_va = int(m[1], 16)
        m = re.search(r"GPU windows (0x[0-9a-f]+), (0x[0-9a-f]+)", line)
        if m:
            WINDOWS[:] = [int(m[1], 16), int(m[2], 16)]
        m = re.match(r"^m ([0-9a-f]{10}):((?: [0-9a-f]{8}){16})\s*$", line)
        if m and sub:
            sub.mem[int(m[1], 16)] = [int(w, 16) for w in m[2].split()]
            continue
        m = re.match(r"^b (\d+) ([0-9a-f]{10}) ([0-9a-f]+) (\w+)(.*)$", line)
        if m and sub:
            sub.buffers.append((int(m[2], 16), int(m[3], 16), int(m[1]), m[4] + m[5]))
            continue
        m = re.match(r"==== submission (\d+): (\d+) IBs, trace ids (\d+)-(\d+)", line)
        if m:
            sub = Submission(int(m[1]), int(m[3]), int(m[4]))
            subs.append(sub)
            ib, shader = None, None
            continue
        if line.startswith("=== top-level IB"):
            top_next = True
            continue
        m = re.match(r"== IB (0x[0-9a-f]+), (\d+) dwords, depth (\d+)", line)
        if m and sub:
            va = int(m[1], 16)
            if top_next:
                sub.top.append(va)
                top_next = False
            ib = sub.ibs.setdefault(va, []) if va not in sub.ibs else None
            shader = None
            continue
        m = re.match(r"==== shader (\w+) (0x[0-9a-f]+)", line)
        if m:
            shader = shaders.setdefault(int(m[2], 16), {"stage": m[1], "words": []})
            ib = None
            continue
        if line.startswith("==="):
            ib, shader = None, None
            continue
        m = re.match(r"^([0-9a-f]{10}):((?: [0-9a-f]{8})+)\s*$", line)
        if m and ib is not None:
            ib.extend(int(w, 16) for w in m[2].split())
        elif shader is not None and re.match(r"^[0-9a-f]{8}( [0-9a-f]{8})*\s*$", line):
            shader["words"].extend(int(w, 16) for w in line.split())
    return subs, shaders, trace_va


def parse_progress(lines):
    rows = []
    for line in lines:
        m = re.match(r"\s*([\d.]+): (\d+) / (\d+) / (\d+) \(of (\d+)\); trace (\d+)", line)
        if m:
            rows.append({"ms": float(m[1]), "done": int(m[2]), "handed": int(m[3]), "started": int(m[4]),
                         "submitted": int(m[5]), "trace": int(m[6])})
    return rows


def walk(sub):
    """The submission's packets in the CP's order: (va, op, body, depth)."""
    out = []

    def ib(va, depth, budget=[200000]):
        for _ in range(256):
            words = sub.ibs.get(va)
            if words is None:
                out.append((va, -1, [], depth))  # not in the record
                return
            i, nxt = 0, None
            while i < len(words):
                h = words[i]
                if h >> 30 == 2 or h == 0xFFFF1000:
                    i += 1
                    continue
                if h >> 30 != 3:
                    out.append((va + 4 * i, -2, [h], depth))
                    return
                op, body = (h >> 8) & 0xFF, ((h >> 16) & 0x3FFF) + 1
                b = words[i + 1:i + 1 + body]
                if len(b) < body:
                    out.append((va + 4 * i, -2, [h], depth))
                    return
                budget[0] -= 1
                if budget[0] < 0:
                    return
                out.append((va + 4 * i, op, b, depth))
                if op in (0x3F, 0x33) and body >= 3:
                    target = b[0] | ((b[1] & 0xFFFF) << 32)
                    if b[2] & (1 << 20):
                        nxt = target
                        break
                    if depth < 4:
                        ib(target, depth + 1)
                i += 1 + body
            if nxt is None:
                return
            va = nxt

    for t in sub.top:
        ib(t, 0)
    return out


class State:
    def __init__(self):
        self.sh, self.ctx, self.uconfig = {}, {}, {}

    def apply(self, op, b):
        if op == 0x76 and b:
            for k, v in enumerate(b[1:]):
                self.sh[(b[0] & 0xFFFF) + k] = v
        elif op == 0x69 and b:
            for k, v in enumerate(b[1:]):
                self.ctx[(b[0] & 0xFFFF) + k] = v
        elif op == 0x79 and b:
            for k, v in enumerate(b[1:]):
                self.uconfig[(b[0] & 0xFFFF) + k] = v

    def shader(self, stage):
        lo, hi = self.sh.get(PGM[stage]), self.sh.get(PGM[stage] + 1, 0)
        return None if lo is None else (lo << 8) | ((hi & 0xFF) << 40)


def describe(op, b, state, trace_va):
    name = OPS.get(op, "op %#x" % op) if op >= 0 else ("(an IB not in the record)" if op == -1 else "(not a packet: %#x)" % b[0])
    if op < 0:
        return name
    if op in (0x76, 0x69, 0x79, 0x68) and b:
        base = {0x76: 0xB000, 0x69: 0x28000, 0x79: 0x30000, 0x68: 0x8000}[op]
        regs = ", ".join("%s=%#x" % (reg_name(base + 4 * ((b[0] & 0xFFFF) + k)), v) for k, v in enumerate(b[1:]))
        idx = " (index %d)" % (b[0] >> 28) if b[0] >> 28 else ""
        return "%s%s %s" % (name, idx, regs)
    if op == 0x37 and len(b) >= 4:
        addr = b[1] | (b[2] << 32)
        if trace_va and addr == trace_va:
            return "trace id %u" % b[3]
        return "%s to %#x (dst %d): %s" % (name, addr, (b[0] >> 8) & 0xF, " ".join("%#x" % v for v in b[3:7]))
    if op == 0x46 and b:
        return "%s %s" % (name, EVENTS.get(b[0] & 0x3F, "%#x" % (b[0] & 0x3F)))
    if op in (0x47, 0x48) and len(b) >= 4:
        return "%s %s to %#x data %#x" % (name, EVENTS.get(b[0] & 0x3F, "%#x" % (b[0] & 0x3F)), b[1] | ((b[2] & 0xFFFF) << 32), b[3])
    if op == 0x3C and len(b) >= 5:
        mem = (b[0] >> 4) & 1
        where = "memory %#x" % ((b[1] & ~3) | (b[2] << 32)) if mem else "register %s" % reg_name(b[1] * 4)
        return "%s %s func %d ref %#x mask %#x" % (name, where, b[0] & 7, b[3], b[4])
    if op == 0x40 and len(b) >= 5:
        src, dst = b[0] & 0xF, (b[0] >> 8) & 0xF
        s = reg_name(b[1] * 4) if src == 0 else "%#x" % (b[1] | (b[2] << 32))
        d = reg_name(b[3] * 4) if dst == 0 else "%#x" % (b[3] | (b[4] << 32))
        return "%s src_sel %d %s -> dst_sel %d %s%s" % (name, src, s, dst, d, " (64 bits)" if b[0] & (1 << 16) else "")
    if op == 0x50 and len(b) >= 6:
        return "%s %#x -> %#x, %u bytes (control %#x, command %#x)" % (
            name, b[1] | (b[2] << 32), b[3] | (b[4] << 32), b[5] & 0x1FFFFF, b[0], b[5])
    if op in (0x3F, 0x33) and len(b) >= 3:
        return "%s %#x, %u dwords%s" % (name, b[0] | ((b[1] & 0xFFFF) << 32), b[2] & 0xFFFFF, " (chain)" if b[2] & (1 << 20) else "")
    if op == 0x43 and len(b) >= 4:
        return "%s cntl %#x size %#x base %#x" % (name, b[0], b[1], b[2])
    text = "%s %s" % (name, " ".join("%#x" % v for v in b[:8]))
    if op in WORK:
        if op in (0x15, 0x16):
            stages = ["CS"]
            t = [state.sh.get(0x207 + k, 0) for k in range(3)]
            text += " (threads %dx%dx%d)" % (t[0] & 0xFFFF, t[1] & 0xFFFF, t[2] & 0xFFFF)
        else:
            en = state.ctx.get(0x2D5, 0)
            stages = ["VS", "PS"] + (["GS"] if en & (1 << 5) else []) + (["ES"] if (en >> 3) & 3 else []) + \
                     (["HS"] if en & 4 else []) + (["LS"] if en & 3 else [])
        for st in stages:
            va = state.shader(st)
            if va is not None:
                text += ", %s %#x" % (st, va)
    return text


def in_buffer(buffers, va, size=0):
    """Where `va` (and `size` bytes from it) lies among the live buffers."""
    if not buffers:
        return "(no buffer list in the record)"
    lo, hi = 0, len(buffers)
    while lo < hi:
        mid = (lo + hi) // 2
        if buffers[mid][0] <= va:
            lo = mid + 1
        else:
            hi = mid
    if lo:
        start, bsize, handle, kind = buffers[lo - 1]
        end = start + bsize + (0x10000 if "tail" in kind else 0)
        if va < end:
            over = va + size - end
            return "in bo %d (%#x, %#x bytes, %s) at +%#x%s" % (
                handle, start, bsize, kind, va - start, "; RUNS %#x PAST ITS END" % over if over > 0 else "")
    return "IN NO LIVE BUFFER"


CB_BPP = {1: 1, 2: 2, 3: 2, 0x10: 2, 0x11: 2, 0x12: 2, 0x13: 2, 4: 4, 5: 4, 6: 4, 7: 4, 8: 4, 9: 4, 0xA: 4, 0x14: 4,
          0x15: 4, 0xB: 8, 0xC: 8, 0x16: 8, 0xE: 16}


def targets(state, buffers):
    """The render targets a draw writes, each against the buffers."""
    out = []
    ctx = state.ctx
    if (ctx.get(0x202, 0) >> 4) & 7:
        for i in range(8):
            r = 0x318 + i * 15
            info = ctx.get(r + 4, 0)
            fmt = (info >> 2) & 0x1F
            if not fmt or not (ctx.get(0x8E, 0xFFFFFFFF) >> (4 * i)) & 0xF:
                continue
            base = ctx.get(r, 0) << 8
            pitch = ((ctx.get(r + 1, 0) & 0x7FF) + 1) * 8
            slice_px = ((ctx.get(r + 2, 0) & 0x3FFFFF) + 1) * 64
            last = (ctx.get(r + 3, 0) >> 13) & 0x7FF
            size = slice_px * CB_BPP.get(fmt, 16) * (last + 1)
            out.append("CB%d %#x (pitch %d, %d slices of %#x bytes, format %#x, tile mode %d): %s" % (
                i, base, pitch, last + 1, slice_px * CB_BPP.get(fmt, 16), fmt, ctx.get(r + 5, 0) & 0x1F,
                in_buffer(buffers, base, size)))
            if info & (3 << 13):
                out.append("  its CMASK %#x: %s" % (ctx.get(r + 7, 0) << 8, in_buffer(buffers, ctx.get(r + 7, 0) << 8)))
    z = ctx.get(0x10, 0) & 3
    if z:
        for reg, what in ((0x12, "depth read"), (0x14, "depth write")):
            out.append("DB %s %#x: %s" % (what, ctx.get(reg, 0) << 8, in_buffer(buffers, ctx.get(reg, 0) << 8)))
    if ctx.get(0x11, 0) & 1:
        for reg, what in ((0x13, "stencil read"), (0x15, "stencil write")):
            out.append("DB %s %#x: %s" % (what, ctx.get(reg, 0) << 8, in_buffer(buffers, ctx.get(reg, 0) << 8)))
    return out


def descriptors(words, buffers):
    """V#s and T#s among 16 dwords at a user data pointer, each against the buffers."""
    out, j = [], 0
    while j + 4 <= len(words):
        d = words[j:j + 8]
        d3 = d[3]
        if d3 & 0xFFF and d3 >> 30 == 0 and d[2] and (d3 >> 15) & 0xF:
            base = d[0] | ((d[1] & 0xFFFF) << 32)
            stride = (d[1] >> 16) & 0x3FFF
            size = d[2] * stride if stride else d[2]
            out.append("V# at +%d: %#x, %d records x %d bytes, data format %d: %s" % (
                4 * j, base, d[2], stride, (d3 >> 15) & 0xF, in_buffer(buffers, base, size)))
            j += 4
        elif 9 <= d3 >> 28 <= 15 and j + 8 <= len(words):
            base = (d[0] | ((d[1] & 0xFF) << 32)) << 8
            width = ((d[2] & 0x3FFF) + 1)
            height = ((d[2] >> 14) & 0x3FFF) + 1
            out.append("T# at +%d: %#x, %dx%d, data format %d, levels %d-%d, tile mode %d, pitch %d, type %d: %s" % (
                4 * j, base, width, height, (d[1] >> 20) & 0x3F, (d3 >> 12) & 0xF, (d3 >> 16) & 0xF,
                (d3 >> 20) & 0x1F, ((d[4] >> 13) & 0x3FFF) + 1, d3 >> 28, in_buffer(buffers, base)))
            j += 8
        else:
            j += 4
    return out


USER_DATA = {"PS": 0x00C, "VS": 0x04C, "GS": 0x08C, "ES": 0x0CC, "HS": 0x10C, "LS": 0x14C, "CS": 0x240}


def work_details(op, state, sub, buffers):
    lines = []
    stages = ["CS"] if op in (0x15, 0x16) else ["VS", "PS"]
    if op not in (0x15, 0x16):
        lines += targets(state, buffers)
    for st in stages:
        for k in range(16):
            v = state.sh.get(USER_DATA[st] + k)
            if v is None:
                continue
            for ptr in (WINDOWS[1] + v,):
                words = sub.mem.get(ptr)
                if words:
                    found = descriptors(words, buffers)
                    lines.append("%s user data %d -> %#x: %s" % (st, k, ptr, "; ".join(found) if found else
                                                                 " ".join("%08x" % w for w in words[:8]) + " ..."))
    return lines


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = argv[1:]
    want_trace = None
    after = 60
    for i, a in enumerate(opts):
        if a == "--trace" and i + 1 < len(opts):
            want_trace = int(opts[i + 1])
        if a == "--after" and i + 1 < len(opts):
            after = int(opts[i + 1])
    args = [a for i, a in enumerate(argv[1:]) if not a.startswith("--") and (i == 0 or argv[i] not in ("--trace", "--after"))]
    if not args:
        print(__doc__)
        return 1
    lines = open(args[0], errors="replace").read().splitlines()
    suffix = "-previous" if "--previous" in opts else ""
    pre = sections(lines, "gpu/preflight%s.txt" % suffix)
    prog = sections(lines, "gpu/progress%s.txt" % suffix)
    if pre is None:
        pre = lines
        prog = open(args[1], errors="replace").read().splitlines() if len(args) > 1 else []
    subs, shaders, trace_va = parse_preflight(pre)
    rows = parse_progress(prog or [])
    print("%d submissions in the record (%s), %d shaders; trace ids at %s" % (
        len(subs), ", ".join(str(s.seq) for s in subs[:12]) + (" ..." if len(subs) > 12 else ""), len(shaders),
        "%#x" % trace_va if trace_va else "?"))
    if rows:
        last = rows[-1]
        print("progress: %d lines, the last at %.1f ms: completed %d, handed to the system %d, the CP started %d "
              "(of %d submitted), trace id %d" % (len(rows), last["ms"], last["done"], last["handed"],
                                                  last["started"], last["submitted"], last["trace"]))
        moved = [r for r in rows if r["trace"] != last["trace"] or r["done"] != last["done"]]
        if moved:
            print("  ... unchanged since %.1f ms" % moved[-1]["ms"])
    trace = want_trace if want_trace is not None else (rows[-1]["trace"] if rows else None)
    done = rows[-1]["done"] if rows else 0
    hit = None
    for sub in subs:
        if sub.seq <= done and want_trace is None:
            continue
        pkts = walk(sub)
        for i, (va, op, b, depth) in enumerate(pkts):
            if op == 0x37 and len(b) >= 4 and b[3] == trace and (not trace_va or (b[1] | (b[2] << 32)) == trace_va):
                hit = (sub, pkts, i)
                break
        if hit:
            break
    if not hit:
        first = next((s for s in subs if s.seq > done), subs[0] if subs else None)
        if not first:
            print("nothing to show")
            return 0
        if trace == 0x7FFF0001:  # (04.51) the probe's
            print("trace id %#x is the probe's: the CP wrote the probe address and stopped before the first trace "
                  "id of submission %d: from its start" % (trace, first.seq))
        else:
            print("trace id %s is in none of the recorded submissions after %d: from the start of submission %d" % (
                trace, done, first.seq))
        hit = (first, walk(first), -1)
    else:
        print("the CP passed trace id %d last, in submission %d (packet %d of %d, at %#x)" % (
            trace, hit[0].seq, hit[2], len(hit[1]), hit[1][hit[2]][0]))
    sub, pkts, at = hit
    state = State()  # the registers as the recorded submissions before it left them, then its own
    for earlier in subs:
        if earlier is sub:
            break
        for va, op, b, depth in walk(earlier):
            state.apply(op, b)
    for va, op, b, depth in pkts[:max(at, 0)]:
        state.apply(op, b)
    print("\nin effect there: shaders %s" % ", ".join(
        "%s %#x" % (st, state.shader(st)) for st in PGM if state.shader(st) is not None))
    print("\nthe packets from there (> marks draws and dispatches, ! the CP waits at it):")
    shown_work = 0
    for va, op, b, depth in pkts[max(at, 0):max(at, 0) + after]:
        mark = ">" if op in WORK else "!" if (op == 0x46 and b and (b[0] & 0x3F) in WAITS_EVENTS) or op in (0x3C, 0x39, 0x43) else " "
        print("%s %s%#x: %s" % (mark, "  " * depth, va, describe(op, b, state, trace_va)))
        state.apply(op, b)
        shown_work += op in WORK
    # (04.48) the work since the last wait before the stop (it hadn't finished), and the next one:
    # render targets and descriptors against the live buffers
    buffers = []
    for s2 in subs:
        if s2.buffers:
            buffers = sorted(s2.buffers)
        if s2 is sub:
            break
    if at >= 0:
        last_wait = 0
        for i, (va, op, b, depth) in enumerate(pkts[:at]):
            if (op == 0x46 and b and (b[0] & 0x3F) in WAITS_EVENTS) or op in (0x3C, 0x39, 0x43):
                last_wait = i
        print("\nthe work issued since the last wait before it (%d packets back), and the next:" % (at - last_wait))
        st2 = State()
        for earlier in subs:
            if earlier is sub:
                break
            for va, op, b, depth in walk(earlier):
                st2.apply(op, b)
        nxt = 0
        for i, (va, op, b, depth) in enumerate(pkts):
            st2.apply(op, b)
            if i < last_wait or op not in WORK:
                continue
            print("> %#x: %s" % (va, describe(op, b, st2, trace_va)))
            for line in work_details(op, st2, sub, buffers):
                print("    " + line)
            if i > at:
                nxt += 1
                if nxt >= 1:
                    break
    used = sorted({state.shader(st) for st in PGM if state.shader(st) is not None})
    for va in used:
        s = shaders.get(va)
        print("shader %#x: %s" % (va, "%s, %d bytes in the record (gpu-hang-shaders.py disassembles it)" % (
            s["stage"], 4 * len(s["words"])) if s else "not in the record"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

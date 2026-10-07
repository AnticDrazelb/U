# Decodes the GPU flight recorder's command streams (port/mesa/ac_ps4_drm.c: ib-N.bin, records of
# 'IBRC' depth seq va num_dw top + the IB's dwords) and shows what the GPU did around a RADV trace
# id (progress.log's "trace N": the draw the GPU reached when the app was killed).
#   python decode-flight.py ib-2.bin [trace id] [packets before] [packets after]
import struct
import sys

OPS = {0x10: "NOP", 0x12: "CLEAR_STATE", 0x13: "INDEX_BUFFER_SIZE", 0x15: "DISPATCH_DIRECT", 0x16: "DISPATCH_INDIRECT",
       0x26: "INDEX_BASE", 0x27: "DRAW_INDEX_2", 0x28: "CONTEXT_CONTROL", 0x2A: "INDEX_TYPE", 0x2D: "DRAW_INDEX_AUTO",
       0x2F: "NUM_INSTANCES", 0x33: "INDIRECT_BUFFER_CONST", 0x35: "DRAW_INDEX_OFFSET_2", 0x37: "WRITE_DATA",
       0x3C: "WAIT_REG_MEM", 0x3F: "INDIRECT_BUFFER", 0x40: "COPY_DATA", 0x41: "CP_DMA", 0x43: "SURFACE_SYNC",
       0x46: "EVENT_WRITE", 0x47: "EVENT_WRITE_EOP", 0x49: "RELEASE_MEM", 0x50: "DMA_DATA", 0x68: "SET_CONFIG_REG",
       0x69: "SET_CONTEXT_REG", 0x6A: "SET_CONTEXT_REG_INDIRECT", 0x76: "SET_SH_REG", 0x79: "SET_UCONFIG_REG",
       0x24: "DRAW_INDIRECT", 0x25: "DRAW_INDEX_INDIRECT", 0x2C: "DRAW_INDIRECT_MULTI", 0x38: "DRAW_INDEX_INDIRECT_MULTI",
       0x80: "LOAD_SH_REG", 0x61: "LOAD_CONTEXT_REG", 0x5F: "LOAD_CONFIG_REG", 0x22: "COND_EXEC", 0x2E: "DRAW_INDEX_MULTI_AUTO",
       0x3B: "PFP_SYNC_ME"}
DRAWS = {0x27, 0x2D, 0x35, 0x24, 0x25, 0x2C, 0x38, 0x2E, 0x15, 0x16}
# a few GFX7 register names (byte addresses)
REGS = {0x28B54: "VGT_SHADER_STAGES_EN", 0x28A40: "VGT_GS_MODE", 0x30908: "VGT_PRIMITIVE_TYPE",
        0x28AAC: "VGT_ESGS_RING_ITEMSIZE", 0x28AB0: "VGT_GSVS_RING_ITEMSIZE", 0x28B38: "VGT_GS_MAX_VERT_OUT",
        0x28A6C: "VGT_GS_OUT_PRIM_TYPE", 0x28B90: "VGT_GS_INSTANCE_CNT", 0x28A54: "VGT_GS_PER_ES",
        0x28A58: "VGT_ES_PER_GS", 0x28A5C: "VGT_GS_PER_VS", 0x28B5C: "VGT_GS_VERT_ITEMSIZE",
        0x28A64: "VGT_GSVS_RING_OFFSET_1", 0x28A68: "VGT_GSVS_RING_OFFSET_2", 0x28A60: "VGT_GSVS_RING_OFFSET_3",
        0x28A84: "VGT_PRIMITIVEID_EN", 0x28A94: "VGT_MULTI_PRIM_IB_RESET_EN", 0x28AA8: "IA_MULTI_VGT_PARAM",
        0x28810: "PA_CL_CLIP_CNTL", 0x28814: "PA_SU_SC_MODE_CNTL", 0x28A4C: "PA_SC_MODE_CNTL_1",
        0x30938: "VGT_GS_... (uconfig)", 0x2C0C: "?", 0x28B50: "VGT_ES_PER_GS?",
        0xB220: "SPI_SHADER_PGM_LO_GS", 0xB224: "SPI_SHADER_PGM_HI_GS", 0xB320: "SPI_SHADER_PGM_LO_ES",
        0xB120: "SPI_SHADER_PGM_LO_VS", 0xB020: "SPI_SHADER_PGM_LO_PS", 0x2834C: "?",
        0x28A00: "PA_SU_POINT_SIZE", 0x28C70: "CB_COLOR0_INFO", 0x28C60: "CB_COLOR0_BASE",
        0x28040: "DB_Z_INFO", 0x28048: "DB_Z_READ_BASE", 0x28B48: "VGT_STRMOUT_CONFIG", 0x28B94: "VGT_STRMOUT_CONFIG2?"}


def records(data):
    at = 0
    while at + 32 <= len(data):
        magic, depth, seq, va, num_dw, top = struct.unpack_from("<IIQQII", data, at)
        if magic != 0x43524249:
            break
        dws = struct.unpack_from("<%dI" % num_dw, data, at + 32)
        yield depth, seq, va, dws, top
        at += 32 + 4 * num_dw


def packets(dws):
    i = 0
    while i < len(dws):
        h = dws[i]
        if h >> 30 == 2:
            i += 1
            continue
        if h >> 30 != 3:
            yield i, None, [h]
            i += 1
            continue
        op = (h >> 8) & 0xFF
        body = ((h >> 16) & 0x3FFF) + 1
        yield i, op, list(dws[i + 1:i + 1 + body])
        i += 1 + body


def describe(op, body):
    name = OPS.get(op, "OP_%#x" % op)
    base = {0x68: 0x8000, 0x69: 0x28000, 0x76: 0xB000, 0x79: 0x30000}.get(op)
    if base is not None and body:
        start = base + (body[0] & 0xFFFF) * 4
        regs = []
        for k, v in enumerate(body[1:]):
            addr = start + 4 * k
            regs.append("%s=%#x" % (REGS.get(addr, "%#x" % addr), v))
        return "%s %s" % (name, " ".join(regs))
    return "%s %s" % (name, " ".join("%#x" % v for v in body[:8]) + (" ..." if len(body) > 8 else ""))


data = open(sys.argv[1], "rb").read()
trace = int(sys.argv[2]) if len(sys.argv) > 2 else None
before = int(sys.argv[3]) if len(sys.argv) > 3 else 40
after = int(sys.argv[4]) if len(sys.argv) > 4 else 15

flat = []  # (record index, depth, packet index, op, body)
for r, (depth, seq, va, dws, top) in enumerate(records(data)):
    for i, op, body in packets(dws):
        flat.append((r, depth, seq, va, i, op, body))
print("%d records, %d packets, %d draws" % (len({f[0] for f in flat}), len(flat), sum(1 for f in flat if f[5] in DRAWS)))
if trace is None:
    sys.exit()
# RADV's trace marker: WRITE_DATA of the id (radv_cmd_buffer_trace_emit)
hits = [n for n, f in enumerate(flat) if f[5] == 0x37 and len(f[6]) >= 4 and f[6][3] == trace]
print("trace %d written at packets %s" % (trace, hits))
for h in hits[-1:]:
    for n in range(max(0, h - before), min(len(flat), h + after)):
        r, depth, seq, va, i, op, body = flat[n]
        mark = ">>" if n == h else ("  D" if op in DRAWS else "   ")
        print("%s rec %d d%d +%04x %s" % (mark, r, depth, i, describe(op, body) if op is not None else "raw %#x" % body[0]))

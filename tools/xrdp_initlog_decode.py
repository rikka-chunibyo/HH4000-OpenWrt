#!/usr/bin/env python3
"""
xrdp_initlog_decode.py - decode Broadcom U-Boot "reduced XRDP" access logs
(drivers/net/bcmbca/xrdp/data_path_<chip>.c, access_log_tuple_t init_data[])
into an annotated register-level init sequence, using work/regdb/rdpa_fields.json.

usage: python3 -I tools/xrdp_initlog_decode.py data_path_6858.c rdpa_fields.json out_prefix

Tuple format (access_logging.h): word0 = size[31:28] | op[27:24] | addr[23:0]
(addr is an offset from 0x82000000), word1 = value.  ops: 0 WRITE, 1 MWRITE,
2 MEMSET (value=len, next tuple = {addr, fill}), 3 MEMSET_32, 4 SLEEP,
5 SET_CORE_ADDR, 6 STOP, 7 MEMSET_ADAPTIVE_32, 8 WRITE_6BYTE_ADDR, 9 POLL.
Outputs: <out>.txt (full annotated log, runs of SRAM/firmware writes
collapsed) and <out>_summary.txt (per-phase block order).
"""
import sys, re, json, bisect, collections

OPS = ['WRITE', 'MWRITE', 'MEMSET', 'MEMSET_32', 'SLEEP', 'SET_CORE_ADDR', 'STOP',
       'MEMSET_ADAPTIVE_32', 'WRITE_6BYTE_ADDR', 'POLL']

def load_db(p):
    d = json.load(open(p))
    blocks = d['blocks'] if isinstance(d, dict) else d
    regs = []   # (abs_addr_start, abs_addr_end, desc, reg)
    spans = []  # block instance spans
    for b in blocks:
        for i, base in enumerate(b['bases']):
            for r in b['regs']:
                n = r['ram_count'] + 1 if r['ram_count'] else 1
                stride = r['stride'] or 4
                for k in range(n):
                    a = base + r['addr'] + k * stride
                    regs.append((a, '%s%s.%s%s' % (b['name'], ('[%d]' % i) if len(b['bases']) > 1 else '',
                                                   r['name'], ('[%d]' % k) if n > 1 else ''), r))
    regs.sort(key=lambda x: x[0])
    return regs, blocks

# memory windows that are SRAM, not registers (6858; from the RU bases)
def mem_region(a, blocks):
    names = {b['name']: b['bases'] for b in blocks}
    for i, base in enumerate(names['RNR_MEM']):
        if base <= a < base + 0x10000: return 'RNR_MEM[%d] (core %d data SRAM)' % (i, i), base
    for i, base in enumerate(names['RNR_INST']):
        if base <= a < base + 0x8000: return 'RNR_INST[%d] (core %d instruction RAM)' % (i, i), base
    for i, base in enumerate(names['RNR_CNTXT']):
        if base <= a < base + 0x4000: return 'RNR_CNTXT[%d] (core %d thread context RAM)' % (i, i), base
    for i, base in enumerate(names['RNR_PRED']):
        if base <= a < base + 0x4000: return 'RNR_PRED[%d] (core %d branch prediction RAM)' % (i, i), base
    if names['PSRAM_MEM'][0] <= a < names['PSRAM_MEM'][0] + 0x80000: return 'PSRAM_MEM (packet SRAM)', names['PSRAM_MEM'][0]
    return None, None

def main():
    src, dbp, out = sys.argv[1:4]
    regs, blocks = load_db(dbp)
    addrs = [r[0] for r in regs]
    txt = open(src).read()
    tup = []
    for m in re.finditer(r'\{\s*(0x[0-9a-fA-F]+|\(ACCESS_LOG_OP_STOP << 24\))\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*\}', txt):
        w0 = m.group(1); w0 = (6 << 24) if 'STOP' in w0 else int(w0, 16)
        tup.append((w0, int(m.group(2), 0)))
    lines = []; summ = []; i = 0; run = None
    blockseq = []
    def flush():
        nonlocal run
        if run:
            lines.append('    %s  +0x%04x..+0x%04x  %d x 32-bit writes (%s)' % (run[0], run[1], run[2] + 3, run[3], run[4]))
            summ.append(('SRAM', run[0], run[3]))
            run = None
    while i < len(tup):
        w0, v = tup[i]
        size, op, a = w0 >> 28, (w0 >> 24) & 0xf, (w0 & 0xffffff) + 0x82000000
        if op in (0, 1, 8):
            if op == 8: a -= 0x82000000
            rg, rbase = mem_region(a, blocks)
            if rg:
                if run and run[0] == rg and 0 < a - rbase - run[2] <= 0x40:
                    run[2] = a - rbase; run[3] += 1
                else:
                    flush(); run = [rg, a - rbase, a - rbase, 1, 'op %s size %d' % (OPS[op], size)]
                i += 1; continue
            flush()
            k = bisect.bisect_right(addrs, a) - 1
            desc = '?'; fdesc = ''
            if k >= 0 and regs[k][0] == a:
                desc = regs[k][1]; r = regs[k][2]
                fs = []
                for f in r.get('fields', []):
                    fv = (v & f['mask']) >> f['lsb']
                    if fv: fs.append('%s=0x%x' % (f['name'], fv))
                fdesc = ' '.join(fs)
                blk = desc.split('.')[0]
                if not blockseq or blockseq[-1][0] != blk: blockseq.append([blk, 0])
                blockseq[-1][1] += 1
            elif k >= 0:
                desc = '%s+0x%x?' % (regs[k][1], a - regs[k][0])
            lines.append('W%d 0x%08x = 0x%08x  %-60s %s' % (size, a, v, desc, fdesc))
        elif op in (2, 3, 7):
            flush()
            n = v; a2, fill = tup[i + 1]
            rg, rbase = mem_region(a2, blocks)
            where = ('%s +0x%x' % (rg, a2 - rbase)) if rg else '0x%08x' % a2
            if op == 7:
                lines.append('MEMSET_ADAPTIVE_32 %s  n=%d start=%d step=%d' % (where, n, fill & 0xffff, fill >> 16))
            else:
                lines.append('%s %s len=0x%x fill=0x%x' % (OPS[op], where, n, fill))
            summ.append(('MEMSET', where, n))
            i += 2; continue
        elif op == 5:
            flush(); lines.append('SET_CORE_ADDR[%d] = 0x%08x   (runner core data-SRAM base used by later RDD_* accessors)' % (v, a))
        elif op == 4:
            flush(); lines.append('SLEEP %d us' % v)
        elif op == 9:
            flush(); lines.append('POLL 0x%08x until (val & 0x%x) == 0x%x' % (a, v, v))
        elif op == 6:
            flush(); lines.append('STOP'); break
        i += 1
    flush()
    open(out + '.txt', 'w').write('\n'.join(lines) + '\n')
    with open(out + '_blockorder.txt', 'w') as f:
        for b, n in blockseq: f.write('%-24s %d writes\n' % (b, n))
    print('tuples', len(tup), 'lines', len(lines), 'register-block runs', len(blockseq))

if __name__ == '__main__':
    main()

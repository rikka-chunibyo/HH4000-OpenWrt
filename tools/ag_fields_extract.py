#!/usr/bin/env python3
"""
ag_fields_extract.py - recover XRDP register bit-field layouts from the
generated ag_drv_* accessors in Broadcom's rdpa.ko, merge them with the RU
register DB (tools/ru_extract.py output) and cross-check against a GPL
XRDP_AG.h (optional).

usage:
  python3 -I tools/ag_fields_extract.py rdpa.ko work/regdb/rdpa.json \
      work/regdb/rdpa_fields [--gpl XRDP_AG.h]

Method (static, no execution):
  * Every accessor reaches its register through a literal-pool pointer to a
    `<BLK>_BLOCK` ru_block_rec (R_AARCH64_ABS64 relocation).  ldr [blk,#8]
    yields the block's address array, ldr [array{,idx}] a block base, and
    ldr/str w,[base,#off] the register at block offset `off`.
  * Getters: the loaded 32-bit value is split with ubfx/and/lsr(/uxtb/uxth)
    - each split is one field (lsb, width).
  * Setters: inputs (struct members ldrb/ldrh/ldr [x0,#k] or w1..w7 args)
    are range-checked with `cmp wN,#(2^w-1)` (VALIDATE_PARMS) and placed
    with lsl/orr(lsl)/bfi/ubfiz; the final str gives the register.
  Field names come from the GPL header when (lsb,width) matches; otherwise
  they are named F<lsb>_<width>.
"""
import sys, json, re, collections
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_REG, ARM64_OP_IMM, ARM64_OP_MEM

def contiguous(mask):
    if mask == 0: return None
    lsb = (mask & -mask).bit_length() - 1
    m = mask >> lsb
    if m & (m + 1): return None
    return lsb, m.bit_length()

class Extractor:
    def __init__(self, path):
        self.f = open(path, 'rb'); self.e = ELFFile(self.f)
        self.text = self.e.get_section_by_name('.text')
        self.code = self.text.data()
        symtab = self.e.get_section_by_name('.symtab')
        self.syms = list(symtab.iter_symbols())
        self.textidx = self.e.get_section_index('.text')
        self.funcs = sorted([(s['st_value'], s['st_size'], s.name) for s in self.syms
                             if s['st_info']['type'] == 'STT_FUNC' and s['st_shndx'] == self.textidx])
        self.rel = {}
        for sec in self.e.iter_sections():
            if isinstance(sec, RelocationSection) and sec.name == '.rela.text':
                st = self.e.get_section(sec['sh_link'])
                for r in sec.iter_relocations():
                    s = st.get_symbol(r['r_info_sym'])
                    self.rel[r['r_offset']] = (s.name, r['r_addend'], r['r_info_type'])
        self.md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); self.md.detail = True

    def regname(self, r): return self.md.reg_name(r)

    @staticmethod
    def norm(r):  # w5 -> x5
        if r is None: return None
        if r.startswith('w') and r[1:].isdigit(): return 'x' + r[1:]
        if r == 'wzr': return 'xzr'
        if r == 'fp': return 'x29'
        if r == 'lr': return 'x30'
        return r

    def analyse(self, start, size):
        st = {}            # reg -> symbolic value
        slots = {}         # stack slot -> value
        cmpw = {}          # src id -> width (from cmp #2^w-1)
        reads = collections.defaultdict(set)   # (blk,off) -> {(lsb,w)}
        writes = collections.defaultdict(set)
        touched = set()
        weak = collections.defaultdict(set)
        code = self.code[start:start + size]
        insns = list(self.md.disasm(code, start))
        def lit_sym(addr):
            r = self.rel.get(addr)
            return r[0] if r else None
        pending_adrp = {}
        for i in insns:
            ops = i.operands
            mn = i.mnemonic
            R = lambda k: self.norm(self.regname(ops[k].reg)) if ops[k].type == ARM64_OP_REG else None
            try:
                if mn == 'ldr' and len(ops) == 2 and ops[1].type == ARM64_OP_IMM:   # literal
                    s = lit_sym(ops[1].imm)
                    d = R(0)
                    if s and s.endswith('_BLOCK'): st[d] = ('blk', s[:-6])
                    else: st.pop(d, None)
                    continue
                if mn == 'adrp':
                    s = self.rel.get(i.address)
                    if s and s[0].endswith('_BLOCK'): st[R(0)] = ('blk', s[0][:-6])
                    else: st.pop(R(0), None)
                    continue
                if mn in ('ldr', 'ldrb', 'ldrh', 'ldrsw') and len(ops) == 2 and ops[1].type == ARM64_OP_MEM:
                    d = R(0); dn = self.regname(ops[0].reg)
                    b = self.norm(self.regname(ops[1].mem.base)); disp = ops[1].mem.disp
                    idx = ops[1].mem.index
                    bv = st.get(b)
                    if b in ('x29', 'sp') and idx == 0:
                        if (b, disp) in slots: st[d] = slots[(b, disp)]
                        else: st.pop(d, None)
                        continue
                    if bv and bv[0] == 'blk' and disp == 8 and idx == 0:
                        st[d] = ('addrs', bv[1]); continue
                    if bv and bv[0] == 'addrs':
                        st[d] = ('ptr', bv[1], 0); continue
                    if bv and bv[0] == 'ptr' and mn == 'ldr' and dn.startswith('w'):
                        ex = disp
                        if idx:
                            iv = st.get(self.norm(self.regname(idx)))
                            ex = iv[1] if (iv and iv[0] == 'const' and not ops[1].shift.value) else 0
                        key = (bv[1], bv[2] + ex)
                        touched.add(key)
                        st[d] = ('val', key, 0); continue
                    if b == 'x0' and bv is None and idx == 0 and mn in ('ldrb', 'ldrh', 'ldr'):
                        st[d] = ('src', 'm%d' % disp, 0, None); continue
                    if bv and bv[0] == 'argp' and idx == 0:
                        st[d] = ('src', 'm%d' % disp, 0, None); continue
                    st.pop(d, None); continue
                if mn in ('str', 'strb', 'strh') and len(ops) == 2 and ops[1].type == ARM64_OP_MEM:
                    v = self.norm(self.regname(ops[0].reg))
                    b = self.norm(self.regname(ops[1].mem.base)); disp = ops[1].mem.disp
                    idx = ops[1].mem.index
                    if b in ('x29', 'sp'):
                        slots[(b, disp)] = st.get(v); continue
                    bv = st.get(b)
                    vv = st.get(v)
                    if vv and vv[0] == 'val' and not (bv and bv[0] == 'ptr'):
                        # getter stores a (possibly lsr-shifted) raw register value to the caller
                        sz = {'strb': 8, 'strh': 16}.get(mn, 32)
                        w = min(32 - vv[2], sz)
                        if vv[2] > 0 or sz < 32: reads[vv[1]].add((vv[2], w))
                        else: weak[vv[1]].add((0, 32))
                        continue
                    if bv and bv[0] == 'ptr' and mn == 'str' and self.regname(ops[0].reg).startswith('w'):
                        key = (bv[1], bv[2] + (disp if idx == 0 else 0))
                        touched.add(key)
                        val = st.get(v)
                        terms = []
                        if val and val[0] == 'expr': terms = val[1]
                        elif val and val[0] == 'src': terms = [(val[1], val[2], val[3])]
                        for (sid, sh, w) in terms:
                            ww = w if w is not None else cmpw.get(sid)
                            if ww is None: ww = 32 - sh
                            writes[key].add((sh, ww))
                    continue
                if mn in ('mov', 'uxtb', 'uxth', 'sxtw') and len(ops) == 2 and ops[1].type == ARM64_OP_REG:
                    d, s = R(0), R(1); sv = st.get(s)
                    if sv is None: st.pop(d, None); continue
                    if sv[0] == 'val' and mn in ('uxtb', 'uxth'):
                        w = 8 if mn == 'uxtb' else 16
                        reads[sv[1]].add((sv[2], w)); st[d] = ('fld',)
                        continue
                    if mn == 'mov' and s == 'x0' and sv is None:
                        pass
                    st[d] = sv; continue
                if mn in ('mov', 'movz') and len(ops) == 2 and ops[1].type == ARM64_OP_IMM:
                    v = ops[1].imm
                    if ops[1].shift.type and ops[1].shift.value: v <<= ops[1].shift.value
                    st[R(0)] = ('const', v & 0xffffffffffffffff); continue
                if mn == 'movk' and ops[1].type == ARM64_OP_IMM:
                    cv = st.get(R(0)); sh = ops[1].shift.value if ops[1].shift.type else 0
                    if cv and cv[0] == 'const':
                        st[R(0)] = ('const', (cv[1] & ~(0xffff << sh)) | (ops[1].imm << sh))
                    else: st.pop(R(0), None)
                    continue
                if mn == 'ubfx':
                    d, s = R(0), R(1); lsb, w = ops[2].imm, ops[3].imm
                    sv = st.get(s)
                    if sv and sv[0] == 'val':
                        reads[sv[1]].add((sv[2] + lsb, w)); st[d] = ('fld',); continue
                    if sv and sv[0] == 'src':
                        st[d] = ('src', sv[1], sv[2], w); continue
                    st.pop(d, None); continue
                if mn == 'and' and len(ops) == 3 and ops[2].type == ARM64_OP_IMM:
                    d, s = R(0), R(1); m = ops[2].imm & 0xffffffff
                    sv = st.get(s); c = contiguous(m)
                    inv = contiguous((~m) & 0xffffffff)
                    if sv and sv[0] == 'val' and (m & 0x80000000) and inv and inv[0] + inv[1] < 32 and sv[2] == 0:
                        st[d] = ('expr', [], [inv]); continue      # RMW clear of field ~m
                    if sv and sv[0] == 'val' and c:
                        reads[sv[1]].add((sv[2] + c[0], c[1])); st[d] = ('fld',); continue
                    if sv and sv[0] == 'src' and c:
                        if c[0] == sv[2]: st[d] = ('src', sv[1], sv[2], c[1])
                        else: st[d] = ('src', sv[1], sv[2], c[0] + c[1] - sv[2])
                        continue
                    if sv and sv[0] == 'expr' and c:
                        st[d] = sv; continue
                    st.pop(d, None); continue
                if mn == 'lsr' and ops[2].type == ARM64_OP_IMM:
                    d, s = R(0), R(1); sv = st.get(s); sh = ops[2].imm
                    if sv and sv[0] == 'val':
                        st[d] = ('val', sv[1], sv[2] + sh)
                        continue
                    st.pop(d, None); continue
                if mn in ('tst',) :
                    continue
                if mn == 'cmp' and len(ops) == 2 and ops[1].type == ARM64_OP_IMM:
                    s = R(0); sv = st.get(s); imm = ops[1].imm
                    if sv and sv[0] == 'src' and imm and (imm & (imm + 1)) == 0:
                        cmpw[sv[1]] = imm.bit_length()
                    continue
                if mn == 'lsl' and ops[2].type == ARM64_OP_IMM:
                    d, s = R(0), R(1); sv = st.get(s); sh = ops[2].imm
                    if sv and sv[0] == 'src':
                        st[d] = ('src', sv[1], sv[2] + sh, sv[3]); continue
                    if sv and sv[0] == 'expr':
                        st[d] = ('expr', [(a, b + sh, c) for a, b, c in sv[1]], sv[2] if len(sv) > 2 else []); continue
                    st.pop(d, None); continue
                if mn == 'ubfiz':
                    d, s = R(0), R(1); sv = st.get(s)
                    if sv and sv[0] == 'src':
                        st[d] = ('src', sv[1], ops[2].imm, ops[3].imm); continue
                    st.pop(d, None); continue
                if mn == 'orr' and len(ops) == 3 and ops[2].type == ARM64_OP_REG:
                    d, a, b = R(0), R(1), R(2)
                    sh = 0
                    if ops[2].shift.type and ops[2].shift.value: sh = ops[2].shift.value
                    def terms(v, extra=0):
                        if v is None: return []
                        if v[0] == 'src': return [(v[1], v[2] + extra, v[3])]
                        if v[0] == 'expr': return [(x, y + extra, z) for x, y, z in v[1]]
                        return []
                    t = terms(st.get(a)) + terms(st.get(b), sh)
                    cl = []
                    for v_ in (st.get(a), st.get(b)):
                        if v_ and v_[0] == 'expr' and len(v_) > 2: cl += v_[2]
                    # a source OR-ed into a cleared hole takes the hole width
                    t = [(x, y, z if z is not None else next((w_ for l_, w_ in cl if l_ == y), None)) for x, y, z in t]
                    if t or cl: st[d] = ('expr', t, cl)
                    else: st.pop(d, None)
                    continue
                if mn == 'bfi':
                    d, s = R(0), R(1); lsb, w = ops[2].imm, ops[3].imm
                    dv, sv = st.get(d), st.get(s)
                    t = []
                    if dv and dv[0] == 'src': t = [(dv[1], dv[2], dv[3])]
                    elif dv and dv[0] == 'expr': t = list(dv[1])
                    if sv and sv[0] == 'src': t.append((sv[1], lsb, w))
                    st[d] = ('expr', t) if t else None
                    continue
                if mn == 'add' and len(ops) == 3:
                    d, s = R(0), R(1); sv = st.get(s)
                    if sv and sv[0] == 'ptr':
                        if ops[2].type == ARM64_OP_IMM:
                            im = ops[2].imm
                            if ops[2].shift.type and ops[2].shift.value: im <<= ops[2].shift.value
                            st[d] = ('ptr', sv[1], sv[2] + im)
                        else:
                            ov = st.get(R(2))
                            if ov and ov[0] == 'const' and not (ops[2].shift.type and ops[2].shift.value) and ops[2].ext == 0:
                                st[d] = ('ptr', sv[1], sv[2] + ov[1])
                            else: st[d] = ('ptr', sv[1], sv[2])
                        continue
                    if ops[2].type == ARM64_OP_REG:
                        ov = st.get(R(2))
                        if ov and ov[0] == 'ptr':
                            cv = sv
                            st[d] = ('ptr', ov[1], ov[2] + (cv[1] if cv and cv[0] == 'const' else 0)); continue
                    st.pop(d, None); continue
                if mn in ('bl', 'blr'):
                    for k in list(st):
                        if k in ('x%d' % n for n in range(19)): st.pop(k, None)
                    continue
                # default: clobber destination
                if ops and ops[0].type == ARM64_OP_REG and mn not in ('cbz', 'cbnz', 'tbz', 'tbnz', 'b', 'ret', 'stp', 'ldp', 'cmp', 'cmn', 'ccmp'):
                    st.pop(R(0), None)
            except Exception:
                if ops and ops[0].type == ARM64_OP_REG: st.pop(R(0), None)
        for k, v in weak.items():
            if k not in reads: reads[k] |= v
        return reads, writes, touched

def load_gpl(path):
    txt = open(path).read()
    fields = collections.defaultdict(list)
    regs = {}
    for m in re.finditer(r'#define (\w+)_REG_OFFSET (0x[0-9a-fA-F]+)', txt):
        regs[m.group(1)] = int(m.group(2), 16)
    for m in re.finditer(r'#define (\w+)_FIELD_MASK\s+(0x[0-9a-fA-F]+)\n#define \1_FIELD_WIDTH (\d+)\n#define \1_FIELD_SHIFT (\d+)', txt):
        full, mask, w, sh = m.group(1), int(m.group(2), 16), int(m.group(3)), int(m.group(4))
        fields[full].append((sh, w))
    # attribute FIELD full names to registers: longest register-name prefix
    byreg = collections.defaultdict(list)
    names = sorted(regs, key=len, reverse=True)
    for full in fields:
        for rn in names:
            if full.startswith(rn + '_'):
                for sh, w in fields[full]:
                    byreg[rn].append((full[len(rn) + 1:], sh, w))
                break
    return regs, byreg

def main():
    ko, dbp, outp = sys.argv[1:4]
    gpl = sys.argv[sys.argv.index('--gpl') + 1] if '--gpl' in sys.argv else None
    db = json.load(open(dbp))
    X = Extractor(ko)
    reads = collections.defaultdict(set); writes = collections.defaultdict(set)
    fnmap = collections.defaultdict(set); nfun = 0
    for addr, size, name in X.funcs:
        if not name.startswith('ag_drv_') or size == 0: continue
        nfun += 1
        r, w, t = X.analyse(addr, size)
        for k, v in r.items(): reads[k] |= v
        for k, v in w.items(): writes[k] |= v
        for k in t: fnmap[k].add(name)
    # register lookup: block name -> offset -> reg
    regindex = {}
    for b in db:
        for r in b['regs']:
            regindex[(b['name'], r['addr'])] = r
    gregs, gfields = load_gpl(gpl) if gpl else ({}, {})
    stats = collections.Counter()
    mm_full = []
    out = []
    hdr = ['/* XRDP (BCM6858) register fields.  Offsets: rdpa.ko RU tables (tools/ru_extract.py).',
           ' * Fields: derived from rdpa.ko ag_drv_* accessors (tools/ag_fields_extract.py)',
           ' * and named/cross-checked with the GPL BCM6858 XRDP_AG.h (SDK 5.04, same 969-register DB).',
           ' * src: A=accessor only, G=GPL only, AG=both agree. */', '']
    for b in db:
        ob = {'name': b['name'], 'bases': b['bases'], 'regs': []}
        hdr.append('/* ===== BLOCK %s  bases: %s */' % (b['name'], ' '.join('0x%08x' % x for x in b['bases'])))
        for r in b['regs']:
            key = (b['name'], r['addr'])
            acc = set()
            for (lsb, w) in reads.get(key, set()) | writes.get(key, set()):
                if 0 <= lsb < 32 and 0 < w <= 32 - lsb: acc.add((lsb, w))
            # drop tentative 'top field' lsr entries that are supersets of a finer split
            acc2 = set()
            for (lsb, w) in acc:
                if lsb + w == 32 and any(l2 == lsb and w2 < w for l2, w2 in acc): continue
                acc2.add((lsb, w))
            acc = acc2
            full = b['name'] + '_' + r['name']
            g = [(n, sh, w) for (n, sh, w) in gfields.get(full, []) if not n.startswith('RESERVED')]
            gset = {(sh, w) for _, sh, w in g}
            fields = []
            for n, sh, w in sorted(g, key=lambda x: x[1]):
                src = 'AG' if (sh, w) in acc else 'G'
                fields.append({'name': n, 'lsb': sh, 'width': w, 'mask': ((1 << w) - 1) << sh, 'src': src})
            def overlaps(l, w):
                return any(not (l + w <= sh or sh + gw <= l) for _, sh, gw in g)
            for (lsb, w) in sorted(acc - gset):
                if overlaps(lsb, w): stats['accessor_fields_overlapping_gpl_dropped'] += 1; continue
                fields.append({'name': 'F%d_%d' % (lsb, w), 'lsb': lsb, 'width': w,
                               'mask': ((1 << w) - 1) << lsb, 'src': 'A'})
            fields.sort(key=lambda f: f['lsb'])
            stats['regs'] += 1
            if acc: stats['regs_with_accessor_fields'] += 1
            if key in fnmap: stats['regs_touched_by_accessor'] += 1
            if g: stats['regs_with_gpl_fields'] += 1
            if g and acc:
                stats['regs_compared'] += 1
                if acc == gset: stats['regs_exact_match'] += 1
                elif acc <= gset: stats['regs_accessor_subset_of_gpl'] += 1
                else:
                    stats['regs_mismatch'] += 1
                    mm_full.append('%s acc=%s gpl=%s' % (full, sorted(acc - gset), sorted((sh, w, n) for n, sh, w in g)))
            for f in fields: stats['fields_' + f['src']] += 1
            orr = dict(r); orr['fields'] = fields; orr['accessors'] = sorted(fnmap.get(key, []))
            ob['regs'].append(orr)
            ram = ' RAM[%d] stride 0x%x' % (r['ram_count'] + 1, r['stride']) if r['ram_count'] else ''
            hdr.append('#define %s_OFFSET 0x%04x /*%s */' % (full, r['addr'], ram))
            for f in fields:
                hdr.append('#define   %s_%s_MASK 0x%08x /* %d @%d %s */' % (full, f['name'], f['mask'], f['width'], f['lsb'], f['src']))
        out.append(ob)
        hdr.append('')
    json.dump({'blocks': out, 'stats': stats, 'n_accessors': nfun}, open(outp + '.json', 'w'), indent=1)
    open(outp + '.h', 'w').write('\n'.join(hdr) + '\n')
    print('accessor functions analysed:', nfun)
    for k, v in sorted(stats.items()): print('%-34s %d' % (k, v))
    # mismatch listing for review
    mm = ['# registers whose accessor-derived (lsb,width) set is not a subset of the GPL field set',
          '# acc = accessor-only (lsb,width) pairs; gpl = (lsb,width,name) from XRDP_AG.h'] + mm_full
    for b in []:
        for r in b['regs']:
            a = {(f['lsb'], f['width']) for f in r['fields'] if f['src'] in ('A', 'AG')}
            gg = {(f['lsb'], f['width']) for f in r['fields'] if f['src'] in ('G', 'AG')}
            if a and gg and not a <= gg:
                mm.append('%s_%s acc=%s gpl=%s' % (b['name'], r['name'], sorted(a - gg), sorted(gg)))
    open(outp + '_mismatch.txt', 'w').write('\n'.join(mm) + '\n')

if __name__ == '__main__':
    main()

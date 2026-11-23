#!/usr/bin/env python3
"""Extract Broadcom RU (register utility) databases from relocatable .ko files.

Three record layouts occur in the HH4000 (BCA 5.02L.07, aarch64) modules, told apart
by the symbol sizes of the *_REG records:

  REG 0x38 / FIELD 0x20  (gponstack, ngponstack, bcmepon): RU_INCLUDE_ACCESS + FIELD_DB
  REG 0x48 / FIELD 0x30  (bcm_pondrv): also RU_INCLUDE_DESC (title*, desc* after name*)
  REG 0x20, no FIELDs    (rdpa): name*, addr, ram_count, offset, log_idx/size only

  ru_block_rec (0x20): name*, addr* (u64 base[]), u8 addr_count, u32 reg_count, regs**
  ru_reg_rec        : name*, [title*, desc*], u64 addr, u32 ram_count, u32 offset (stride),
                      u32 log_idx, u32 access, u32 field_count, fields**, u32 reg_size
  ru_field_rec      : name*, [title*, desc*], u64 mask, u32 bits, u32 shift, u32 access

Pointers are resolved by applying the module's RELA relocations.
Usage: ru_extract.py module.ko outprefix   -> outprefix.h, outprefix.json
"""
import sys, struct, json
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

ko, out = sys.argv[1], sys.argv[2]
f = open(ko, "rb")
elf = ELFFile(f)
data = {}
for i, s in enumerate(elf.iter_sections()):
    if s["sh_type"] != "SHT_NOBITS":
        data[i] = bytearray(s.data())
syms = list(elf.get_section_by_name(".symtab").iter_symbols())

reloc = {}
for s in elf.iter_sections():
    if not isinstance(s, RelocationSection):
        continue
    tgt = s["sh_info"]
    for r in s.iter_relocations():
        sym = syms[r["r_info_sym"]]
        if sym["st_shndx"] in ("SHN_UNDEF", "SHN_ABS", "SHN_COMMON"):
            continue
        reloc[(tgt, r["r_offset"])] = (sym["st_shndx"], sym["st_value"] + r["r_addend"])


def u32(si, o):
    return struct.unpack_from("<I", data[si], o)[0]


def u64(si, o):
    return struct.unpack_from("<Q", data[si], o)[0]


def ptr(si, o):
    return reloc.get((si, o))


def cstr(p):
    if not p or p[0] not in data:
        return None
    si, o = p
    b = data[si]
    e = b.index(0, o)
    return b[o:e].decode("latin1")


regsz = {sy["st_size"] for sy in syms
         if sy.name.endswith("_REG") and sy["st_info"]["type"] == "STT_OBJECT"}
DESC = 0x48 in regsz
NOFDB = 0x20 in regsz
D = 16 if DESC else 0

blocks = []
for sym in syms:
    n = sym.name
    if not n.endswith("_BLOCK") or sym["st_size"] != 0x20 or sym["st_info"]["type"] != "STT_OBJECT":
        continue
    si, o = sym["st_shndx"], sym["st_value"]
    bname = cstr(ptr(si, o)) or n[:-6]
    ap = ptr(si, o + 8)
    cnt = data[si][o + 16]
    rc = u32(si, o + 20)
    rp = ptr(si, o + 24)
    bases = [u64(ap[0], ap[1] + 8 * i) for i in range(cnt)] if ap and ap[0] in data else []
    regs = []
    for i in range(rc if rp else 0):
        p = ptr(rp[0], rp[1] + 8 * i)
        if not p:
            continue
        rs, ro = p
        if NOFDB:
            regs.append(dict(name=cstr(ptr(rs, ro)), addr=u64(rs, ro + 8), ram_count=u32(rs, ro + 16),
                             stride=u32(rs, ro + 20), access=0, fields=[]))
            continue
        reg = dict(name=cstr(ptr(rs, ro)), addr=u64(rs, ro + 8 + D), ram_count=u32(rs, ro + 16 + D),
                   stride=u32(rs, ro + 20 + D), access=u32(rs, ro + 28 + D), fields=[])
        if DESC:
            reg["title"] = cstr(ptr(rs, ro + 8))
            reg["desc"] = cstr(ptr(rs, ro + 16))
        fc = u32(rs, ro + 32 + D)
        fp = ptr(rs, ro + 40 + D)
        for j in range(fc if fp else 0):
            q = ptr(fp[0], fp[1] + 8 * j)
            if not q:
                continue
            fs, fo = q
            fd = dict(name=cstr(ptr(fs, fo)), mask=u64(fs, fo + 8 + D), bits=u32(fs, fo + 16 + D),
                      shift=u32(fs, fo + 20 + D), access=u32(fs, fo + 24 + D))
            if DESC:
                fd["title"] = cstr(ptr(fs, fo + 8))
                fd["desc"] = cstr(ptr(fs, fo + 16))
            reg["fields"].append(fd)
        regs.append(reg)
    blocks.append(dict(symbol=n, name=bname, bases=bases, regs=regs))

blocks.sort(key=lambda b: (b["bases"][0] if b["bases"] else 1 << 63, b["name"]))
json.dump(blocks, open(out + ".json", "w"), indent=1)
acc = {0: "", 1: "RO", 2: "WO", 3: "RW"}


def c(t):
    return (t or "").replace("*/", "* /").replace("\n", " ")


with open(out + ".h", "w") as h:
    h.write("/* Auto-extracted from %s RU register database (tools/ru_extract.py).\n"
            " * Register offsets are block-relative; add a block base. */\n" % ko.split("/")[-1])
    for b in blocks:
        h.write("\n/* ===== BLOCK %s  bases: %s */\n"
                % (b["name"], ", ".join("0x%x" % x for x in b["bases"]) or "(runtime)"))
        for r in b["regs"]:
            ram = " RAM[%d] stride 0x%x" % (r["ram_count"] + 1, r["stride"]) if r["ram_count"] else ""
            t = (" - " + c(r["title"])) if r.get("title") else ""
            h.write("#define %s_%s_OFFSET 0x%04x /* %s%s%s */\n"
                    % (b["name"], r["name"], r["addr"], acc.get(r["access"], "?"), ram, t))
            for fl in r["fields"]:
                t = (" - " + c(fl["title"])) if fl.get("title") else ""
                h.write("#define   %s_%s_%s_MASK 0x%08x /* bits %d @%d%s */\n"
                        % (b["name"], r["name"], fl["name"], fl["mask"], fl["bits"], fl["shift"], t))

nr = sum(len(b["regs"]) for b in blocks)
nf = sum(len(r["fields"]) for b in blocks for r in b["regs"])
print("%s: layout=%s %d blocks, %d registers, %d fields"
      % (ko.split("/")[-1], "desc" if DESC else ("nofdb" if NOFDB else "std"), len(blocks), nr, nf))

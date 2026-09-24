#include "mango/decoder.h"

#include "mango/cpu.h"

static void mango_insn_clear(MangoInsn* out) {
  out->rd = out->rn = out->rm = out->rs = out->imm = out->reglist = 0;
  out->shift_type = 0;
  out->shift_amount = 0;
  out->is_imm = 0;
  out->shift_by_reg = 0;
  out->sets_flags = 0;
  out->u = 0;
  out->b = 0;
  out->p = 0;
  out->w = 0;
  out->op = MANGO_OP_UNKNOWN;
}

/* NEON VMOV.I* / VMOV.F32 / VMVN imm8 → 64-bit D-lane pattern (repeated across Q).
 * op==1 && cmode==0xE is VMOV.I64 byte-mask (not VMVN). Other op==1 cmodes expand as
 * VMOV (op=0) then bitwise-NOT the 64-bit pattern. */
static int mango_neon_expand_imm(uint32_t cmode, uint32_t op, uint32_t imm8, uint64_t* out) {
  if (op == 1 && cmode == 0xEu) {
    uint64_t p = 0;
    for (uint32_t i = 0; i < 8u; i++) {
      if (imm8 & (1u << i)) {
        p |= 0xFFull << (8u * i);
      }
    }
    *out = p;
    return 0;
  }
  /* VMVN: expand like VMOV then invert. */
  uint32_t eop = (op == 1u) ? 0u : op;
  int invert = (op == 1u);
  if (eop == 0 && (cmode & 1u) == 0 && cmode <= 0x6u) {
    uint32_t w = imm8 << ((cmode >> 1) * 8u);
    *out = (uint64_t)w | ((uint64_t)w << 32);
  } else if (eop == 0 && cmode == 0x8u) {
    uint32_t w = imm8 | (imm8 << 16);
    *out = (uint64_t)w | ((uint64_t)w << 32);
  } else if (eop == 0 && cmode == 0xAu) {
    uint32_t w = (imm8 << 8) | (imm8 << 24);
    *out = (uint64_t)w | ((uint64_t)w << 32);
  } else if (eop == 0 && cmode == 0xCu) {
    /* I32: each lane = imm8 | (imm8 << 8) in bits[15:0] (e.g. #0xffff). */
    uint32_t w = imm8 | (imm8 << 8);
    *out = (uint64_t)w | ((uint64_t)w << 32);
  } else if (eop == 0 && cmode == 0xEu) {
    uint64_t p = 0;
    for (uint32_t i = 0; i < 8u; i++) {
      p |= (uint64_t)imm8 << (8u * i);
    }
    *out = p;
  } else if (eop == 0 && cmode == 0xFu) {
    uint32_t b = (imm8 >> 6) & 1u;
    uint32_t w = ((imm8 & 0x80u) << 24) | ((1u - b) << 30) | (b ? 0x3E000000u : 0) |
                 ((imm8 & 0x3Fu) << 19);
    *out = (uint64_t)w | ((uint64_t)w << 32);
  } else {
    return -1;
  }
  if (invert) {
    *out = ~*out;
  }
  return 0;
}


/* ThumbExpandImm (modified immediate): Arm ARM Immediate constants in A32/T32.
 * imm12 = i:imm3:imm8. Expand at decode into out->imm (like A32 imm8/rot).
 * Returns -1 for architecturally UNPREDICTABLE encodings (copy forms with imm8=0). */
static int mango_thumb_expand_imm(uint32_t imm12, uint32_t* out) {
  imm12 &= 0xFFFu;
  if (((imm12 >> 10) & 3u) == 0u) {
    uint32_t imm8 = imm12 & 0xFFu;
    switch ((imm12 >> 8) & 3u) {
      case 0:
        *out = imm8;
        return 0;
      case 1:
        if (imm8 == 0u) {
          return -1;
        }
        *out = (imm8 << 16) | imm8;
        return 0;
      case 2:
        if (imm8 == 0u) {
          return -1;
        }
        *out = (imm8 << 24) | (imm8 << 8);
        return 0;
      default: /* case 3 */
        if (imm8 == 0u) {
          return -1;
        }
        *out = (imm8 << 24) | (imm8 << 16) | (imm8 << 8) | imm8;
        return 0;
    }
  }
  {
    uint32_t unrot = 0x80u | (imm12 & 0x7Fu);
    uint32_t rot = (imm12 >> 7) & 0x1Fu;
    if (rot == 0u) {
      *out = unrot;
    } else {
      *out = (unrot >> rot) | (unrot << (32u - rot));
    }
    return 0;
  }
}

int mango_decode(uint32_t word, MangoInsn* out) {
  mango_insn_clear(out);
  out->cond = (word >> 28) & 0xF;

  if (out->cond == 0xF) {
    /* Advanced SIMD data-processing: 1111 001x */
    if (((word >> 25) & 0x7) == 0x1) {
      /* VMOV immediate: 1111 001 i 1 D 000 imm3 Vd cmode 0 Q op 1 imm4 */
      /* Three registers of the same type: bit23=0, bit4=0. VADD/VSUB integer. */
      if (((word >> 23) & 1u) == 0 && ((word >> 4) & 1u) == 0) {
        uint32_t u = (word >> 24) & 1u;
        uint32_t size = (word >> 20) & 3u;
        uint32_t opc = (word >> 8) & 0xFu;
        uint32_t q = (word >> 6) & 1u;
        uint32_t d = (((word >> 22) & 1u) << 4) | ((word >> 12) & 0xFu);
        uint32_t n = (((word >> 7) & 1u) << 4) | ((word >> 16) & 0xFu);
        uint32_t m = (((word >> 5) & 1u) << 4) | (word & 0xFu);
        if (q && ((d | n | m) & 1u)) {
          return -1;
        }
        if (opc == 0x8u && size != 3u) {
          out->op = u ? MANGO_OP_VSUBI : MANGO_OP_VADDI;
          out->cond = 0xE;
          out->rd = d;
          out->rn = n;
          out->rm = m;
          out->b = (int)q;
          out->imm = 1u << size;
          return 0;
        }
        return -1;
      }
      /* Three-reg same length with o1=1 (bit4): VORR / VRECPS / VMUL.F32. */
      if (((word >> 23) & 1u) == 0 && ((word >> 4) & 1u) == 1) {
        uint32_t u = (word >> 24) & 1u;
        uint32_t size = (word >> 20) & 3u;
        uint32_t opc = (word >> 8) & 0xFu;
        uint32_t q = (word >> 6) & 1u;
        uint32_t d = (((word >> 22) & 1u) << 4) | ((word >> 12) & 0xFu);
        uint32_t n = (((word >> 7) & 1u) << 4) | ((word >> 16) & 0xFu);
        uint32_t m = (((word >> 5) & 1u) << 4) | (word & 0xFu);
        if (q && ((d | n | m) & 1u)) {
          return -1;
        }
        /* VORR (register): U=0, opc=0001, size!=11. */
        if (u == 0u && opc == 0x1u && size != 3u) {
          out->op = MANGO_OP_VORR;
          out->cond = 0xE;
          out->rd = d;
          out->rn = n;
          out->rm = m;
          out->b = (int)q;
          return 0;
        }
        /* VRECPS.F32: U=0, size=0sz with sz=0 (F32), opc=1111, o1=1. */
        if (u == 0u && size == 0u && opc == 0xFu) {
          out->op = MANGO_OP_VRECPS;
          out->cond = 0xE;
          out->rd = d;
          out->rn = n;
          out->rm = m;
          out->b = (int)q;
          out->imm = 4; /* F32 lane size in bytes */
          return 0;
        }
        /* VMUL.F32 AdvSIMD: U=1, size=0sz sz=0, opc=1101, o1=1.
         * Reuses MANGO_OP_VMUL with u=1 so VFP Sd/Dd (u=0) stays intact. */
        if (u == 1u && size == 0u && opc == 0xDu) {
          out->op = MANGO_OP_VMUL;
          out->cond = 0xE;
          out->rd = d;
          out->rn = n;
          out->rm = m;
          out->b = (int)q;
          out->u = 1; /* AdvSIMD D/Q f32 lanes */
          out->imm = 4;
          return 0;
        }
        return -1;
      }
      if (((word >> 23) & 1u) == 1 && ((word >> 19) & 7u) == 0 && ((word >> 7) & 1u) == 0 &&
          ((word >> 4) & 1u) == 1) {
        uint32_t imm8 =
            (((word >> 24) & 1u) << 7) | (((word >> 16) & 7u) << 4) | (word & 0xFu);
        uint32_t cmode = (word >> 8) & 0xFu;
        uint32_t opbit = (word >> 5) & 1u;
        uint32_t q = (word >> 6) & 1u;
        uint32_t d = (((word >> 22) & 1u) << 4) | ((word >> 12) & 0xFu);
        uint64_t pat;
        if (q && (d & 1u)) {
          return -1;
        }
        if (mango_neon_expand_imm(cmode, opbit, imm8, &pat) != 0) {
          return -1;
        }
        out->op = MANGO_OP_VMOV;
        out->cond = 0xE;
        out->u = 3;
        out->rd = d;
        out->b = (int)q;
        out->imm = (uint32_t)pat;
        out->rs = (uint32_t)(pat >> 32);
        return 0;
      }
      /* VEXT: 1111 0010 1 D 11 Vn Vd imm4 N Q M 0 Vm (bit24=0, bit4=0). */
      if (((word >> 23) & 1u) == 1 && ((word >> 20) & 3u) == 3u && ((word >> 24) & 1u) == 0 &&
          ((word >> 4) & 1u) == 0) {
        uint32_t q = (word >> 6) & 1u;
        uint32_t imm4 = (word >> 8) & 0xFu;
        uint32_t d = (((word >> 22) & 1u) << 4) | ((word >> 12) & 0xFu);
        uint32_t n = (((word >> 7) & 1u) << 4) | ((word >> 16) & 0xFu);
        uint32_t m = (((word >> 5) & 1u) << 4) | (word & 0xFu);
        if (q && ((d | n | m) & 1u)) {
          return -1;
        }
        if ((!q && imm4 >= 8u) || (q && imm4 >= 16u)) {
          return -1;
        }
        out->op = MANGO_OP_VEXT;
        out->cond = 0xE;
        out->rd = d;
        out->rn = n;
        out->rm = m;
        out->b = (int)q;
        out->imm = imm4; /* byte offset */
        return 0;
      }
      /* VSWP: 1111 0011 1 D 11 size=00 opc1=10 Vd 00000 Q M 0 Vm. */
      if (((word >> 23) & 1u) == 1 && ((word >> 20) & 3u) == 3u && ((word >> 24) & 1u) == 1 &&
          ((word >> 16) & 0xFu) == 0x2u && ((word >> 7) & 0x1Fu) == 0 && ((word >> 4) & 1u) == 0) {
        uint32_t q = (word >> 6) & 1u;
        uint32_t d = (((word >> 22) & 1u) << 4) | ((word >> 12) & 0xFu);
        uint32_t m = (((word >> 5) & 1u) << 4) | (word & 0xFu);
        if (q && ((d | m) & 1u)) {
          return -1;
        }
        out->op = MANGO_OP_VSWP;
        out->cond = 0xE;
        out->rd = d;
        out->rm = m;
        out->b = (int)q;
        return 0;
      }
      /* VRECPE: 1111 0011 1 D 11 size 11 Vd 0 10 F 0 Q M 0 Vm; F=1 size=10 → F32. */
      if (((word >> 23) & 1u) == 1 && ((word >> 20) & 3u) == 3u && ((word >> 24) & 1u) == 1 &&
          ((word >> 16) & 3u) == 3u && ((word >> 18) & 3u) == 2u && ((word >> 8) & 0xFu) == 0x5u &&
          ((word >> 7) & 1u) == 0 && ((word >> 4) & 1u) == 0) {
        uint32_t q = (word >> 6) & 1u;
        uint32_t d = (((word >> 22) & 1u) << 4) | ((word >> 12) & 0xFu);
        uint32_t m = (((word >> 5) & 1u) << 4) | (word & 0xFu);
        if (q && ((d | m) & 1u)) {
          return -1;
        }
        out->op = MANGO_OP_VRECPE;
        out->cond = 0xE;
        out->rd = d;
        out->rm = m;
        out->b = (int)q;
        out->imm = 4; /* F32 lane size in bytes */
        return 0;
      }
      return -1;
    }
    /* Advanced SIMD element/structure load/store: 1111 010x */
    if (((word >> 25) & 0x7) == 0x2) {
      /* VLD1/VST1 multiple: 1111 0100 0 D L 0 Rn Vd type size align Rm */
      if (((word >> 23) & 1u) == 0 && ((word >> 20) & 1u) == 0) {
        uint32_t l = (word >> 21) & 1u;
        uint32_t type = (word >> 8) & 0xFu;
        uint32_t nd;
        uint32_t d;
        switch (type) {
          case 0x7:
            nd = 1;
            break;
          case 0xA:
            nd = 2;
            break;
          case 0x6:
            nd = 3;
            break;
          case 0x2:
            nd = 4;
            break;
          default:
            return -1;
        }
        d = (((word >> 22) & 1u) << 4) | ((word >> 12) & 0xFu);
        if (d + nd > 32u) {
          return -1;
        }
        out->op = l ? MANGO_OP_VLD1 : MANGO_OP_VST1;
        out->cond = 0xE;
        out->rd = d;
        out->rn = (word >> 16) & 0xFu;
        out->rm = word & 0xFu;
        out->imm = nd;
        return 0;
      }
    }
    /* PLD/PLDW/PLI */
    if (((word >> 24) & 0xFEu) == 0xF4u && ((word >> 12) & 0xFu) == 0xFu) {
      out->op = MANGO_OP_NOP;
      out->cond = 0xE;
      return 0;
    }
    /* DMB/DSB/ISB/CLREX */
    if ((word & 0xFFFFFFF0u) == 0xF57FF050u || (word & 0xFFFFFFF0u) == 0xF57FF040u ||
        (word & 0xFFFFFFF0u) == 0xF57FF060u || word == 0xF57FF01Fu) {
      out->op = MANGO_OP_NOP;
      out->cond = 0xE;
      return 0;
    }
    /* BLX imm: 1111 101 H imm24 */
    if (((word >> 25) & 0x7) == 0x5) {
      uint32_t h = (word >> 24) & 1u;
      uint32_t imm24 = word & 0xFFFFFFu;
      uint32_t offset = (imm24 << 2) | (h << 1);
      if (imm24 & 0x800000u) {
        offset |= 0xFC000000u;
      }
      out->op = MANGO_OP_BLX;
      out->cond = 0xE;
      out->is_imm = 1;
      out->imm = offset;
      return 0;
    }
    return -1;
  }

  /* SVC/SWI: bits 27-24 = 1111, the rest is a legacy immediate EABI code
   * ignores; the actual syscall number is in r7 at execution time, not
   * decoded here, see mango_interp_run's SVC case in interp.c. */
  if (((word >> 24) & 0xF) == 0xF) {
    out->op = MANGO_OP_SVC;
    return 0;
  }

  /* VFP VLDR/VSTR: coproc 1010 (single) or 1011 (double), bits 27-25=110. */
  if (((word >> 25) & 0x7) == 0x6 && (((word >> 8) & 0xF) == 0xA || ((word >> 8) & 0xF) == 0xB)) {
    uint32_t p = (word >> 24) & 1u;
    uint32_t u = (word >> 23) & 1u;
    uint32_t dbit = (word >> 22) & 1u;
    uint32_t w = (word >> 21) & 1u;
    uint32_t l = (word >> 20) & 1u;
    uint32_t rn = (word >> 16) & 0xF;
    uint32_t vd = (word >> 12) & 0xF;
    uint32_t cp = (word >> 8) & 0xF;
    uint32_t imm8 = word & 0xFFu;
    int dbl = cp == 0xB;
    if (!p && !w && dbl) {
      /* VMOV between a double and two GPRs (Rt, Rt2). */
      out->op = MANGO_OP_VMOV;
      out->rd = vd;
      out->rn = rn;
      out->rm = (dbit << 4) | (word & 0xFu);
      if ((word >> 5) & 1u) {
        out->rm |= 16u;
      }
      out->b = 1;
      out->u = l ? 1 : 2;
      return 0;
    }
    if (!p || w) {
      /* VLDM/VSTM / VPUSH/VPOP: IA (P=0 U=1) or DB (P=1 U=0). */
      uint32_t first = dbl ? ((dbit << 4) | vd) : ((vd << 1) | dbit);
      uint32_t nregs = dbl ? (imm8 / 2u) : imm8;
      if (nregs == 0 || (dbl && (imm8 & 1u)) || first + nregs > 32u) {
        return -1;
      }
      out->op = l ? MANGO_OP_VLDM : MANGO_OP_VSTM;
      out->rn = rn;
      out->rd = first;
      out->b = dbl;
      out->imm = nregs;
      out->u = (int)u;
      out->p = (int)p;
      out->w = (int)w;
      return 0;
    }
    out->op = l ? MANGO_OP_VLDR : MANGO_OP_VSTR;
    out->rn = rn;
    out->rd = dbl ? ((dbit << 4) | vd) : ((vd << 1) | dbit);
    out->b = dbl;
    out->is_imm = 1;
    out->imm = imm8 << 2;
    out->u = (int)u;
    out->p = 1;
    return 0;
  }

  /* VFP data-processing, coproc 1010/1011, bits 27-24=1110. */
  if (((word >> 24) & 0xF) == 0xE && (((word >> 8) & 0xF) == 0xA || ((word >> 8) & 0xF) == 0xB)) {
    uint32_t opc = (word >> 20) & 0xF;
    uint32_t dbit = (word >> 22) & 1u;
    uint32_t nbit = (word >> 7) & 1u;
    uint32_t mbit = (word >> 5) & 1u;
    uint32_t vd = (word >> 12) & 0xF;
    uint32_t vn = (word >> 16) & 0xF;
    uint32_t vm = word & 0xF;
    uint32_t cp = (word >> 8) & 0xF;
    int dbl = cp == 0xB;
    uint32_t fd = dbl ? ((dbit << 4) | vd) : ((vd << 1) | dbit);
    uint32_t fn = dbl ? ((nbit << 4) | vn) : ((vn << 1) | nbit);
    uint32_t fm = dbl ? ((mbit << 4) | vm) : ((vm << 1) | mbit);
    out->b = dbl;
    out->rd = fd;
    out->rn = fn;
    out->rm = fm;
    /* VMOV Sn, Rt / Rt, Sn: coproc 1010, bit4=1, bits 23-21=000. */
    if (!dbl && ((word >> 4) & 1u) && ((word >> 21) & 7u) == 0) {
      uint32_t rt = vd;
      uint32_t sn = (vn << 1) | nbit;
      if (rt == MANGO_REG_PC || sn >= 32u) {
        return -1;
      }
      out->op = MANGO_OP_VMOV;
      out->u = 4;
      out->rd = sn;
      out->rn = rt;
      out->b = (int)((word >> 20) & 1u);
      return 0;
    }
    /* VDUP.<size> Qd/Dd, Rt: bit4=1, coproc 1011, bit23=1, bit20=0. */
    if (dbl && ((word >> 4) & 1u) && ((word >> 23) & 1u) && ((word >> 20) & 1u) == 0) {
      uint32_t q = (word >> 21) & 1u;
      uint32_t d = (nbit << 4) | vn; /* D is bit 7, Vd is bits 19-16 */
      uint32_t bbit = (word >> 22) & 1u;
      uint32_t ebit = (word >> 5) & 1u;
      uint32_t esize = bbit ? 1u : (ebit ? 2u : 4u);
      if (q && (d & 1u)) {
        return -1;
      }
      out->op = MANGO_OP_VDUP;
      out->rd = d;
      out->rn = vd; /* Rt lives in the CRd/Vd field (bits 15-12) */
      out->b = (int)q;
      out->imm = esize;
      return 0;
    }
    /* VMOV.32 Dd[x], Rt / Rt, Dd[x] (A8.8.340): bit4=1, cp=1011, bit23=0,
     * opc1=0H (bits22:21), opc2=00, bits3-0=0000. u=7; b=L (0=to neon). */
    if (dbl && ((word >> 4) & 1u) && ((word >> 23) & 1u) == 0 && (word & 0xFu) == 0 &&
        ((word >> 21) & 2u) == 0 && ((word >> 5) & 3u) == 0) {
      uint32_t rt = vd; /* bits 15-12 */
      uint32_t d = (nbit << 4) | vn; /* D is bit 7, Vd is bits 19-16 */
      uint32_t lane = (word >> 21) & 1u;
      if (rt == MANGO_REG_PC || d >= 32u) {
        return -1;
      }
      out->op = MANGO_OP_VMOV;
      out->u = 7; /* scalar 32-bit lane ↔ GPR */
      out->rd = d;
      out->rn = rt;
      out->imm = lane;
      out->b = (int)((word >> 20) & 1u); /* L: 0 = Dd[x]←Rt, 1 = Rt←Dd[x] */
      return 0;
    }
    if (dbl && ((word >> 16) & 0xBFu) == 0xB8u && ((word >> 4) & 0xDu) == 0xCu) {
      out->op = MANGO_OP_VCVT; /* vcvt.f64.s32 Dd, Sm */
      out->rd = (dbit << 4) | vd;
      out->rn = (vm << 1) | mbit;
      out->imm = 0;
      return 0;
    }
    if (dbl && ((word >> 16) & 0xBFu) == 0xB8u && ((word >> 4) & 0xDu) == 0x4u) {
      out->op = MANGO_OP_VCVT; /* vcvt.f64.u32 Dd, Sm */
      out->rd = (dbit << 4) | vd;
      out->rn = (vm << 1) | mbit;
      out->imm = 8;
      return 0;
    }
    if (dbl && ((word >> 4) & 0xDu) == 0xCu) {
      uint32_t opc8 = (word >> 16) & 0xBFu;
      if (opc8 == 0xBDu || opc8 == 0xBCu) {
        out->op = MANGO_OP_VCVT; /* s32/u32.f64 Sd, Dm */
        out->rd = (vd << 1) | dbit;
        out->rm = (mbit << 4) | vm;
        out->imm = opc8 == 0xBDu ? 6u : 7u;
        return 0;
      }
    }
    if (!dbl && ((word >> 4) & 0xDu) == 0xCu) {
      uint32_t opc8 = (word >> 16) & 0xBFu;
      uint32_t sd = (vd << 1) | dbit;
      uint32_t sm = (vm << 1) | mbit;
      if (opc8 == 0xBDu || opc8 == 0xBCu || opc8 == 0xB8u) {
        out->op = MANGO_OP_VCVT;
        out->rd = sd;
        out->rn = sm;
        if (opc8 == 0xBDu) {
          out->imm = 3; /* s32.f32 */
        } else if (opc8 == 0xBCu) {
          out->imm = 4; /* u32.f32 */
        } else {
          out->imm = 5; /* f32.s32 */
        }
        return 0;
      }
    }
    if (!dbl && ((word >> 4) & 0xDu) == 0x4u) {
      uint32_t opc8 = (word >> 16) & 0xBFu;
      uint32_t sd = (vd << 1) | dbit;
      uint32_t sm = (vm << 1) | mbit;
      if (opc8 == 0xB8u) {
        out->op = MANGO_OP_VCVT; /* vcvt.f32.u32 Sd, Sm */
        out->rd = sd;
        out->rn = sm;
        out->imm = 9; /* f32.u32 */
        return 0;
      }
    }
    /* VCVT.F32.F64 Sd, Dm (cp B) / VCVT.F64.F32 Dd, Sm (cp A). */
    if (((word >> 16) & 0xFFu) == 0xB7u && ((word >> 4) & 9u) == 8u) {
      out->op = MANGO_OP_VCVT;
      if (dbl) {
        out->imm = 1;
        out->rd = (vd << 1) | dbit;
        out->rm = (mbit << 4) | vm;
      } else {
        out->imm = 2;
        out->rd = (dbit << 4) | vd;
        out->rn = (vm << 1) | mbit;
      }
      return 0;
    }
    if ((word & 0x0FB00F50u) == 0x0E300B00u || (word & 0x0FB00F50u) == 0x0E300A00u) {
      out->op = MANGO_OP_VADD;
      return 0;
    }
    if ((word & 0x0FB00F50u) == 0x0E300B40u || (word & 0x0FB00F50u) == 0x0E300A40u) {
      out->op = MANGO_OP_VSUB;
      return 0;
    }
    if ((word & 0x0FB00F50u) == 0x0E200B00u || (word & 0x0FB00F50u) == 0x0E200A00u) {
      out->op = MANGO_OP_VMUL;
      return 0;
    }
    if ((word & 0x0FB00F50u) == 0x0E800B00u || (word & 0x0FB00F50u) == 0x0E800A00u) {
      out->op = MANGO_OP_VDIV;
      return 0;
    }
    if (dbl && (word & 0x0FBF0FD0u) == 0x0EB00BC0u) {
      out->op = MANGO_OP_VABS;
      return 0;
    }
    if (dbl && (word & 0x0FBF0FD0u) == 0x0EB10B40u) {
      out->op = MANGO_OP_VNEG;
      return 0;
    }
    if (dbl && (word & 0x0FBF0FD0u) == 0x0EB10BC0u) {
      out->op = MANGO_OP_VSQRT;
      return 0;
    }
    if (((word >> 16) & 0x9Eu) == 0x94u && ((word >> 4) & 4u) == 4u) {
      out->op = MANGO_OP_VCMP;
      out->b = dbl;
      out->imm = ((word >> 16) & 1u) ? 1u : 0u; /* 1 = compare with #0.0 */
      if (dbl) {
        out->rd = (dbit << 4) | vd;
        out->rm = (mbit << 4) | vm;
      } else {
        out->rd = (vd << 1) | dbit;
        out->rm = (vm << 1) | mbit;
      }
      return 0;
    }
    if ((word & 0x0FFFFFFF) == 0x0EF1FA10u) {
      out->op = MANGO_OP_VMRS;
      return 0;
    }
    /* VMOV.F32 Sd,Sm / VMOV.F64 Dd,Dm (A8.8.340): opc=1011, Vn=0000, bits7-4=01M0.
     * Must run before VMOV#imm — Vm=0,M=0 (e.g. 0xeeb00a40) aliases the imm mask. */
    if (((word >> 16) & 0xBFu) == 0xB0u && ((word >> 4) & 0xDu) == 0x4u) {
      if (fd >= 32u || fm >= 32u) {
        return -1;
      }
      out->op = MANGO_OP_VMOV;
      out->u = 6; /* scalar Sn←Sm / Dn←Dm */
      out->rd = fd;
      out->rm = fm;
      out->b = dbl;
      return 0;
    }
    /* VMOV.F32 Sd,#imm / VMOV.F64 Dd,#imm (A8.8.343): bits23=1,21-20=11, bits[7:4]==0;
     * imm4L is bits[3:0] (do NOT require low nibble zero). */
    /* Mask clears D (bit22) and imm/Vd fields; cp already 0xA/0xB in this block. */
    if ((word & 0x0FB00EF0u) == 0x0EB00A00u) {
      uint32_t imm8 = (((word >> 16) & 0xFu) << 4) | (word & 0xFu);
      uint64_t pat;
      if (dbl) {
        /* F64 modified immediate: sign:~expbit:expbit*8:imm6:zeros(48). */
        uint32_t sign = (imm8 >> 7) & 1u;
        uint32_t expb = (imm8 >> 6) & 1u;
        uint32_t hi = (sign << 31) | ((1u - expb) << 30) | ((expb ? 0xFFu : 0u) << 22) |
                      ((imm8 & 0x3Fu) << 16);
        pat = (uint64_t)hi << 32;
      } else if (mango_neon_expand_imm(0xFu, 0u, imm8, &pat) != 0) {
        return -1;
      }
      out->op = MANGO_OP_VMOV;
      out->u = 5;
      out->rd = dbl ? ((dbit << 4) | vd) : ((vd << 1) | dbit);
      out->b = dbl;
      out->imm = (uint32_t)pat;
      out->rs = (uint32_t)(pat >> 32);
      return 0;
    }
    (void)opc;
    return -1;
  }

  /* BX Rm: cond 0001 0010 1111 1111 1111 0001 Rm */
  if (((word >> 20) & 0xFF) == 0x12 && ((word >> 4) & 0xFFFF) == 0xFFF1) {
    out->op = MANGO_OP_BX;
    out->rm = word & 0xF;
    return 0;
  }

  /* Hint NOP/YIELD/WFE/WFI/SEV: cond 0011 0010 0000 1111 0000 xxxx */
  if ((word & 0x0FFFFFF0u) == 0x0320F000u) {
    out->op = MANGO_OP_NOP;
    return 0;
  }

  /* LDREX/STREX family: bits 27-24=0001, bits 11-4=11111001. Single-threaded
   * guest: load/store with STREX always reporting success. */
  if (((word >> 24) & 0xF) == 0x1 && ((word >> 4) & 0xFFu) == 0xF9u) {
    uint32_t opc = (word >> 20) & 0xFu;
    uint32_t rn = (word >> 16) & 0xF;
    uint32_t rd = (word >> 12) & 0xF;
    uint32_t rt = word & 0xF;
    int load = opc & 1u;
    uint32_t size = (opc >> 1) & 3u; /* 0=word, 1=dword, 2=byte, 3=half */
    if (rn == MANGO_REG_PC || rd == MANGO_REG_PC || (!load && rt == MANGO_REG_PC)) {
      return -1;
    }
    {
      uint32_t pair = load ? rd : rt;
      if (size == 1u && (pair & 1u)) {
        return -1; /* LDREXD/STREXD data pair must be even */
      }
    }
    out->op = load ? MANGO_OP_LDREX : MANGO_OP_STREX;
    out->rn = rn;
    out->rd = rd;
    out->rm = load ? 0 : rt;
    out->b = (int)size;
    return 0;
  }

  /* CLZ Rd, Rm: cond 0001 0110 1111 Rd 1111 0001 Rm */
  if (((word >> 16) & 0xFFFu) == 0x16Fu && ((word >> 4) & 0xFFu) == 0xF1u) {
    uint32_t rd = (word >> 12) & 0xF;
    uint32_t rm = word & 0xF;
    if (rd == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_CLZ;
    out->rd = rd;
    out->rm = rm;
    return 0;
  }

  /* SMLAxy / SMULxy (A8.8.207 / A8.8.220): signed 16x16→32 halfword multiply.
   * SMLA: cond 00010000 Rd Ra Rm 1 M N 0 Rn → Rd = halfN(Rn)*halfM(Rm) + Ra
   * SMUL: cond 00010110 Rd SBZ Rm 1 M N 0 Rn → Rd = halfN(Rn)*halfM(Rm)
   * N=bit5 selects Rn half (0=bottom/B, 1=top/T); M=bit6 selects Rm half.
   * Shared decode: b=N, u=M; rm=Rn, rs=Rm, rn=Ra (SMLA only). */
  if ((((word >> 20) & 0xFF) == 0x10 || ((word >> 20) & 0xFF) == 0x16) &&
      ((word >> 7) & 1u) == 1u && ((word >> 4) & 1u) == 0u) {
    uint32_t smul = ((word >> 20) & 0xFF) == 0x16;
    uint32_t rd = (word >> 16) & 0xF;
    uint32_t ra = (word >> 12) & 0xF;
    uint32_t rm = (word >> 8) & 0xF;
    uint32_t rn = word & 0xF;
    uint32_t n_bit = (word >> 5) & 1u;
    uint32_t m_bit = (word >> 6) & 1u;
    if (smul && ra != 0) {
      return -1; /* SMULxy Ra is SBZ */
    }
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC ||
        (!smul && ra == MANGO_REG_PC)) {
      return -1;
    }
    out->op = smul ? MANGO_OP_SMUL : MANGO_OP_SMLA;
    out->rd = rd;
    out->rn = smul ? 0 : ra; /* accumulate for SMLA */
    out->rm = rn;            /* ARM Rn (bits 3-0): first halfword source */
    out->rs = rm;            /* ARM Rm (bits 11-8): second halfword source */
    out->b = (int)n_bit;     /* 1 = top half of Rn */
    out->u = (int)m_bit;     /* 1 = top half of Rm */
    return 0;
  }

  /* BLX Rm: cond 0001 0010 1111 1111 1111 0011 Rm. JNI vtable calls. */
  if (((word >> 20) & 0xFF) == 0x12 && ((word >> 4) & 0xFFFF) == 0xFFF3) {
    uint32_t rm = word & 0xF;
    if (rm == MANGO_REG_PC) {
      return -1; /* BLX PC is UNPREDICTABLE */
    }
    out->op = MANGO_OP_BLX;
    out->rm = rm;
    out->is_imm = 0;
    return 0;
  }

  /* Bitfield BFC/BFI/SBFX/UBFX: bits 27-24=0111, bit4=1. */
  if (((word >> 24) & 0xF) == 0x7 && ((word >> 4) & 1u) == 1) {
    uint32_t opc = (word >> 21) & 7u;
    uint32_t rd = (word >> 12) & 0xF;
    uint32_t lsb = (word >> 7) & 0x1Fu;
    uint32_t msb = (word >> 16) & 0x1Fu;
    uint32_t rn = word & 0xF;
    uint32_t op2 = (word >> 4) & 7u;
    /* SMMUL Rd, Rn, Rm: 0111 0101 Rd 1111 Rm 0001 00 Rn. Ra(15:12)=1111. */
    if (((word >> 20) & 0xF) == 0x5 && ((word >> 12) & 0xF) == 0xF &&
        ((word >> 4) & 0x9) == 0x1) {
      uint32_t d = (word >> 16) & 0xF;
      uint32_t n = word & 0xF;
      uint32_t m = (word >> 8) & 0xF;
      if (d == MANGO_REG_PC || n == MANGO_REG_PC || m == MANGO_REG_PC) {
        return -1;
      }
      out->op = MANGO_OP_SMMUL;
      out->rd = d;
      out->rn = n;
      out->rm = m;
      return 0;
    }
    if (rd == MANGO_REG_PC) {
      return -1;
    }
    if (op2 == 1u && opc == 6u) {
      if (msb < lsb) {
        return -1;
      }
      out->op = (rn == 0xFu) ? MANGO_OP_BFC : MANGO_OP_BFI;
      out->rd = rd;
      out->rm = rn;
      out->imm = lsb;
      out->rs = msb;
      return 0;
    }
    if (op2 == 5u && (opc == 5u || opc == 7u)) {
      if ((uint32_t)lsb + msb > 31u) {
        return -1;
      }
      out->op = (opc == 7u) ? MANGO_OP_UBFX : MANGO_OP_SBFX;
      out->rd = rd;
      out->rn = rn;
      out->imm = lsb;
      out->rs = msb; /* width-1 */
      return 0;
    }
  }

  /* MOVW: cond 0011 0000 imm4 Rd imm12. Overlaps S=0 TST, which we reject. */
  if (((word >> 20) & 0xFF) == 0x30) {
    uint32_t rd = (word >> 12) & 0xF;
    if (rd == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_MOV;
    out->rd = rd;
    out->is_imm = 1;
    out->imm = (((word >> 16) & 0xF) << 12) | (word & 0xFFF);
    return 0;
  }

  /* MOVT: cond 0011 0100 imm4 Rd imm12. Overlaps S=0 CMP, which we reject. */
  if (((word >> 20) & 0xFF) == 0x34) {
    uint32_t rd = (word >> 12) & 0xF;
    if (rd == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_MOVT;
    out->rd = rd;
    out->is_imm = 1;
    out->imm = (((word >> 16) & 0xF) << 12) | (word & 0xFFF);
    return 0;
  }

  /* B/BL imm24: bits 27-25 = 101, bit 24 = L (0=B, 1=BL) */
  if (((word >> 25) & 0x7) == 0x5) {
    uint32_t l = (word >> 24) & 0x1;
    uint32_t imm24 = word & 0xFFFFFF;
    uint32_t offset;
    if (imm24 & 0x800000) {
      offset = (imm24 | 0xFF000000u) << 2;
    } else {
      offset = imm24 << 2;
    }
    out->op = l ? MANGO_OP_BL : MANGO_OP_B;
    out->imm = offset; /* two's complement offset, added as unsigned */
    return 0;
  }

  /* MUL/MLA: cond 000000 A S Rd Ra Rs 1001 Rm. Same bits27-26 as
   * data-processing below, so this must be checked first or AND/EOR would
   * silently steal it (their opcodes are 0000/0001, exactly A/S here). */
  if (((word >> 22) & 0x3F) == 0x0 && ((word >> 4) & 0xF) == 0x9) {
    uint32_t a = (word >> 21) & 0x1;
    uint32_t s = (word >> 20) & 0x1;
    uint32_t rd = (word >> 16) & 0xF;
    uint32_t ra = (word >> 12) & 0xF;
    uint32_t rs = (word >> 8) & 0xF;
    uint32_t rm = word & 0xF;
    if (rd == MANGO_REG_PC || rm == MANGO_REG_PC || rs == MANGO_REG_PC ||
        (a && ra == MANGO_REG_PC)) {
      return -1; /* PC as an operand is UNPREDICTABLE */
    }
    if (!a && ra != 0) {
      return -1; /* MUL's Ra field is SBZ */
    }
    out->op = a ? MANGO_OP_MLA : MANGO_OP_MUL;
    out->rd = rd;
    out->rn = ra; /* accumulate for MLA; 0 for MUL */
    out->rm = rm;
    out->rs = rs;
    out->sets_flags = (int)s;
    return 0;
  }

  /* UMULL/UMLAL/SMULL/SMLAL (A8.8.255 / A8.8.189 / A8.8.221 / A8.8.210):
   * cond 00001 U A S RdHi RdLo Rm 1001 Rn. Bit23=1 distinguishes from MUL.
   * U=0 unsigned (UMULL/UMLAL), U=1 signed (SMULL/SMLAL). A=accumulate.
   * RdLo<-rd, RdHi<-rn, multiplicands Rm<-rs / Rn<-rm (same bit lanes as MUL). */
  if (((word >> 23) & 0x1F) == 0x01 && ((word >> 4) & 0xF) == 0x9) {
    uint32_t u = (word >> 22) & 0x1;
    uint32_t a = (word >> 21) & 0x1;
    uint32_t s = (word >> 20) & 0x1;
    uint32_t rdhi = (word >> 16) & 0xF;
    uint32_t rdlo = (word >> 12) & 0xF;
    uint32_t rm = (word >> 8) & 0xF;
    uint32_t rn = word & 0xF;
    if (rdlo == MANGO_REG_PC || rdhi == MANGO_REG_PC || rm == MANGO_REG_PC ||
        rn == MANGO_REG_PC || rdlo == rdhi) {
      return -1; /* PC or RdLo==RdHi is UNPREDICTABLE */
    }
    if (!u && !a) {
      out->op = MANGO_OP_UMULL;
    } else if (!u && a) {
      out->op = MANGO_OP_UMLAL;
    } else if (u && !a) {
      out->op = MANGO_OP_SMULL;
    } else {
      out->op = MANGO_OP_SMLAL;
    }
    out->rd = rdlo;
    out->rn = rdhi;
    out->rm = rn; /* ARM Rn (bits 3-0), same lane as MUL's Rm */
    out->rs = rm; /* ARM Rm (bits 11-8), same lane as MUL's Rs */
    out->sets_flags = (int)s;
    return 0;
  }

  /* SWP/SWPB: cond 00010 B 00 Rn Rt SBZ 1001 Rm. Bits 7-4 match MUL, but
   * bits 27-23 are 00010 rather than MUL's 00000. */
  if (((word >> 23) & 0x1F) == 0x02 && ((word >> 20) & 0x3) == 0x0 && ((word >> 4) & 0xF) == 0x9) {
    uint32_t b = (word >> 22) & 0x1;
    uint32_t rn = (word >> 16) & 0xF;
    uint32_t rt = (word >> 12) & 0xF;
    uint32_t sbz = (word >> 8) & 0xF;
    uint32_t rm = word & 0xF;
    if (sbz != 0 || rn == MANGO_REG_PC || rt == MANGO_REG_PC || rm == MANGO_REG_PC || rt == rn ||
        rm == rn) {
      return -1; /* SBZ, PC, or Rn overlapping Rt/Rm are all UNPREDICTABLE */
    }
    out->op = MANGO_OP_SWP;
    out->rn = rn;
    out->rd = rt;
    out->rm = rm;
    out->b = (int)b;
    return 0;
  }

  /* Extra load/store: bits 27-25=000, bits 7 and 4 set, bits 6-5 != 00
   * (00 would be MUL/SWP, already handled). Bit 22 is I here (1=imm),
   * inverted from the regular LDR/STR I-bit. */
  if (((word >> 25) & 0x7) == 0x0 && ((word >> 7) & 0x1) == 1 && ((word >> 4) & 0x1) == 1 &&
      ((word >> 5) & 0x3) != 0) {
    uint32_t p = (word >> 24) & 0x1;
    uint32_t u = (word >> 23) & 0x1;
    uint32_t i = (word >> 22) & 0x1;
    uint32_t w = (word >> 21) & 0x1;
    uint32_t l = (word >> 20) & 0x1;
    uint32_t rn = (word >> 16) & 0xF;
    uint32_t rt = (word >> 12) & 0xF;
    uint32_t s = (word >> 6) & 0x1;
    uint32_t h = (word >> 5) & 0x1;
    int writeback = (!p || w) ? 1 : 0;
    MangoOp op;

    if (!p && w) {
      return -1; /* unprivileged extra T-form, UNPREDICTABLE in this encoding */
    }
    if (rt == MANGO_REG_PC) {
      return -1;
    }
    if (writeback && rn == MANGO_REG_PC) {
      return -1;
    }
    if (l && !s && h) {
      op = MANGO_OP_LDRH;
    } else if (!l && !s && h) {
      op = MANGO_OP_STRH;
    } else if (l && s && !h) {
      op = MANGO_OP_LDRSB;
    } else if (l && s && h) {
      op = MANGO_OP_LDRSH;
    } else if (!l && s && !h) {
      op = MANGO_OP_LDRD;
    } else if (!l && s && h) {
      op = MANGO_OP_STRD;
    } else {
      return -1;
    }
    if (op == MANGO_OP_LDRD || op == MANGO_OP_STRD) {
      if ((rt & 1u) || rt == 14u) {
        return -1; /* pair must be even and not include PC */
      }
      if (writeback && (rn == rt || rn == rt + 1u)) {
        return -1;
      }
    } else if (writeback && l && rt == rn) {
      return -1; /* LDRH/LDRSB/LDRSH writeback into the dest */
    }

    out->op = op;
    out->rn = rn;
    out->rd = rt;
    out->u = (int)u;
    out->p = (int)p;
    out->w = writeback;

    if (i) {
      out->is_imm = 1;
      out->imm = (((word >> 8) & 0xF) << 4) | (word & 0xF);
    } else {
      if (((word >> 8) & 0xF) != 0) {
        return -1; /* register form's bits 11-8 are SBZ */
      }
      uint32_t rm = word & 0xF;
      if (rm == MANGO_REG_PC) {
        return -1; /* Rm=PC is UNPREDICTABLE for extra load/store */
      }
      if (op == MANGO_OP_LDRD && (rm == rt || rm == rt + 1u)) {
        return -1;
      }
      out->is_imm = 0;
      out->rm = rm;
    }
    return 0;
  }

  /* Data-processing: bits 27-26 == 00 */
  if (((word >> 26) & 0x3) == 0x0) {
    uint32_t i = (word >> 25) & 0x1;
    uint32_t opcode = (word >> 21) & 0xF;
    uint32_t s = (word >> 20) & 0x1;
    uint32_t rn = (word >> 16) & 0xF;
    uint32_t rd = (word >> 12) & 0xF;
    uint32_t operand2 = word & 0xFFF;
    MangoOp op;

    switch (opcode) {
      case 0x0:
        op = MANGO_OP_AND;
        break;
      case 0x1:
        op = MANGO_OP_EOR;
        break;
      case 0x2:
        op = MANGO_OP_SUB;
        break;
      case 0x3:
        op = MANGO_OP_RSB;
        break;
      case 0x4:
        op = MANGO_OP_ADD;
        break;
      case 0x5:
        op = MANGO_OP_ADC;
        break;
      case 0x6:
        op = MANGO_OP_SBC;
        break;
      case 0x7:
        op = MANGO_OP_RSC;
        break;
      case 0x8:
      case 0x9:
      case 0xA:
      case 0xB:
        /* TST/TEQ/CMP/CMN: S=0 here isn't one of these at all (it overlaps
         * MRS/MSR's encoding instead), so decode nothing rather than
         * guess. */
        if (!s) {
          return -1;
        }
        op = opcode == 0x8   ? MANGO_OP_TST
             : opcode == 0x9 ? MANGO_OP_TEQ
             : opcode == 0xA ? MANGO_OP_CMP
                             : MANGO_OP_CMN;
        break;
      case 0xC:
        op = MANGO_OP_ORR;
        break;
      case 0xD:
        op = MANGO_OP_MOV;
        break;
      case 0xE:
        op = MANGO_OP_BIC;
        break;
      case 0xF:
        op = MANGO_OP_MVN;
        break;
      default:
        return -1;
    }

    out->op = op;
    out->rn = rn;
    out->rd = rd;
    out->sets_flags = (int)s;

    if (i) {
      uint32_t imm8 = operand2 & 0xFF;
      uint32_t rot = ((operand2 >> 8) & 0xF) * 2;
      uint32_t val = imm8;
      if (rot != 0) {
        val = (imm8 >> rot) | (imm8 << (32 - rot));
      }
      out->is_imm = 1;
      out->imm = val;
      out->shift_amount = rot; /* 0 => C unaffected; else C = rotated bit 31 */
    } else {
      uint32_t shift_by_reg = (operand2 >> 4) & 0x1;
      uint32_t shift_type = (operand2 >> 5) & 0x3;
      uint32_t rm = operand2 & 0xF;
      if (shift_by_reg) {
        if ((operand2 >> 7) & 0x1) {
          return -1; /* bit 7 must be 0; 1 is multiply/SWP/etc. */
        }
        uint32_t rs = (operand2 >> 8) & 0xF;
        if (rs == MANGO_REG_PC) {
          return -1; /* register-specified shift with Rs=PC is UNPREDICTABLE */
        }
        out->rm = rm;
        out->rs = rs;
        out->shift_type = shift_type;
        out->shift_by_reg = 1;
      } else {
        /* amount 0 is LSL #0, LSR #32, ASR #32, or RRX — execute handles it */
        out->rm = rm;
        out->shift_type = shift_type;
        out->shift_amount = (operand2 >> 7) & 0x1F;
      }
    }
    return 0;
  }

  /* Media: bits 27-24=0110, bit4=1. REV/RBIT, UXT/SXT, PKH. Must run
   * before LDR/STR, which would see bit4=1 and reject these as undefined. */
  if (((word >> 24) & 0xF) == 0x6 && ((word >> 4) & 1u) == 1) {
    uint32_t op = (word >> 20) & 0xF;
    uint32_t rn = (word >> 16) & 0xF;
    uint32_t rd = (word >> 12) & 0xF;
    uint32_t rm = word & 0xF;
    uint32_t op2 = (word >> 4) & 0xF;
    if (rd == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    if (rn == 0xFu && ((word >> 8) & 0xF) == 0xFu) {
      if (op == 0xBu && op2 == 0x3u) {
        out->op = MANGO_OP_REV;
        out->rd = rd;
        out->rm = rm;
        out->imm = 0; /* REV */
        return 0;
      }
      if (op == 0xBu && op2 == 0xBu) {
        out->op = MANGO_OP_REV;
        out->rd = rd;
        out->rm = rm;
        out->imm = 1; /* REV16 */
        return 0;
      }
      if (op == 0xFu && op2 == 0xBu) {
        out->op = MANGO_OP_REV;
        out->rd = rd;
        out->rm = rm;
        out->imm = 2; /* REVSH */
        return 0;
      }
      if (op == 0xFu && op2 == 0x3u) {
        out->op = MANGO_OP_REV;
        out->rd = rd;
        out->rm = rm;
        out->imm = 3; /* RBIT */
        return 0;
      }
    }
    if (((word >> 4) & 0x3Fu) == 0x07u) {
      int uns;
      int half;
      switch (op) {
        case 0xA:
          uns = 0;
          half = 0;
          break; /* SXTB / SXTAB */
        case 0xB:
          uns = 0;
          half = 1;
          break; /* SXTH / SXTAH */
        case 0xE:
          uns = 1;
          half = 0;
          break; /* UXTB / UXTAB */
        case 0xF:
          uns = 1;
          half = 1;
          break; /* UXTH / UXTAH */
        default:
          return -1;
      }
      out->op = MANGO_OP_XTEND;
      out->rd = rd;
      out->rn = rn;
      out->rm = rm;
      out->imm = ((word >> 10) & 3u) * 8u;
      out->u = uns;
      out->b = half;
      return 0;
    }
    if (op == 0x8u && (op2 & 3u) == 1u) {
      /* PKHBT (bit6=0) / PKHTB (bit6=1), bits 5-4 = 01. */
      out->op = MANGO_OP_PKH;
      out->rd = rd;
      out->rn = rn;
      out->rm = rm;
      out->shift_amount = (word >> 7) & 0x1Fu;
      out->b = (op2 >> 2) & 1; /* 0=PKHBT LSL, 1=PKHTB ASR */
      return 0;
    }
  }

  /* LDR/STR: bits 27-26=01. Immediate or register offset, pre/post-index,
   * optional writeback. P=0 W=1 is LDRT/STRT, not this subset. */
  if (((word >> 26) & 0x3) == 0x1) {
    uint32_t i = (word >> 25) & 0x1;
    uint32_t p = (word >> 24) & 0x1;
    uint32_t u = (word >> 23) & 0x1;
    uint32_t b = (word >> 22) & 0x1;
    uint32_t w = (word >> 21) & 0x1;
    uint32_t l = (word >> 20) & 0x1;
    uint32_t rn = (word >> 16) & 0xF;
    uint32_t rt = (word >> 12) & 0xF;
    uint32_t operand12 = word & 0xFFF;
    int writeback = (!p || w) ? 1 : 0;

    if (!p && w) {
      return -1; /* LDRT/STRT unprivileged form */
    }
    if (rt == MANGO_REG_PC && !l) {
      return -1; /* STR PC; LDR PC is a branch, executed in interp */
    }
    if (writeback && rn == MANGO_REG_PC) {
      return -1; /* writeback into PC is UNPREDICTABLE */
    }
    if (writeback && l && rt == rn) {
      return -1; /* LDR writeback into the same register as the dest */
    }

    out->op = l ? MANGO_OP_LDR : MANGO_OP_STR;
    out->rn = rn;
    out->rd = rt;
    out->u = (int)u;
    out->b = (int)b;
    out->p = (int)p;
    out->w = writeback;

    if (!i) {
      out->is_imm = 1;
      out->imm = operand12;
    } else {
      if (operand12 & 0x10u) {
        return -1; /* bit 4 must be 0; 1 is media/undefined, not Rm-shift */
      }
      out->is_imm = 0;
      out->rm = operand12 & 0xF;
      out->shift_type = (operand12 >> 5) & 0x3;
      out->shift_amount = (operand12 >> 7) & 0x1F;
    }
    return 0;
  }

  /* LDM/STM: bits 27-25 = 100. PUSH is STMDB sp!, POP is LDMIA sp!. */
  if (((word >> 25) & 0x7) == 0x4) {
    uint32_t p = (word >> 24) & 0x1;
    uint32_t u = (word >> 23) & 0x1;
    uint32_t s = (word >> 22) & 0x1;
    uint32_t w = (word >> 21) & 0x1;
    uint32_t l = (word >> 20) & 0x1;
    uint32_t rn = (word >> 16) & 0xF;
    uint32_t reglist = word & 0xFFFFu;

    if (s) {
      return -1; /* user-bank / SPSR form, not needed for user-mode code */
    }
    if (reglist == 0) {
      return -1; /* empty list is UNPREDICTABLE */
    }
    if (rn == MANGO_REG_PC) {
      return -1; /* PC as base is UNPREDICTABLE */
    }
    if (w && (reglist & (1u << rn)) && rn != MANGO_REG_SP) {
      /* Writeback with Rn in the list is UNPREDICTABLE for non-SP bases.
       * STMDB/LDMIA sp!,{sp,...} appears in real AAPCS/bionic frames; allow
       * SP. STM stores the original (pre-writeback) SP — interp applies
       * writeback after the stores. */
      return -1;
    }
    if (!l && (reglist & (1u << MANGO_REG_PC))) {
      /* STM of PC stores PC+8 or PC+12 depending on the core; refuse to
       * guess. Real prologues push LR, not PC. */
      return -1;
    }

    out->op = l ? MANGO_OP_LDM : MANGO_OP_STM;
    out->rn = rn;
    out->reglist = reglist;
    out->p = (int)p;
    out->u = (int)u;
    out->w = (int)w;
    return 0;
  }

  return -1;
}

int mango_decode_t16(uint16_t hw, MangoInsn* out) {
  mango_insn_clear(out);
  out->cond = 0xE; /* AL; conditional B overrides this */

  /* 32-bit Thumb: BL and a large ALU/load group. Not this subset. */
  if ((hw >> 11) >= 0x1Du) {
    return -1;
  }

  /* Shifts and add/sub (000xx) */
  if ((hw >> 13) == 0) {
    uint32_t opc = (hw >> 11) & 3u;
    uint32_t rd = hw & 7u;
    if (opc < 3u) {
      out->op = MANGO_OP_MOV;
      out->rd = rd;
      out->rm = (hw >> 3) & 7u;
      out->shift_type = opc; /* 0=LSL, 1=LSR, 2=ASR */
      out->shift_amount = (hw >> 6) & 0x1Fu;
      out->sets_flags = 1;
      return 0;
    }
    out->op = ((hw >> 9) & 1u) ? MANGO_OP_SUB : MANGO_OP_ADD;
    out->rd = rd;
    out->rn = (hw >> 3) & 7u;
    out->sets_flags = 1;
    if ((hw >> 10) & 1u) {
      out->is_imm = 1;
      out->imm = (hw >> 6) & 7u;
    } else {
      out->rm = (hw >> 6) & 7u;
    }
    return 0;
  }

  /* MOV/CMP/ADD/SUB Rd, #imm8 (001xx) */
  if ((hw >> 13) == 1u) {
    uint32_t opc = (hw >> 11) & 3u;
    uint32_t rd = (hw >> 8) & 7u;
    out->is_imm = 1;
    out->imm = hw & 0xFFu;
    out->rd = rd;
    out->rn = rd;
    out->sets_flags = 1;
    if (opc == 0) {
      out->op = MANGO_OP_MOV;
    } else if (opc == 1) {
      out->op = MANGO_OP_CMP;
      out->sets_flags = 1;
    } else if (opc == 2) {
      out->op = MANGO_OP_ADD;
    } else {
      out->op = MANGO_OP_SUB;
    }
    return 0;
  }

  /* Data-processing register (010000) */
  if ((hw >> 10) == 0x10u) {
    uint32_t opc = (hw >> 6) & 0xFu;
    uint32_t rs = (hw >> 3) & 7u;
    uint32_t rd = hw & 7u;
    static const MangoOp kDp[16] = {
        MANGO_OP_AND, MANGO_OP_EOR, MANGO_OP_MOV, MANGO_OP_MOV, MANGO_OP_MOV, MANGO_OP_ADC,
        MANGO_OP_SBC, MANGO_OP_MOV, MANGO_OP_TST, MANGO_OP_RSB, MANGO_OP_CMP, MANGO_OP_CMN,
        MANGO_OP_ORR, MANGO_OP_MUL, MANGO_OP_BIC, MANGO_OP_MVN,
    };
    out->op = kDp[opc];
    out->rd = rd;
    out->sets_flags = 1; /* T16 ALU always sets NZCV */
    if (opc == 2u || opc == 3u || opc == 4u || opc == 7u) {
      /* LSL/LSR/ASR/ROR Rd, Rs: Rd = Rd shift Rs */
      out->rm = rd;
      out->rs = rs;
      out->shift_by_reg = 1;
      out->shift_type = (opc == 2u) ? 0u : (opc == 3u) ? 1u : (opc == 4u) ? 2u : 3u;
    } else if (opc == 8u || opc == 10u || opc == 11u) {
      out->rn = rd;
      out->rm = rs;
      out->sets_flags = 1;
    } else if (opc == 9u) {
      /* NEG Rd, Rm = RSB Rd, Rm, #0 */
      out->rn = rs;
      out->is_imm = 1;
      out->imm = 0;
    } else if (opc == 13u) {
      out->rm = rd;
      out->rs = rs;
    } else if (opc == 15u) {
      out->rm = rs;
    } else {
      out->rn = rd;
      out->rm = rs;
    }
    return 0;
  }

  /* Special high registers / BX (010001) */
  if ((hw >> 10) == 0x11u) {
    uint32_t opc = (hw >> 8) & 3u;
    uint32_t rm = (hw >> 3) & 0xFu;
    uint32_t rd = (hw & 7u) | (((hw >> 7) & 1u) << 3);
    if (opc == 3u) {
      if (hw & 7u) {
        return -1; /* SBZ Rd bits must be 0 */
      }
      out->rm = rm;
      if (hw & (1u << 7)) {
        out->op = MANGO_OP_BLX; /* T16 BLX Rm, used for Thumb JNI vtable calls */
        out->is_imm = 0;
      } else {
        out->op = MANGO_OP_BX;
      }
      return 0;
    }
    out->rd = rd;
    out->rm = rm;
    out->rn = rd;
    if (opc == 0) {
      out->op = MANGO_OP_ADD;
    } else if (opc == 1) {
      out->op = MANGO_OP_CMP;
      out->sets_flags = 1;
    } else {
      out->op = MANGO_OP_MOV;
    }
    return 0;
  }

  /* LDR literal (01001) */
  if ((hw >> 11) == 0x09u) {
    out->op = MANGO_OP_LDR;
    out->rd = (hw >> 8) & 7u;
    out->rn = MANGO_REG_PC;
    out->is_imm = 1;
    out->imm = (uint32_t)(hw & 0xFFu) << 2;
    out->p = 1;
    out->u = 1;
    return 0;
  }

  /* Load/store register offset (0101) */
  if ((hw >> 12) == 0x5u) {
    uint32_t opc = (hw >> 9) & 7u;
    out->rn = (hw >> 3) & 7u;
    out->rd = hw & 7u;
    out->rm = (hw >> 6) & 7u;
    out->p = 1;
    out->u = 1;
    switch (opc) {
      case 0:
        out->op = MANGO_OP_STR;
        break;
      case 1:
        out->op = MANGO_OP_STRH;
        break;
      case 2:
        out->op = MANGO_OP_STR;
        out->b = 1;
        break;
      case 3:
        out->op = MANGO_OP_LDRSB;
        break;
      case 4:
        out->op = MANGO_OP_LDR;
        break;
      case 5:
        out->op = MANGO_OP_LDRH;
        break;
      case 6:
        out->op = MANGO_OP_LDR;
        out->b = 1;
        break;
      default:
        out->op = MANGO_OP_LDRSH;
        break;
    }
    return 0;
  }

  /* STR/LDR/STRB/LDRB imm5 (011xx) */
  if ((hw >> 13) == 0x3u) {
    int b = (int)((hw >> 12) & 1u);
    int l = (int)((hw >> 11) & 1u);
    uint32_t imm5 = (hw >> 6) & 0x1Fu;
    out->op = l ? MANGO_OP_LDR : MANGO_OP_STR;
    out->b = b;
    out->rd = hw & 7u;
    out->rn = (hw >> 3) & 7u;
    out->is_imm = 1;
    out->imm = b ? imm5 : (imm5 << 2);
    out->p = 1;
    out->u = 1;
    return 0;
  }

  /* STRH/LDRH imm5 (1000x) */
  if ((hw >> 12) == 0x8u) {
    out->op = ((hw >> 11) & 1u) ? MANGO_OP_LDRH : MANGO_OP_STRH;
    out->rd = hw & 7u;
    out->rn = (hw >> 3) & 7u;
    out->is_imm = 1;
    out->imm = ((hw >> 6) & 0x1Fu) << 1;
    out->p = 1;
    out->u = 1;
    return 0;
  }

  /* STR/LDR SP-relative (1001x) */
  if ((hw >> 12) == 0x9u) {
    out->op = ((hw >> 11) & 1u) ? MANGO_OP_LDR : MANGO_OP_STR;
    out->rd = (hw >> 8) & 7u;
    out->rn = MANGO_REG_SP;
    out->is_imm = 1;
    out->imm = (uint32_t)(hw & 0xFFu) << 2;
    out->p = 1;
    out->u = 1;
    return 0;
  }

  /* ADR / ADD Rd, SP, #imm (1010x) */
  if ((hw >> 12) == 0xAu) {
    out->op = MANGO_OP_ADD;
    out->rd = (hw >> 8) & 7u;
    out->rn = ((hw >> 11) & 1u) ? MANGO_REG_SP : MANGO_REG_PC;
    out->is_imm = 1;
    out->imm = (uint32_t)(hw & 0xFFu) << 2;
    return 0;
  }

  /* Misc 1011: ADD/SUB SP, PUSH/POP */
  if ((hw >> 12) == 0xBu) {
    if ((hw >> 8) == 0xB0u) {
      out->op = ((hw >> 7) & 1u) ? MANGO_OP_SUB : MANGO_OP_ADD;
      out->rd = MANGO_REG_SP;
      out->rn = MANGO_REG_SP;
      out->is_imm = 1;
      out->imm = (uint32_t)(hw & 0x7Fu) << 2;
      return 0;
    }
    if ((hw >> 9) == 0x5Au) { /* PUSH */
      uint32_t list = hw & 0xFFu;
      if ((hw >> 8) & 1u) {
        list |= (1u << MANGO_REG_LR);
      }
      if (list == 0) {
        return -1;
      }
      out->op = MANGO_OP_STM;
      out->rn = MANGO_REG_SP;
      out->reglist = list;
      out->p = 1;
      out->u = 0;
      out->w = 1;
      return 0;
    }
    if ((hw >> 8) == 0xBFu) { /* IT */
      uint32_t mask = hw & 0xFu;
      uint32_t firstcond = (hw >> 4) & 0xFu;
      if (mask == 0 || firstcond == 0xFu) {
        return -1;
      }
      out->op = MANGO_OP_IT;
      out->imm = (firstcond << 4) | mask; /* 8-bit ITSTATE */
      return 0;
    }
    if ((hw >> 9) == 0x5Eu) { /* POP */
      uint32_t list = hw & 0xFFu;
      if ((hw >> 8) & 1u) {
        list |= (1u << MANGO_REG_PC);
      }
      if (list == 0) {
        return -1;
      }
      out->op = MANGO_OP_LDM;
      out->rn = MANGO_REG_SP;
      out->reglist = list;
      out->p = 0;
      out->u = 1;
      out->w = 1;
      return 0;
    }
    /* CBZ/CBNZ: 1011 op 0 i 1 imm5 Rn — (hw & 0xF500) == 0xB100.
     * op bit11: 0=CBZ, 1=CBNZ. Tests Rn vs zero; does not touch NZCV. */
    if ((hw & 0xF500u) == 0xB100u) {
      uint32_t i = (hw >> 9) & 1u;
      uint32_t imm5 = (hw >> 3) & 0x1Fu;
      out->op = ((hw >> 11) & 1u) ? MANGO_OP_CBNZ : MANGO_OP_CBZ;
      out->rn = hw & 7u;
      out->is_imm = 1;
      out->imm = (i << 6) | (imm5 << 1);
      out->cond = 0xEu; /* always execute the compare-branch itself */
      return 0;
    }
    /* Q-OTTD-0l: T16 UXTH/UXTB/SXTH/SXTB — Misc extract, no rotate.
     * (hw & 0xFF00) == 0xB200; bits[7:6]: 00=SXTH, 01=SXTB, 10=UXTH, 11=UXTB.
     * Guest b2b6 = uxth r6,r6; sibling b2f6 = uxtb r6,r6. Map → XTEND
     * (rn=15, imm=0); do not open A32 accumulate / T32 FA this bite. */
    if ((hw & 0xFF00u) == 0xB200u) {
      out->op = MANGO_OP_XTEND;
      out->rd = hw & 7u;
      out->rm = (hw >> 3) & 7u;
      out->rn = 15u; /* no accumulate */
      out->imm = 0;  /* T16 has no ROR */
      out->u = (int)((hw >> 7) & 1u);
      out->b = !((hw >> 6) & 1u); /* half when bit6=0 (UXTH/SXTH) */
      out->sets_flags = 0;
      return 0;
    }
    /* Q-OTTD-0aq: T16 REV/REV16/REVSH. OpenTTD ba49 = rev16 r1,r1.
     * bits[7:6]: 00=REV, 01=REV16, 11=REVSH. 10 is not a reverse.
     * Reuse MANGO_OP_REV (imm 0/1/2). Low registers only. */
    if ((hw & 0xFF00u) == 0xBA00u && ((hw >> 6) & 3u) != 2u) {
      uint32_t kind = (hw >> 6) & 3u;
      out->op = MANGO_OP_REV;
      out->rd = hw & 7u;
      out->rm = (hw >> 3) & 7u;
      out->imm = (kind == 3u) ? 2u : kind;
      return 0;
    }
    return -1;
  }

  /* STMIA/LDMIA (1100x) */
  if ((hw >> 12) == 0xCu) {
    uint32_t l = (hw >> 11) & 1u;
    uint32_t rn = (hw >> 8) & 7u;
    uint32_t list = hw & 0xFFu;
    if (list == 0) {
      return -1;
    }
    out->op = l ? MANGO_OP_LDM : MANGO_OP_STM;
    out->rn = rn;
    out->reglist = list;
    out->p = 0;
    out->u = 1;
    out->w = (l && (list & (1u << rn))) ? 0 : 1;
    return 0;
  }

  /* B<cond> / SVC (1101) */
  if ((hw >> 12) == 0xDu) {
    uint32_t cond = (hw >> 8) & 0xFu;
    if (cond == 0xFu) {
      out->op = MANGO_OP_SVC;
      return 0;
    }
    if (cond == 0xEu) {
      return -1; /* undefined */
    }
    int32_t imm8 = (int8_t)(hw & 0xFFu);
    out->op = MANGO_OP_B;
    out->cond = cond;
    out->imm = (uint32_t)(imm8 << 1);
    return 0;
  }

  /* B uncond (11100) */
  if ((hw >> 11) == 0x1Cu) {
    uint32_t offset = (uint32_t)(hw & 0x7FFu) << 1;
    if (offset & 0x800u) {
      offset |= 0xFFFFF000u;
    }
    out->op = MANGO_OP_B;
    out->imm = offset;
    return 0;
  }

  return -1;
}

static uint32_t mango_t32_branch_imm25(uint16_t hw1, uint16_t hw2) {
  uint32_t s = (hw1 >> 10) & 1u;
  uint32_t j1 = (hw2 >> 13) & 1u;
  uint32_t j2 = (hw2 >> 11) & 1u;
  uint32_t i1 = (j1 ^ s) ^ 1u;
  uint32_t i2 = (j2 ^ s) ^ 1u;
  uint32_t imm10 = hw1 & 0x3FFu;
  uint32_t imm11 = hw2 & 0x7FFu;
  uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1);
  if (s) {
    imm |= 0xFE000000u;
  }
  return imm;
}

static uint32_t mango_t32_bcond_imm(uint16_t hw1, uint16_t hw2) {
  /* T32 B<c>.W (T3): imm32 = SignExtend(S:J1:J2:imm6:imm11:0).
   * Unlike unconditional B.W/BL (T4), J1/J2 are NOT inverted via NOT(J EOR S).
   * Q-OTTD-0w: tip previously reused the T4 inversion, so guest f000 8134
   * (beq.w +0x268 in IcuStringIterator::SetString) became +0xc0268 and landed
   * mid A32 ICU body with T=1 (stop word eaffffb2). */
  uint32_t s = (hw1 >> 10) & 1u;
  uint32_t j1 = (hw2 >> 13) & 1u;
  uint32_t j2 = (hw2 >> 11) & 1u;
  uint32_t imm6 = hw1 & 0x3Fu;
  uint32_t imm11 = hw2 & 0x7FFu;
  uint32_t imm = (s << 20) | (j1 << 19) | (j2 << 18) | (imm6 << 12) | (imm11 << 1);
  if (s) {
    imm |= 0xFFE00000u;
  }
  return imm;
}

int mango_decode_t32(uint16_t hw1, uint16_t hw2, MangoInsn* out) {
  mango_insn_clear(out);
  out->cond = 0xE;

  /* Q-OTTD-0d: MOV.W Rd,#<const> modified-imm (Rn=15, S=0).
   * Primary f04f 0800 = mov.w r8,#0; sibling f04f 0825 = mov.w r8,#0x25.
   * Not MOVW (F240). Do not open S=1 / Rn!=15 (ORR) this bite. */
  if ((hw1 & 0xFBE0u) == 0xF040u && (hw1 & 0xFu) == 0xFu && (hw1 & 0x10u) == 0 &&
      (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm = 0;
    if (rd == MANGO_REG_PC) {
      return -1;
    }
    if (mango_thumb_expand_imm(imm12, &imm) != 0) {
      return -1;
    }
    out->op = MANGO_OP_MOV;
    out->rd = rd;
    out->is_imm = 1;
    out->sets_flags = 0;
    out->imm = imm;
    out->shift_amount = 0;
    return 0;
  }

  /* Q-OTTD-0al: ORR.W Rd,Rn,#<const> modified-imm (S=0, Rn≠15).
   * OpenTTD f046 0601 = orr.w r6,r6,#1 (llvm-mc [46,f0,01,06]). Rn=15 is
   * MOV.W (0d). ORRS (S=1) and register ORR (EA4x Rn≠15) stay closed.
   * Reject Rd=PC. */
  if ((hw1 & 0xFBE0u) == 0xF040u && (hw1 & 0xFu) != 0xFu && (hw1 & 0x10u) == 0 &&
      (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t rn = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm = 0;
    if (rd == MANGO_REG_PC) {
      return -1;
    }
    if (mango_thumb_expand_imm(imm12, &imm) != 0) {
      return -1;
    }
    out->op = MANGO_OP_ORR;
    out->rd = rd;
    out->rn = rn;
    out->is_imm = 1;
    out->sets_flags = 0;
    out->imm = imm;
    out->shift_amount = 0;
    return 0;
  }

  /* Q-OTTD-0ao: EOR.W Rd,Rn,#<const> modified-imm (S=0).
   * OpenTTD f083 0301 = eor.w r3,r3,#1 (llvm-mc). EORS (S=1) stays closed.
   * Reject Rd/Rn=PC. */
  if ((hw1 & 0xFBE0u) == 0xF080u && (hw1 & 0x10u) == 0 && (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t rn = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm = 0;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    if (mango_thumb_expand_imm(imm12, &imm) != 0) {
      return -1;
    }
    out->op = MANGO_OP_EOR;
    out->rd = rd;
    out->rn = rn;
    out->is_imm = 1;
    out->sets_flags = 0;
    out->imm = imm;
    out->shift_amount = 0;
    return 0;
  }

  /* Q-OTTD-0d: SUB.W Rd,Rn,#<const> modified-imm (S=0). Guest f5ad 5d08 =
   * sub.w sp,sp,#0x2200 (ThumbExpandImm(0xD08)=0x2200). Plain SUBW #0xD08 is
   * f6ad 5d08 (already handled). SP as Rd/Rn allowed; PC rejected. */
  if ((hw1 & 0xFBE0u) == 0xF1A0u && (hw1 & 0x10u) == 0 && (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t rn = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm = 0;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    if (mango_thumb_expand_imm(imm12, &imm) != 0) {
      return -1;
    }
    out->op = MANGO_OP_SUB;
    out->rd = rd;
    out->rn = rn;
    out->is_imm = 1;
    out->sets_flags = 0;
    out->imm = imm;
    out->shift_amount = 0;
    return 0;
  }

  /* Q-OTTD-0k: CMP.W Rn,#<const> modified-imm (op=1101, S=1, Rd=15 flags-only).
   * Primary f1b5 4f00 = cmp.w r5,#0x80000000 (imm12 0x400 → ThumbExpandImm 0x80000000,
   * NOT #0x400 / f5b5 6f80). Sibling f1b9 0f1a = cmp.w r9,#0x1a.
   * Same op as 0d SUB but S=1 + Rd=15. S=1 Rd≠15 is SUBS.W (Q-OTTD-0as). */
  if ((hw1 & 0xFBE0u) == 0xF1A0u && (hw1 & 0x10u) != 0 && ((hw2 >> 8) & 0xFu) == 0xFu &&
      (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t rn = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm8 = hw2 & 0xFFu;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm = 0;
    if (rn == MANGO_REG_PC) {
      return -1;
    }
    if (mango_thumb_expand_imm(imm12, &imm) != 0) {
      return -1;
    }
    out->op = MANGO_OP_CMP;
    out->rn = rn;
    out->is_imm = 1;
    out->sets_flags = 1;
    out->imm = imm;
    out->shift_amount = 0;
    return 0;
  }

  /* Q-OTTD-0as: SUBS.W Rd,Rn,#<const> modified-imm (S=1, Rd≠15).
   * OpenTTD f1b5 0610 = subs.w r6,r5,#0x10 (llvm-mc [b5,f1,10,06]).
   * Same expand as 0d; sets NZCV. Rd=15 stays CMP. Reject Rd/Rn=PC.
   * Register-form SUBS (EBBx) and RSBS (f1d0) stay closed. */
  if ((hw1 & 0xFBE0u) == 0xF1A0u && (hw1 & 0x10u) != 0 && (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t rn = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm = 0;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    if (mango_thumb_expand_imm(imm12, &imm) != 0) {
      return -1;
    }
    out->op = MANGO_OP_SUB;
    out->rd = rd;
    out->rn = rn;
    out->is_imm = 1;
    out->sets_flags = 1;
    out->imm = imm;
    out->shift_amount = 0;
    return 0;
  }

  /* Q-OTTD-0l-add: ADD.W Rd,Rn,Rm{,shift} register (op=1000, S=0).
   * Guest eb0e 0080 = add.w r0,lr,r0,lsl#2. Opening all ADD.W reg S=0
   * (any shift) also clears drive eb04/eb05. ADDS register is
   * Q-OTTD-0cd. Reject Rd/Rn/Rm=PC. */
  if ((hw1 & 0xFFE0u) == 0xEB00u && (hw1 & 0x10u) == 0 && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_ADD;
    out->rd = rd;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = 0;
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    return 0;
  }

  /* Q-OTTD-0cd: ADDS.W Rd,Rn,Rm{,shift} register (op=1000, S=1).
   * SQStringTable::Add eb1a 0604 = adds.w r6, r10, r4
   * (llvm-mc [1a,eb,04,06]). Rd = Rn + shifted Rm, NZCV update.
   * Rd=15 is CMN and stays closed. Reject Rn/Rm=PC and a register shift. */
  if ((hw1 & 0xFFE0u) == 0xEB00u && (hw1 & 0x10u) != 0 && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_ADD;
    out->rd = rd;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = 1;
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    return 0;
  }

  /* Q-OTTD-0bz: ADC.W Rd,Rn,Rm{,shift} register (op=0100, S=0).
   * RefTable::AllocNodes eb43 0107 = adc.w r1, r3, r7
   * (llvm-mc [43,eb,07,01]). r1 = r3 + r7 + C. ADCS (EB5x) stays
   * closed. Reject Rd/Rn/Rm=PC and a register shift. */
  if ((hw1 & 0xFFE0u) == 0xEB40u && (hw1 & 0x10u) == 0 && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_ADC;
    out->rd = rd;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = 0;
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    return 0;
  }

  /* Q-OTTD-0ca: ADC.W Rd,Rn,#<const> modified-imm (op=1010, S=0).
   * SQLexer::Lex f143 0300 = adc r3, r3, #0 (llvm-mc). The refcount
   * path f143 33ff is adc r3, r3, #0xffffffff. Carry-in comes from
   * the previous adds. ADCS (S=1) stays closed. Reject Rd/Rn=PC. */
  if ((hw1 & 0xFBE0u) == 0xF140u && (hw1 & 0x10u) == 0 && (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t rn = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm = 0;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    if (mango_thumb_expand_imm(imm12, &imm) != 0) {
      return -1;
    }
    out->op = MANGO_OP_ADC;
    out->rd = rd;
    out->rn = rn;
    out->is_imm = 1;
    out->sets_flags = 0;
    out->imm = imm;
    out->shift_amount = 0;
    return 0;
  }

  /* Q-OTTD-0cb: SBCS.W Rd,Rn,#<const> modified-imm (op=1011, S=1).
   * SQTable::SQTable f175 0300 = sbcs r3, r5, #0
   * (llvm-mc [75,f1,00,03]). r3 = r5 - imm - NOT(C), and NZCV update.
   * SBC (S=0, f165) stays closed. Reject Rd/Rn=PC. */
  if ((hw1 & 0xFBE0u) == 0xF160u && (hw1 & 0x10u) != 0 && (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t rn = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm = 0;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    if (mango_thumb_expand_imm(imm12, &imm) != 0) {
      return -1;
    }
    out->op = MANGO_OP_SBC;
    out->rd = rd;
    out->rn = rn;
    out->is_imm = 1;
    out->sets_flags = 1;
    out->imm = imm;
    out->shift_amount = 0;
    return 0;
  }

  /* Q-OTTD-0cc / 0cf: SBC.W Rd,Rn,Rm{,shift} register (op=0110).
   * SQTable eb73 0105 = sbcs.w r1, r3, r5 (S=1). The following
   * hole eb65 0b03 = sbc.w r11, r5, r3 (S=0, llvm-mc [65,eb,03,0b]).
   * Result is Rn - shifted Rm - NOT(C). S=0 leaves NZCV.
   * Reject Rd/Rn/Rm=PC and a register shift. */
  if ((hw1 & 0xFFE0u) == 0xEB60u && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_SBC;
    out->rd = rd;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = (hw1 & 0x10u) != 0;
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    return 0;
  }

  /* Q-OTTD-0be: SUB.W Rd,Rn,Rm{,shift} register, S=0.
   * OpenTTD eba9 7cec = sub.w r12,r9,r12,asr #31 (llvm-mc [a9,eb,ec,7c]).
   * CMP.W register (Rd=15) is Q-OTTD-0bw. SUBS register is Q-OTTD-0bx.
   * Reject Rd/Rn/Rm=PC. */
  if ((hw1 & 0xFFE0u) == 0xEBA0u && (hw1 & 0x10u) == 0 && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_SUB;
    out->rd = rd;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = 0;
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    return 0;
  }

  /* Q-OTTD-0bw / 0bx: SUBS.W register. Rd=15 is CMP.W.
   * ebb0 0f83 = cmp.w r0, r3, lsl #2. ebb3 1202 = subs.w r2, r3, r2, lsl #4
   * (llvm-mc [b3,eb,02,12]). Reject Rn/Rm=PC. */
  if ((hw1 & 0xFFE0u) == 0xEBA0u && (hw1 & 0x10u) != 0 && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = 1;
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    if (rd == MANGO_REG_PC) {
      out->op = MANGO_OP_CMP;
      return 0;
    }
    out->op = MANGO_OP_SUB;
    out->rd = rd;
    return 0;
  }

  /* Q-OTTD-0r: MOV.W Rd,Rm{,shift} register — ORR with Rn=15, S=0.
   * Primary ea4f 7ad0 = mov.w sl,r0,lsr#31; sib ea4f 0847 lsl#1.
   * Rn==15 only. ORR Rn≠15 is Q-OTTD-0aw. MOVS is Q-OTTD-0cg.
   * MVN register is Q-OTTD-0bl (EA6F). Reject Rd/Rm=PC. */
  if ((hw1 & 0xFFE0u) == 0xEA40u && (hw1 & 0xFu) == 0xFu && (hw1 & 0x10u) == 0 &&
      (hw2 & 0x8000u) == 0) {
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rd == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_MOV;
    out->rd = rd;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = 0;
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    return 0;
  }

  /* Q-OTTD-0cg: MOVS.W Rd,Rm{,shift} — ORR with Rn=15, S=1.
   * sq_newclosure ea5f 0c43 = lsls.w r12, r3, #1
   * (llvm-mc [5f,ea,43,0c]). NZ from the result, C from the
   * shifter, V unchanged. ORRS Rn≠15 is 0bf. Reject Rd/Rm=PC. */
  if ((hw1 & 0xFFE0u) == 0xEA40u && (hw1 & 0xFu) == 0xFu && (hw1 & 0x10u) != 0 &&
      (hw2 & 0x8000u) == 0) {
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rd == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_MOV;
    out->rd = rd;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = 1;
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    return 0;
  }

  /* Q-OTTD-0bl: MVN.W Rd,Rm{,shift} register — ORN with Rn=15, S=0.
   * MD5 ea6f 0a02 = mvn.w r10,r2 (llvm-mc [6f,ea,02,0a]).
   * ea6f 70d1 = mvn.w r0,r1,lsr #31. MVNS (EA7F) stays closed.
   * ORN (Rn≠15) is Q-OTTD-0bm. Reject Rd/Rm=PC. */
  if ((hw1 & 0xFFE0u) == 0xEA60u && (hw1 & 0xFu) == 0xFu && (hw1 & 0x10u) == 0 &&
      (hw2 & 0x8000u) == 0) {
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rd == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_MVN;
    out->rd = rd;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = 0;
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    return 0;
  }

  /* Q-OTTD-0bm: ORN.W Rd,Rn,Rm{,shift}, Rn≠15, S=0.
   * MD5 ea63 0101 = orn r1,r3,r1 (llvm-mc [63,ea,01,01]).
   * Rd = Rn | ~Rm. ORNS (EA7x, Rn≠15) stays closed. Rn=15 is MVN.
   * Reject Rd/Rn/Rm=PC. */
  if ((hw1 & 0xFFE0u) == 0xEA60u && (hw1 & 0xFu) != 0xFu && (hw1 & 0x10u) == 0 &&
      (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_ORN;
    out->rd = rd;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = 0;
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    return 0;
  }

  /* Q-OTTD-0aw / 0bf: ORR.W / ORRS.W Rd,Rn,Rm{,shift}, Rn≠15.
   * ea41 1383 = orr.w r3,r1,r3,lsl #6. SDL ea5b 0b07 = orrs.w r11,r11,r7.
   * S is hw1 bit 4. Rn=15 S=0 is MOV.W. Rn=15 S=1 is MOVS (0cg).
   * Reject Rd/Rn/Rm=PC. */
  if ((hw1 & 0xFFE0u) == 0xEA40u && (hw1 & 0xFu) != 0xFu && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_ORR;
    out->rd = rd;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = (int)((hw1 >> 4) & 1u);
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    return 0;
  }

  /* Q-OTTD-0bk: AND.W Rd,Rn,Rm{,shift} register, S=0.
   * MD5 ea0b 0303 = and.w r3,r11,r3 (llvm-mc [0b,ea,03,03]);
   * next in the same round is ea08 030b = and.w r3,r8,r11.
   * ANDS (EA1x) stays closed. Reject Rd/Rn/Rm=PC. */
  if ((hw1 & 0xFFE0u) == 0xEA00u && (hw1 & 0x10u) == 0 && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_AND;
    out->rd = rd;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = 0;
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    return 0;
  }

  /* Q-OTTD-0ay: EOR.W Rd,Rn,Rm{,shift} register, S=0.
   * OpenTTD ea82 0304 = eor.w r3,r2,r4 (llvm-mc [82,ea,04,03]).
   * EORS (EA9x) stays closed. Reject Rd/Rn/Rm=PC. */
  if ((hw1 & 0xFFE0u) == 0xEA80u && (hw1 & 0x10u) == 0 && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_EOR;
    out->rd = rd;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = 0;
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    return 0;
  }

  /* Q-OTTD-0e: ADD.W Rd,Rn,#<const> modified-imm (S=0). Guest f50d 5108 =
   * add.w r1,sp,#0x2200 (ThumbExpandImm(0xD08)=0x2200). Plain ADDW #0xD08 is
   * f60d 5108 (already handled). SP as Rn allowed; PC as Rd/Rn rejected. */
  if ((hw1 & 0xFBE0u) == 0xF100u && (hw1 & 0x10u) == 0 && (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t rn = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm = 0;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    if (mango_thumb_expand_imm(imm12, &imm) != 0) {
      return -1;
    }
    out->op = MANGO_OP_ADD;
    out->rd = rd;
    out->rn = rn;
    out->is_imm = 1;
    out->sets_flags = 0;
    out->imm = imm;
    out->shift_amount = 0;
    return 0;
  }

  /* Q-OTTD-0ak: ADDS.W Rd,Rn,#<const> modified-imm (S=1, Rd≠15).
   * OpenTTD f11a 0600 = adds.w r6,r10,#0 (llvm-mc [1a,f1,00,06]).
   * Same expand as 0e; sets NZCV. Rd=15 is CMN and stays closed.
   * Reject Rd/Rn=PC. Register-form ADDS (EB1x) stays closed. */
  if ((hw1 & 0xFBE0u) == 0xF100u && (hw1 & 0x10u) != 0 && (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t rn = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm = 0;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    if (mango_thumb_expand_imm(imm12, &imm) != 0) {
      return -1;
    }
    out->op = MANGO_OP_ADD;
    out->rd = rd;
    out->rn = rn;
    out->is_imm = 1;
    out->sets_flags = 1;
    out->imm = imm;
    out->shift_amount = 0;
    return 0;
  }

  /* Q-OTTD-0am: RSB.W Rd,Rn,#<const> modified-imm (S=0).
   * OpenTTD f1c0 0401 = rsb.w r4,r0,#1 (llvm-mc [c0,f1,01,04]).
   * Rd = imm - Rn. RSBS (S=1, f1d0) stays closed. Reject Rd/Rn=PC. */
  if ((hw1 & 0xFBE0u) == 0xF1C0u && (hw1 & 0x10u) == 0 && (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t rn = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm = 0;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    if (mango_thumb_expand_imm(imm12, &imm) != 0) {
      return -1;
    }
    out->op = MANGO_OP_RSB;
    out->rd = rd;
    out->rn = rn;
    out->is_imm = 1;
    out->sets_flags = 0;
    out->imm = imm;
    out->shift_amount = 0;
    return 0;
  }

  /* Q-OTTD-0bb: RSB.W Rd,Rn,Rm{,shift} register, S=0.
   * OpenTTD ebc5 05c5 = rsb.w r5,r5,r5,lsl #3 (llvm-mc [c5,eb,c5,05]),
   * r5 = (r5<<3) - r5. Sibling ebc6 1606 is lsl #4. RSBS (EBDx) stays
   * closed. Reject Rd/Rn/Rm=PC. */
  if ((hw1 & 0xFFE0u) == 0xEBC0u && (hw1 & 0x10u) == 0 && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_RSB;
    out->rd = rd;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = 0;
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    return 0;
  }

  /* Q-OTTD-0an / 0az: TBB/TBH. OpenTTD e8df f002 = tbb [pc, r2]
   * (llvm-mc [df,e8,02,f0]). e8df f016 = tbh [pc, r6, lsl #1]
   * (llvm-mc [df,e8,16,f0]) in the deque push switch.
   * hw1 E8D0|Rn, hw2 F000|(H<<4)|Rm. BranchWritePC(PC + 2*table).
   * Rn=PC uses the address of the next byte (addr+4) for both TBB and
   * TBH. Rm=PC rejected. */
  if ((hw1 & 0xFFF0u) == 0xE8D0u && (hw2 & 0xFFE0u) == 0xF000u) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    int tbh = (hw2 & 0x10u) != 0;
    if (rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_TBB;
    out->rn = rn;
    out->rm = rm;
    out->b = tbh;
    return 0;
  }


  /* Q-OTTD-0f: MVN.W Rd,#<const> modified-imm (op=0011, Rn=15, S=0).
   * Guest f46f 5207 = mvn.w r2,#0x21c0 (ThumbExpandImm(0xD07)=0x21C0; NOT #0x87000 /
   * imm12 0xA07 which is f46f 2207). Rn!=15 is ORN — require Rn==15. Do not open S=1. */
  if ((hw1 & 0xFBE0u) == 0xF060u && (hw1 & 0xFu) == 0xFu && (hw1 & 0x10u) == 0 &&
      (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm = 0;
    if (rd == MANGO_REG_PC) {
      return -1;
    }
    if (mango_thumb_expand_imm(imm12, &imm) != 0) {
      return -1;
    }
    out->op = MANGO_OP_MVN;
    out->rd = rd;
    out->is_imm = 1;
    out->sets_flags = 0;
    out->imm = imm;
    out->shift_amount = 0;
    return 0;
  }

  /* Q-OTTD-0q / 0bu: BIC/BICS.W Rd,Rn,#<const> modified-imm.
   * S=0 f026 0603 = bic.w r6,r6,#3. S=1 f03a 0302 = bics r3,r10,#2.
   * S is hw1 bit 4. BIC register is 0au. Reject Rd=PC / Rn=PC. */
  if ((hw1 & 0xFBE0u) == 0xF020u && (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t rn = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm = 0;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    if (mango_thumb_expand_imm(imm12, &imm) != 0) {
      return -1;
    }
    out->op = MANGO_OP_BIC;
    out->rd = rd;
    out->rn = rn;
    out->is_imm = 1;
    out->sets_flags = (int)((hw1 >> 4) & 1u);
    out->imm = imm;
    out->shift_amount = 0;
    return 0;
  }

  /* Q-OTTD-0au: BIC/BICS.W register with a shifted Rm.
   * OpenTTD ea37 0720 = bics.w r7,r7,r0,asr #32 (llvm-mc [37,ea,20,07]).
   * imm2:imm3 of 0 with type ASR is ASR #32 (sign fill). S from hw1 bit 4.
   * Immediate BICS is Q-OTTD-0bu. Reject Rd/Rn/Rm=PC. */
  if ((hw1 & 0xFFE0u) == 0xEA20u && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t imm2 = (hw2 >> 6) & 3u;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_BIC;
    out->rd = rd;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->sets_flags = (hw1 >> 4) & 1u;
    out->shift_type = (hw2 >> 4) & 3u;
    out->shift_amount = (imm3 << 2) | imm2;
    out->shift_by_reg = 0;
    return 0;
  }

  /* Q-OTTD-0ac: AND/ANDS.W Rd,Rn,#<const> modified-imm (op=0000).
   * Primary f013 0301 = ands.w r3,r3,#1; contrast f003 0301 = and.w r3,r3,#1;
   * sib f013 03ff = ands.w r3,r3,#255. Mirror 0q BIC but open both S=0 and S=1 via
   * sets_flags=(hw1>>4)&1. Do NOT steal BIC (F02x/F03x) / ORR / etc.
   * S=1 Rd=15 is TST (Q-OTTD-0ap). S=0 Rd=15 and Rn=PC stay closed. */
  if ((hw1 & 0xFBE0u) == 0xF000u && (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t rn = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    uint32_t imm12 = (i << 11) | (imm3 << 8) | imm8;
    uint32_t imm = 0;
    int s = (hw1 & 0x10u) != 0;
    if (rn == MANGO_REG_PC || (rd == MANGO_REG_PC && !s)) {
      return -1;
    }
    if (mango_thumb_expand_imm(imm12, &imm) != 0) {
      return -1;
    }
    if (rd == MANGO_REG_PC) {
      out->op = MANGO_OP_TST;
      out->rn = rn;
      out->is_imm = 1;
      out->sets_flags = 1;
      out->imm = imm;
      out->shift_amount = 0;
      return 0;
    }
    out->op = MANGO_OP_AND;
    out->rd = rd;
    out->rn = rn;
    out->is_imm = 1;
    out->sets_flags = (hw1 >> 4) & 1u;
    out->imm = imm;
    out->shift_amount = 0;
    return 0;
  }


  /* MOVW: 11110 i 100100 imm4 / 0 imm3 Rd imm8 */
  if ((hw1 & 0xFBF0u) == 0xF240u && (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t imm4 = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    if (rd == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_MOV;
    out->rd = rd;
    out->is_imm = 1;
    out->imm = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
    return 0;
  }

  /* ADDW Rd, Rn, #imm12: 11110 i 100000 Rn / 0 imm3 Rd imm8 */
  if ((hw1 & 0xFBF0u) == 0xF200u && (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t rn = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    if (rd == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_ADD;
    out->rd = rd;
    out->rn = rn;
    out->is_imm = 1;
    out->imm = (i << 11) | (imm3 << 8) | imm8;
    return 0;
  }

  /* SUBW Rd, Rn, #imm12: 11110 i 101010 Rn / 0 imm3 Rd imm8 */
  if ((hw1 & 0xFBF0u) == 0xF2A0u && (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t rn = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    if (rd == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_SUB;
    out->rd = rd;
    out->rn = rn;
    out->is_imm = 1;
    out->imm = (i << 11) | (imm3 << 8) | imm8;
    return 0;
  }

  /* MOVT: 11110 i 101100 imm4 / 0 imm3 Rd imm8 */
  if ((hw1 & 0xFBF0u) == 0xF2C0u && (hw2 & 0x8000u) == 0) {
    uint32_t i = (hw1 >> 10) & 1u;
    uint32_t imm4 = hw1 & 0xFu;
    uint32_t imm3 = (hw2 >> 12) & 7u;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    if (rd == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_MOVT;
    out->rd = rd;
    out->is_imm = 1;
    out->imm = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
    return 0;
  }

  if ((hw1 >> 11) == 0x1Eu) {
    uint32_t b15_14 = hw2 >> 14;
    uint32_t b12 = (hw2 >> 12) & 1u;
    if (b15_14 == 3u && b12 == 1u) {
      out->op = MANGO_OP_BL;
      out->imm = mango_t32_branch_imm25(hw1, hw2);
      return 0;
    }
    if (b15_14 == 3u && b12 == 0u) {
      if (hw2 & 1u) {
        return -1; /* BLX imm H bit must be 0 */
      }
      out->op = MANGO_OP_BLX;
      out->is_imm = 1;
      out->imm = mango_t32_branch_imm25(hw1, hw2);
      return 0;
    }
    if (b15_14 == 2u && b12 == 1u) {
      out->op = MANGO_OP_B;
      out->imm = mango_t32_branch_imm25(hw1, hw2);
      return 0;
    }
    if (b15_14 == 2u && b12 == 0u) {
      uint32_t cond = (hw1 >> 6) & 0xFu;
      /* cond 0xE/0xF share this shape with hints/barriers (e.g. F3BF 8F5F
       * DMB) — do not reject; fall through so later arms can match. */
      if (cond < 0xEu) {
        out->op = MANGO_OP_B;
        out->cond = cond;
        out->imm = mango_t32_bcond_imm(hw1, hw2);
        return 0;
      }
    }
  }


  /* Q-OTTD-0f-ldr / 0y: LDR.W Rt,[Rn,Rm,LSL#imm2] T2 — size=10 L=1, register
   * form bits[11:6]=0, any imm2 0..3 (mirror 0u STR.W / 0m LDRB / 0v STRH).
   * Guest f854 a003 = ldr.w r10,[r4,r3] imm2=0; footnote f853 1021 LSL#2.
   * Tip F880 LDR imm12 (bit7=1); this is F850 (bit7=0). Reject PC.
   * Imm8 P/U/W post-index (f855 2b04 / f85d 4b04) is Q-OTTD-0m below (bit11=1).
   * LDRH.W WB (f834 1f02) cleared by Q-OTTD-0ab. */
  if ((hw1 & 0xFFF0u) == 0xF850u && (hw2 & 0x0FC0u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm2 = (hw2 >> 4) & 3u;
    if (rt == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_LDR;
    out->rd = rt;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->shift_type = 0; /* LSL */
    out->shift_amount = imm2;
    out->p = 1;
    out->u = 1;
    out->w = 0;
    out->b = 0;
    return 0;
  }

  /* Q-OTTD-0m-ldr / 0ag: T32 LDR.W Rt,[Rn,#±imm8]!? T4 — bit11=1 full P/U/W.
   * Post (0m) f855 2b04 = ldr.w r2,[r5],#4 and f85d 4b04 = ldr.w r4,[sp],#4.
   * Pre (0ag) OpenTTD f85a 3f04 = ldr.w r3,[r10,#4]! (P=1 U=1 W=1). Also
   * covers U=0 no-WB. Mirror 0s STR.W imm8 (F840). Mutually exclusive with
   * 0f reg form (bits[11:6]==0). Font::getFontTable f85d fb04 =
   * ldr pc, [sp], #4 (Q-OTTD-0br); execute already interworks on bit 0.
   * Reject LDRT (P=0 W=0), Rn=PC, and writeback into Rt. */
  if ((hw1 & 0xFFF0u) == 0xF850u && (hw2 & 0x0800u) != 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    int p = (int)((hw2 >> 10) & 1u);
    int u = (int)((hw2 >> 9) & 1u);
    int w = (int)((hw2 >> 8) & 1u);
    if (p == 0 && w == 0) {
      return -1; /* LDRT */
    }
    if (rn == MANGO_REG_PC) {
      return -1;
    }
    if (w && rt == rn) {
      return -1; /* WB into Rt UNPRED */
    }
    out->op = MANGO_OP_LDR;
    out->rd = rt;
    out->rn = rn;
    out->is_imm = 1;
    out->imm = imm8;
    out->p = p;
    out->u = u;
    out->w = w;
    out->b = 0;
    return 0;
  }

  /* Q-OTTD-0g: T32 LDRSH.W Rt,[Rn,#imm12] T1 — F9B0 class (signed halfword).
   * Guest f9bd b034 = ldrsh.w r11,[sp,#0x34]; sibling f9bd a020.
   * Tip F880 covers unsigned LDRH (F8B0), not F9B0. Reject Rt/Rn=PC. */
  if ((hw1 & 0xFFF0u) == 0xF9B0u) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t imm12 = hw2 & 0xFFFu;
    if (rt == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_LDRSH;
    out->rd = rt;
    out->rn = rn;
    out->is_imm = 1;
    out->imm = imm12;
    out->p = 1;
    out->u = 1;
    out->w = 0;
    return 0;
  }

  /* Q-OTTD-0aj: T32 LDRSB.W Rt,[Rn,#imm12] T1 — F990 class.
   * OpenTTD f992 3000 = ldrsb.w r3,[r2] (imm12=0). Mirror 0g LDRSH (F9B0).
   * Reuse MANGO_OP_LDRSB (byte sign-extend). Reject Rt/Rn=PC. Not the
   * register form (F910 bit11=0) and not writeback. */
  if ((hw1 & 0xFFF0u) == 0xF990u) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t imm12 = hw2 & 0xFFFu;
    if (rt == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_LDRSB;
    out->rd = rt;
    out->rn = rn;
    out->is_imm = 1;
    out->imm = imm12;
    out->p = 1;
    out->u = 1;
    out->w = 0;
    return 0;
  }

  /* Q-OTTD-0s / widen 0g-str: T32 STR.W Rt,[Rn,#±imm8]!? T4 — bit11=1 P/U/W.
   * Guest f840 1f0c = str.w r1,[r0,#12]! (P=1 U=1 W=1). Also covers tip 0g
   * f849 6c3c / f840 1c0c (P=1 U=0 W=0), post 1b0c, neg WB 1d0c.
   * Distinct from 0i reg form (bit11=0). Reject STRT (P=0 W=0) and Rt/Rn=PC.
   * Do not open LDR.W-reg / STRH this bite; LDRH.W WB is 0ab. */
  if ((hw1 & 0xFFF0u) == 0xF840u && (hw2 & 0x0800u) != 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    int p = (int)((hw2 >> 10) & 1u);
    int u = (int)((hw2 >> 9) & 1u);
    int w = (int)((hw2 >> 8) & 1u);
    if (p == 0 && w == 0) {
      return -1; /* STRT */
    }
    if (rt == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_STR;
    out->rd = rt;
    out->rn = rn;
    out->is_imm = 1;
    out->imm = imm8; /* byte offset, not shifted */
    out->p = p;
    out->u = u;
    out->w = w;
    out->b = 0;
    return 0;
  }

  /* Q-OTTD-0z: T32 STRB.W Rt,[Rn,#±imm8]!? T4 — size=00 L=0, bit11=1 P/U/W.
   * Guest f804 3b01 = strb.w r3,[r4],#1 (P=0 U=1 W=1). Also SDL uncover
   * f809 0c04 (P=1 U=0 W=0), pre WB f804 3f01, neg offset f804 3c01.
   * Mirror 0s STR.W imm8 (F840); reuse MANGO_OP_STR with b=1. Execute already
   * honors p/u/w + byte. Distinct from F880 STRB imm12 (bit7=1) and F810 LDRB
   * reg (bit11=0). Reject STRBT (P=0 W=0) and Rt/Rn=PC. LDRB imm8 T4 is 0aa;
   * LDRH.W imm8 T4 is 0ab; hold T32 CLZ. */
  if ((hw1 & 0xFFF0u) == 0xF800u && (hw2 & 0x0800u) != 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    int p = (int)((hw2 >> 10) & 1u);
    int u = (int)((hw2 >> 9) & 1u);
    int w = (int)((hw2 >> 8) & 1u);
    if (p == 0 && w == 0) {
      return -1; /* STRBT */
    }
    if (rt == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_STR;
    out->rd = rt;
    out->rn = rn;
    out->is_imm = 1;
    out->imm = imm8; /* byte offset, not shifted */
    out->p = p;
    out->u = u;
    out->w = w;
    out->b = 1;
    return 0;
  }

  /* Q-OTTD-0ad: T32 STRB.W Rt,[Rn,Rm,LSL#imm2] T2 — size=00 L=0, register form
   * bits[11:6]=0, any imm2 0..3 (mirror 0v STRH.W / 0u STR.W / 0m LDRB.W).
   * Guest f804 b003 = strb.w fp,[r4,r3] imm2=0; also f804 b013 LSL#1 etc.
   * Mutually exclusive with tip 0z STRB.W imm8 (bit11=1) and F880 imm12 (bit7=1).
   * Reuse MANGO_OP_STR b=1 is_imm=0; execute already honors reg+shift via
   * mango_eval_operand2. Reject Rt/Rn/Rm=PC. STRH.W imm8 is 0ae; hold CLZ. */
  if ((hw1 & 0xFFF0u) == 0xF800u && (hw2 & 0x0FC0u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm2 = (hw2 >> 4) & 3u;
    if (rt == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_STR;
    out->rd = rt;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->shift_type = 0; /* LSL */
    out->shift_amount = imm2;
    out->p = 1;
    out->u = 1;
    out->w = 0;
    out->b = 1;
    return 0;
  }

  /* Q-OTTD-0i / 0u: T32 STR.W Rt,[Rn,Rm,LSL#imm2] T2 — size=10 L=0,
   * hw2 bits[11:6]=0 (register form), any imm2 0..3 (like 0m LDRB.W).
   * Guest f842 b006 = str.w fp,[r2,r6] imm2=0; tip 0i f840 4025 LSL#2.
   * Distinct from 0s/0g (bit11=1 imm8). Reject PC. STRB-reg is 0ad; hold STRH imm8. */
  if ((hw1 & 0xFFF0u) == 0xF840u && (hw2 & 0x0FC0u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm2 = (hw2 >> 4) & 3u;
    if (rt == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_STR;
    out->rd = rt;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->shift_type = 0; /* LSL */
    out->shift_amount = imm2;
    out->p = 1;
    out->u = 1;
    out->w = 0;
    out->b = 0;
    return 0;
  }

  /* Q-OTTD-0v: T32 STRH.W Rt,[Rn,Rm,LSL#imm2] T2 — size=01 L=0, register form.
   * Guest f822 600c = strh.w r6,[r2,ip] imm2=0; footnote f820 6012 LSL#1.
   * Open any imm2 0..3 (mirror 0u STR.W / 0m LDRB.W). Execute must honor
   * shift_amount. Distinct from F8A0 STRH imm12 (bit7=1) and F840 STR.W (0u).
   * Reject Rt/Rn/Rm=PC. STRH.W imm8 T4 is 0ae (bit11=1). */
  if ((hw1 & 0xFFF0u) == 0xF820u && (hw2 & 0x0FC0u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm2 = (hw2 >> 4) & 3u;
    if (rt == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_STRH;
    out->rd = rt;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->shift_type = 0; /* LSL */
    out->shift_amount = imm2;
    out->p = 1;
    out->u = 1;
    out->w = 0;
    out->b = 0;
    return 0;
  }

  /* Q-OTTD-0ae: T32 STRH.W Rt,[Rn,#±imm8]!? T4 — size=01 L=0, bit11=1 P/U/W.
   * Guest f823 2c08 = strh.w r2,[r3,#-8] (P=1 U=0 W=0). Also covers post
   * f824 1b02 and pre WB f824 1f02. Mirror 0ab LDRH.W imm8 (F830) / 0z STRB
   * imm8 (F800); reuse MANGO_OP_STRH. Execute already honors p/u/w + halfword.
   * Distinct from F8A0 STRH imm12 (bit7=1) and 0v STRH-reg (bits[11:6]==0).
   * Reject STRHT (P=0 W=0), Rt/Rn=PC, and writeback into Rt (W=1 && Rt==Rn).
   * Do not open T32 CLZ / TBB / BFI / T16 REV16. */
  if ((hw1 & 0xFFF0u) == 0xF820u && (hw2 & 0x0800u) != 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    int p = (int)((hw2 >> 10) & 1u);
    int u = (int)((hw2 >> 9) & 1u);
    int w = (int)((hw2 >> 8) & 1u);
    if (p == 0 && w == 0) {
      return -1; /* STRHT */
    }
    if (rt == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    if (w && rt == rn) {
      return -1; /* WB into Rt UNPRED for STRH */
    }
    out->op = MANGO_OP_STRH;
    out->rd = rt;
    out->rn = rn;
    out->is_imm = 1;
    out->imm = imm8; /* byte offset, not <<1 */
    out->p = p;
    out->u = u;
    out->w = w;
    out->b = 0;
    return 0;
  }

  /* Q-OTTD-0aa: T32 LDRB.W Rt,[Rn,#±imm8]!? T4 — size=00 L=1, bit11=1 P/U/W.
   * Guest f810 3b01 = ldrb.w r3,[r0],#1 (P=0 U=1 W=1). Also covers pre WB
   * f810 3f01 and U=0 no-WB f810 3c01. Mirror 0z STRB.W imm8 (F800); reuse
   * MANGO_OP_LDR with b=1. Execute already honors p/u/w + byte. Distinct from
   * F890 LDRB imm12 (bit7=1) and 0m LDRB-reg (bit11=0). Reject LDRBT (P=0 W=0)
   * and Rt/Rn=PC. LDRH.W imm8 is 0ab; hold T32 CLZ. */
  if ((hw1 & 0xFFF0u) == 0xF810u && (hw2 & 0x0800u) != 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    int p = (int)((hw2 >> 10) & 1u);
    int u = (int)((hw2 >> 9) & 1u);
    int w = (int)((hw2 >> 8) & 1u);
    if (p == 0 && w == 0) {
      return -1; /* LDRBT */
    }
    if (rt == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_LDR;
    out->rd = rt;
    out->rn = rn;
    out->is_imm = 1;
    out->imm = imm8; /* byte offset, not shifted */
    out->p = p;
    out->u = u;
    out->w = w;
    out->b = 1;
    return 0;
  }

  /* Q-OTTD-0ab: T32 LDRH.W Rt,[Rn,#±imm8]!? T4 — size=01 L=1, bit11=1 P/U/W.
   * Guest f834 1f02 = ldrh.w r1,[r4,#2]! (P=1 U=1 W=1). Also covers post
   * f834 1b02 and U=0 no-WB f834 1c02. Mirror 0aa LDRB.W imm8 (F810); reuse
   * MANGO_OP_LDRH. Execute already honors p/u/w + halfword. Distinct from
   * F8B0 LDRH imm12 (bit7=1) and 0v STRH-reg (F820 bit11=0). LDRH.W
   * register (bits[11:6]==0) is Q-OTTD-0by. Reject LDRHT
   * (P=0 W=0), Rt/Rn=PC, and writeback into Rt (W=1 && Rt==Rn UNPRED).
   * Hold T32 CLZ; STRH.W imm8 is 0ae. */
  if ((hw1 & 0xFFF0u) == 0xF830u && (hw2 & 0x0800u) != 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    int p = (int)((hw2 >> 10) & 1u);
    int u = (int)((hw2 >> 9) & 1u);
    int w = (int)((hw2 >> 8) & 1u);
    if (p == 0 && w == 0) {
      return -1; /* LDRHT */
    }
    if (rt == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    if (w && rt == rn) {
      return -1; /* WB into dest UNPRED for LDRH */
    }
    out->op = MANGO_OP_LDRH;
    out->rd = rt;
    out->rn = rn;
    out->is_imm = 1;
    out->imm = imm8; /* byte offset, not shifted */
    out->p = p;
    out->u = u;
    out->w = w;
    return 0;
  }

  /* Q-OTTD-0by: T32 LDRH.W Rt,[Rn,Rm,LSL#imm2] T2 — size=01 L=1,
   * register form bits[11:6]=0. ConvertDateToYMD f832 3014 =
   * ldrh.w r3, [r2, r4, lsl #1] (llvm-mc [32,f8,14,30]).
   * Mirror 0v STRH.W reg (F820) and 0m LDRB.W reg (F810).
   * Mutually exclusive with 0ab imm8 (bit11=1) and F8B0 imm12 (bit7=1).
   * Reject Rt/Rn/Rm=PC. LDRSH register (F930, bit11=0) stays closed. */
  if ((hw1 & 0xFFF0u) == 0xF830u && (hw2 & 0x0FC0u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm2 = (hw2 >> 4) & 3u;
    if (rt == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_LDRH;
    out->rd = rt;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->shift_type = 0; /* LSL */
    out->shift_amount = imm2;
    out->p = 1;
    out->u = 1;
    out->w = 0;
    return 0;
  }

  /* Q-OTTD-0bn: T32 LDRSH.W Rt,[Rn,#±imm8]!? T4 — F930 class, bit11=1.
   * NWidget f934 ec04 = ldrsh.w lr,[r4,#-4] (P=1 U=0 W=0). Sibling
   * f934 3c04 = ldrsh.w r3,[r4,#-4]. Mirror 0ab LDRH imm8; reuse
   * MANGO_OP_LDRSH (sign-extend). Distinct from F9B0 imm12. Reject
   * LDRSHT (P=0 W=0), Rt/Rn=PC, and writeback into Rt. */
  if ((hw1 & 0xFFF0u) == 0xF930u && (hw2 & 0x0800u) != 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    int p = (int)((hw2 >> 10) & 1u);
    int u = (int)((hw2 >> 9) & 1u);
    int w = (int)((hw2 >> 8) & 1u);
    if (p == 0 && w == 0) {
      return -1; /* LDRSHT */
    }
    if (rt == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    if (w && rt == rn) {
      return -1;
    }
    out->op = MANGO_OP_LDRSH;
    out->rd = rt;
    out->rn = rn;
    out->is_imm = 1;
    out->imm = imm8;
    out->p = p;
    out->u = u;
    out->w = w;
    return 0;
  }

  /* Q-OTTD-0m: T32 LDRB.W Rt,[Rn,Rm,LSL#imm2] T2 — size=00 L=1, register form.
   * Guest f81b 0032 = ldrb.w r0,[r11,r2,lsl#3]; sib f81b 8003 imm2=0.
   * Open any imm2 0..3 (execute honors shift_amount). Reject Rt/Rn/Rm=PC.
   * Distinct from F890 LDRB imm12 (bit7=1). LDRH.W register is Q-OTTD-0by.
   * LDRSB register stays closed. */
  if ((hw1 & 0xFFF0u) == 0xF810u && (hw2 & 0x0FC0u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    uint32_t imm2 = (hw2 >> 4) & 3u;
    if (rt == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_LDR;
    out->rd = rt;
    out->rn = rn;
    out->rm = rm;
    out->is_imm = 0;
    out->shift_type = 0; /* LSL */
    out->shift_amount = imm2;
    out->p = 1;
    out->u = 1;
    out->w = 0;
    out->b = 1;
    return 0;
  }

  /* LDR/STR/LDRB/STRB/LDRH/STRH imm12: 11111 000 1 size L Rn / Rt imm12.
   * Rn=15 + LDR is the literal form (U in bit 7). */
  if ((hw1 & 0xFF80u) == 0xF880u) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t kind = (hw1 >> 4) & 7u;
    uint32_t imm12 = hw2 & 0xFFFu;
    if (rt == MANGO_REG_PC) {
      return -1;
    }
    out->rd = rt;
    out->rn = rn;
    out->is_imm = 1;
    out->imm = imm12;
    out->p = 1;
    out->w = 0;
    if (rn == MANGO_REG_PC) {
      if (kind != 5u) {
        return -1; /* only LDR literal in this subset */
      }
      out->op = MANGO_OP_LDR;
      out->u = (int)((hw1 >> 7) & 1u);
      return 0;
    }
    out->u = 1;
    switch (kind) {
      case 0:
        out->op = MANGO_OP_STR;
        out->b = 1;
        return 0;
      case 1:
        out->op = MANGO_OP_LDR;
        out->b = 1;
        return 0;
      case 2:
        out->op = MANGO_OP_STRH;
        return 0;
      case 3:
        out->op = MANGO_OP_LDRH;
        return 0;
      case 4:
        out->op = MANGO_OP_STR;
        return 0;
      case 5:
        out->op = MANGO_OP_LDR;
        return 0;
      default:
        return -1;
    }
  }


  /* Q-OTTD-0at: T32 LDREX/STREX word. Must precede the STRD mask
   * (hw1&0xFE50)==0xE840, which also covers STREX hw1 E84x and rejects it
   * as P=0 W=0. OpenTTD e853 2f00 = ldrex r2,[r3] (llvm-mc [53,e8,00,2f]);
   * e843 1e00 = strex lr,r1,[r3]. Single-thread: STREX status is 0.
   * Byte/half/double and CLREX stay closed. Reject Rn/Rt=PC; status may be LR. */
  if ((hw1 & 0xFFF0u) == 0xE850u && (hw2 & 0x0FFFu) == 0x0F00u) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    if (rn == MANGO_REG_PC || rt == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_LDREX;
    out->rn = rn;
    out->rd = rt;
    out->b = 0; /* word */
    return 0;
  }
  if ((hw1 & 0xFFF0u) == 0xE840u && (hw2 & 0x00FFu) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    if (rn == MANGO_REG_PC || rt == MANGO_REG_PC || rd == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_STREX;
    out->rn = rn;
    out->rd = rd;
    out->rm = rt;
    out->b = 0;
    return 0;
  }

  /* Q-OTTD-0t / widen 0e-strd: T32 STRD imm T1 L=0 — general P/U/W from hw1.
   * Guest e946 4502 = strd r4,r5,[r6,#-8] (P=1 U=0 W=0). Also tip 0e e9c1/e9c6
   * (P=1 U=1 W=0), optional e966 (neg WB) / e866 (post). Mask (hw1&0xFE50)==0xE840
   * → 1110100 P U 1 W 0 Rn (bit6=1 L=0; excludes LDRD E9D0/E950 and STM bit6=0).
   * Reject undefined P==0&&W==0; even-pair Rt; Rt/Rn/Rt2!=PC. Reuse MANGO_OP_STRD. */
  if ((hw1 & 0xFE50u) == 0xE840u) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t rt2 = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    int p = (int)((hw1 >> 8) & 1u);
    int u = (int)((hw1 >> 7) & 1u);
    int w = (int)((hw1 >> 5) & 1u);
    if (p == 0 && w == 0) {
      return -1; /* undefined STRD form */
    }
    if ((rt & 1u) != 0 || rt2 != rt + 1u) {
      return -1; /* even pair only — matches A32 STRD execute */
    }
    if (rn == MANGO_REG_PC || rt == MANGO_REG_PC || rt2 == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_STRD;
    out->rn = rn;
    out->rd = rt;
    out->is_imm = 1;
    out->imm = imm8 << 2;
    out->p = p;
    out->u = u;
    out->w = w;
    return 0;
  }

  /* Q-OTTD-0n / 0ce / 0ci / 0cj: T32 LDRD immediate.
   * e9d1 2302 = ldrd r2,r3,[r1,#8]. e9f5 8940 = ldrd r8,r9,[r5,#256]!.
   * CommaExpr e8f5 2308 = ldrd r2,r3,[r5],#32 (llvm-mc [f5,e8,08,23]).
   * P=0 loads at the old Rn, then W=1 adds the offset. P=0 W=0 is
   * undefined. W=1 with Rn equal to Rt or Rt2 is UNPREDICTABLE. */
  if ((hw1 & 0xFE50u) == 0xE850u) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rt = (hw2 >> 12) & 0xFu;
    uint32_t rt2 = (hw2 >> 8) & 0xFu;
    uint32_t imm8 = hw2 & 0xFFu;
    int p = (int)((hw1 >> 8) & 1u);
    int w = (int)((hw1 >> 5) & 1u);
    if (p == 0 && w == 0) {
      return -1;
    }
    if ((rt & 1u) != 0 || rt2 != rt + 1u) {
      return -1; /* even pair only — matches A32 LDRD execute */
    }
    if (rn == MANGO_REG_PC || rt == MANGO_REG_PC || rt2 == MANGO_REG_PC) {
      return -1;
    }
    if (w && (rn == rt || rn == rt2)) {
      return -1;
    }
    out->op = MANGO_OP_LDRD;
    out->rn = rn;
    out->rd = rt;
    out->is_imm = 1;
    out->imm = imm8 << 2;
    out->p = p;
    out->u = (int)((hw1 >> 7) & 1u);
    out->w = w;
    return 0;
  }

  /* Q-OTTD-0a: T32 STMDB / PUSH.W SP! — hw1 E92D form, Rn=SP, W=1, P(hw2)=0.
   * Reuse MANGO_OP_STM (p=1 u=0 w=1). Not full T32 LDM/STMIA. */
  if ((hw1 & 0xFFD0u) == 0xE900u && (hw1 & 0x0020u) != 0 && (hw1 & 0xFu) == MANGO_REG_SP &&
      (hw2 & 0x8000u) == 0) {
    uint32_t reglist = hw2 & 0x7FFFu; /* M<<14 | R[12:0]; PC forbidden */
    if (reglist == 0) {
      return -1; /* empty list UNPRED */
    }
    out->op = MANGO_OP_STM;
    out->rn = MANGO_REG_SP;
    out->reglist = reglist;
    out->p = 1;
    out->u = 0;
    out->w = 1;
    return 0;
  }


  /* Q-OTTD-0j / 0ai: T32 STMIA Rn{!} — W=0 guest e880 002c, W=1 OpenTTD
   * e8ac 000f = stmia.w r12!, {r0-r3} (llvm-mc [ac,e8,0f,00]). Reuse
   * MANGO_OP_STM (p=0 u=1). Reject PC in the list, Rn=PC, empty list, and
   * writeback with a non-SP Rn in the list. STMDB non-SP stays closed. */
  if ((hw1 & 0xFFD0u) == 0xE880u && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t reglist = hw2 & 0x7FFFu;
    int w = (hw1 & 0x0020u) != 0;
    if (rn == MANGO_REG_PC || reglist == 0) {
      return -1;
    }
    if (w && (reglist & (1u << rn)) && rn != MANGO_REG_SP) {
      return -1;
    }
    out->op = MANGO_OP_STM;
    out->rn = rn;
    out->reglist = reglist;
    out->p = 0;
    out->u = 1;
    out->w = w;
    return 0;
  }

  /* Q-OTTD-0c / 0ah: T32 LDMIA Rn{!} without PC. POP.W is Rn=SP W=1
   * (e8bd 4ff8). OpenTTD e8be 000f = ldmia.w lr!, {r0-r3}; the next word
   * e89e 000f = ldmia.w lr, {r0-r3} (W=0). p=0 u=1. Reject empty list,
   * Rn=PC, and writeback with a non-SP Rn in the list. SP-in-list stays
   * allowed. PC-in-list is 0i-pop (SP only). */
  if ((hw1 & 0xFFD0u) == 0xE890u && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t reglist = hw2 & 0x7FFFu; /* M<<14 | R[12:0]; PC forbidden here */
    int w = (hw1 & 0x0020u) != 0;
    if (rn == MANGO_REG_PC || reglist == 0) {
      return -1;
    }
    if (w && (reglist & (1u << rn)) && rn != MANGO_REG_SP) {
      return -1; /* WB with Rn in the list is UNPRED except SP */
    }
    out->op = MANGO_OP_LDM;
    out->rn = rn;
    out->reglist = reglist;
    out->p = 0;
    out->u = 1;
    out->w = w;
    return 0;
  }

  /* Q-OTTD-0bo: T32 LDMDB Rn{!} without PC. NWidget e914 0006 =
   * ldmdb r4, {r1, r2} (llvm-mc [14,e9,06,00]). p=1 u=0. W is hw1 bit 5;
   * e934 0006 is ldmdb r4!, {r1, r2}. STMDB is Q-OTTD-0bt.
   * Reject empty list, Rn=PC, PC in the list, and writeback with a
   * non-SP Rn also in the list. */
  if ((hw1 & 0xFFD0u) == 0xE910u && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t reglist = hw2 & 0x7FFFu;
    int w = (hw1 & 0x0020u) != 0;
    if (rn == MANGO_REG_PC || reglist == 0) {
      return -1;
    }
    if (w && (reglist & (1u << rn)) && rn != MANGO_REG_SP) {
      return -1;
    }
    out->op = MANGO_OP_LDM;
    out->rn = rn;
    out->reglist = reglist;
    out->p = 1;
    out->u = 0;
    out->w = w;
    return 0;
  }

  /* Q-OTTD-0bt: T32 STMDB Rn{!} without PC. Guest e903 0006 =
   * stmdb r3, {r1, r2} (llvm-mc). p=1 u=0. Lowest reg at the lowest
   * address. Reject empty list, Rn=PC, PC in the list, and writeback
   * with a non-SP Rn also in the list. */
  if ((hw1 & 0xFFD0u) == 0xE900u && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t reglist = hw2 & 0x7FFFu;
    int w = (hw1 & 0x0020u) != 0;
    if (rn == MANGO_REG_PC || reglist == 0) {
      return -1;
    }
    if (w && (reglist & (1u << rn)) && rn != MANGO_REG_SP) {
      return -1;
    }
    if (reglist & (1u << MANGO_REG_PC)) {
      return -1;
    }
    out->op = MANGO_OP_STM;
    out->rn = rn;
    out->reglist = reglist;
    out->p = 1;
    out->u = 0;
    out->w = w;
    return 0;
  }

  /* Q-OTTD-0i-pop: T32 LDMIA / POP.W SP! with PC in list (P=1, M=0).
   * Guest e8bd 81f0 = pop.w {r4-r8,pc}. Keep bit15 in reglist for LoadWritePC
   * (execute already sets/clears CPSR.T from loaded word bit0). Reject LR+PC. */
  if ((hw1 & 0xFFD0u) == 0xE890u && (hw1 & 0x0020u) != 0 && (hw1 & 0xFu) == MANGO_REG_SP &&
      (hw2 & 0x8000u) != 0 && (hw2 & 0x4000u) == 0) {
    uint32_t reglist = hw2 & 0xFFFFu; /* MUST keep bit15 */
    if (reglist == 0) {
      return -1; /* empty list UNPRED */
    }
    out->op = MANGO_OP_LDM;
    out->rn = MANGO_REG_SP;
    out->reglist = reglist;
    out->p = 0;
    out->u = 1;
    out->w = 1;
    return 0;
  }

  /* Q-OTTD-0b / 0av: T32 MUL (Ra=15) and MLA (Ra≠15), op2=0000, S=0.
   * mul.w r1,r1,r4 = fb01 f104. OpenTTD fb01 3102 = mla r1,r1,r2,r3
   * (Rd = Rn*Rm + Ra). MLS (op2=0001) is Q-OTTD-0bq. */
  if ((hw1 & 0xFFF0u) == 0xFB00u && (hw2 & 0x00F0u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t ra = (hw2 >> 12) & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = (ra == 0xFu) ? MANGO_OP_MUL : MANGO_OP_MLA;
    out->rd = rd;
    out->rm = rn; /* multiplicand Rn → interp rm lane */
    out->rs = rm; /* multiplicand Rm → interp rs lane */
    out->rn = (ra == 0xFu) ? 0u : ra;
    out->sets_flags = 0;
    return 0;
  }

  /* Q-OTTD-0bq: T32 MLS Rd,Rn,Rm,Ra. ResizeWindow fb09 6610 =
   * mls r6,r9,r0,r6 (llvm-mc [09,fb,10,66]). Rd = Ra - Rn*Rm.
   * op2=0001. Ra=15 is not MLS. Reject Rd/Rn/Rm/Ra=PC. */
  if ((hw1 & 0xFFF0u) == 0xFB00u && (hw2 & 0x00F0u) == 0x0010u) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t ra = (hw2 >> 12) & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC || ra == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_MLS;
    out->rd = rd;
    out->rm = rn;
    out->rs = rm;
    out->rn = ra;
    out->sets_flags = 0;
    return 0;
  }

  /* Q-OTTD-0bd: T32 SMULL RdLo,RdHi,Rn,Rm. OpenTTD fb8c 8900 =
   * smull r8,r9,r12,r0 (llvm-mc [8c,fb,00,89]). RdLo→rd, RdHi→rn,
   * Rn→rm, Rm→rs. SMLAL/UMLAL stay closed. UMULL is 0bj (FBA0). */
  if ((hw1 & 0xFFF0u) == 0xFB80u && (hw2 & 0x00F0u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rdlo = (hw2 >> 12) & 0xFu;
    uint32_t rdhi = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    if (rdlo == MANGO_REG_PC || rdhi == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC ||
        rdlo == rdhi) {
      return -1;
    }
    out->op = MANGO_OP_SMULL;
    out->rd = rdlo;
    out->rn = rdhi;
    out->rm = rn;
    out->rs = rm;
    out->sets_flags = 0;
    return 0;
  }

  /* Q-OTTD-0bj: T32 UMULL RdLo,RdHi,Rn,Rm. OpenTTD fba4 4505 =
   * umull r4,r5,r4,r5 (llvm-mc [a4,fb,05,45]); next is fba2 2306
   * umull r2,r3,r2,r6. Same lanes as SMULL. Unsigned product.
   * UMLAL (FBE0) and SMLAL (FBC0) stay closed. */
  if ((hw1 & 0xFFF0u) == 0xFBA0u && (hw2 & 0x00F0u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rdlo = (hw2 >> 12) & 0xFu;
    uint32_t rdhi = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    if (rdlo == MANGO_REG_PC || rdhi == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC ||
        rdlo == rdhi) {
      return -1;
    }
    out->op = MANGO_OP_UMULL;
    out->rd = rdlo;
    out->rn = rdhi;
    out->rm = rn;
    out->rs = rm;
    out->sets_flags = 0;
    return 0;
  }

  /* Q-OTTD-0bi: T32 SMULxy. SDL fb12 f000 = smulbb r0,r2,r0
   * (llvm-mc [12,fb,00,f0]). 16x16 signed, N=hw2 bit5, M=hw2 bit4.
   * Ra=15. SMLAxy is Q-OTTD-0bp. Reject Rd/Rn/Rm=PC. */
  if ((hw1 & 0xFFF0u) == 0xFB10u && (hw2 & 0xF0C0u) == 0xF000u) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_SMUL;
    out->rd = rd;
    out->rm = rn;
    out->rs = rm;
    out->b = (int)((hw2 >> 5) & 1u);
    out->u = (int)((hw2 >> 4) & 1u);
    out->sets_flags = 0;
    return 0;
  }

  /* Q-OTTD-0bp: T32 SMLAxy Rd,Rn,Rm,Ra. WindowDesc fb12 3300 =
   * smlabb r3,r2,r0,r3 (llvm-mc [12,fb,00,33]). Rd = Ra + bottom/top
   * 16x16 product. N=hw2 bit5, M=hw2 bit4. Ra is hw2[15:12], not 15
   * (that is SMUL). Reject Rd/Rn/Rm=PC. */
  if ((hw1 & 0xFFF0u) == 0xFB10u && (hw2 & 0x00C0u) == 0 && ((hw2 >> 12) & 0xFu) != 0xFu) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t ra = (hw2 >> 12) & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_SMLA;
    out->rd = rd;
    out->rn = ra;
    out->rm = rn;
    out->rs = rm;
    out->b = (int)((hw2 >> 5) & 1u);
    out->u = (int)((hw2 >> 4) & 1u);
    out->sets_flags = 0;
    return 0;
  }

  /* Q-OTTD-0o: T32 UXTH.W Rd,Rm{,ROR#} — Rn=15 no accumulate.
   * Guest fa1f fa81 = uxth.w r10,r1 (rot=0); sib fa1f fa8a = uxth.w r10,r10.
   * Encoding (hw2): 1111 | Rd | 10 | rotate | Rm — rot in bits[5:4];
   * bits[7:6] fixed 10. imm=((hw2>>4)&3)*8 (execute already RORs).
   * Map → XTEND u=1 b=1 rn=15. UXTB.W is Q-OTTD-0bg. SXTH.W/SXTB.W
   * (FA0F/FA4F) and accumulate Rn≠15 stay closed. Reject Rd/Rm=PC. */
  if (hw1 == 0xFA1Fu && (hw2 & 0xF0C0u) == 0xF080u) {
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    if (rd == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_XTEND;
    out->rd = rd;
    out->rm = rm;
    out->rn = 15u; /* no accumulate */
    out->imm = ((hw2 >> 4) & 3u) * 8u;
    out->u = 1;
    out->b = 1; /* halfword */
    out->sets_flags = 0;
    return 0;
  }

  /* Q-OTTD-0bg: T32 UXTB.W Rd,Rm{,ROR#} — Rn=15, unsigned byte.
   * SDL fa5f f588 = uxtb.w r5,r8 (llvm-mc [5f,fa,88,f5]); sib fa5f fb86.
   * Same hw2 shape as UXTH.W. b=0. UXTAB (Rn≠15) is Q-OTTD-0ch.
   * SXTB and SXTAH stay closed. */
  if (hw1 == 0xFA5Fu && (hw2 & 0xF0C0u) == 0xF080u) {
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    if (rd == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_XTEND;
    out->rd = rd;
    out->rm = rm;
    out->rn = 15u;
    out->imm = ((hw2 >> 4) & 3u) * 8u;
    out->u = 1;
    out->b = 0; /* byte */
    out->sets_flags = 0;
    return 0;
  }

  /* Q-OTTD-0ch: T32 UXTAB Rd,Rn,Rm{,ROR#}. SQLexer::Lex fa53 f381 =
   * uxtab r3, r3, r1 (llvm-mc [53,fa,81,f3]). Rd = Rn + the low byte
   * of Rm after the rotate. Rn=15 is UXTB.W. Reject Rd/Rn/Rm=PC. */
  if ((hw1 & 0xFFF0u) == 0xFA50u && (hw1 & 0xFu) != 0xFu && (hw2 & 0xF0C0u) == 0xF080u) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_XTEND;
    out->rd = rd;
    out->rn = rn;
    out->rm = rm;
    out->imm = ((hw2 >> 4) & 3u) * 8u;
    out->u = 1;
    out->b = 0;
    out->sets_flags = 0;
    return 0;
  }

  /* Q-OTTD-0p: T32 DMB option → MANGO_OP_NOP (single-thread; no GPR/CPSR effect).
   * Guest f3bf 8f5f = dmb sy; wider (hw2 & 0xFFF0)==0x8F50 opens option sibs
   * (ish/osh/…). DSB/ISB/CLREX T32 (8F4x/8F6x/8F2F) stay uncover this bite —
   * A32 F57FF05x/04x/06x/01F already NOP. Execute is existing NOP (pc+=4). */
  if (hw1 == 0xF3BFu && (hw2 & 0xFFF0u) == 0x8F50u) {
    out->op = MANGO_OP_NOP;
    return 0;
  }

  /* Q-OTTD-0ar: T32 BFI/BFC. OpenTTD f36c 401f = bfi r0,r12,#16,#16
   * (llvm-mc [6c,f3,1f,40]). lsb=imm3:imm2, msb=hw2[4:0], width=msb-lsb+1.
   * Rn=15 is BFC. Reuse MANGO_OP_BFI (rm=source). Reject Rd=PC and msb<lsb. */
  if ((hw1 & 0xFFF0u) == 0xF360u && (hw2 & 0x8020u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t lsb = (((hw2 >> 12) & 7u) << 2) | ((hw2 >> 6) & 3u);
    uint32_t msb = hw2 & 0x1Fu;
    if (rd == MANGO_REG_PC || msb < lsb) {
      return -1;
    }
    out->op = (rn == MANGO_REG_PC) ? MANGO_OP_BFC : MANGO_OP_BFI;
    out->rd = rd;
    out->rm = rn;
    out->imm = lsb;
    out->rs = msb;
    return 0;
  }

  /* Q-OTTD-0j-ubfx: T32 UBFX — guest f3c1 070a (lsb=0, widthm1=10 → width 11).
   * Reuse MANGO_OP_UBFX (imm=lsb, rs=widthm1). Not SBFX/BFI/BFC this bite. */
  if ((hw1 & 0xFFF0u) == 0xF3C0u && (hw2 & 0x8000u) == 0) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t lsb = (((hw2 >> 12) & 7u) << 2) | ((hw2 >> 6) & 3u);
    uint32_t widthm1 = hw2 & 0x3Fu;
    uint32_t width = widthm1 + 1u;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC) {
      return -1;
    }
    if (lsb + width > 32u) {
      return -1;
    }
    out->op = MANGO_OP_UBFX;
    out->rd = rd;
    out->rn = rn;
    out->imm = lsb;
    out->rs = widthm1;
    return 0;
  }

  /* Q-OTTD-0ax: T32 LSL.W Rd,Rn,Rm (shift amount in Rm, low 8 bits).
   * OpenTTD fa08 f204 = lsl.w r2,r8,r4 (llvm-mc [08,fa,04,f2]).
   * Reuse MANGO_OP_MOV + shift_by_reg, S=0 so NZCV hold. ASR.W is
   * Q-OTTD-0bh. LSR/ROR.W (FA2x/FA6x) and LSLS/ASRS stay closed.
   * Reject Rd/Rn/Rm=PC. */
  if ((hw1 & 0xFFF0u) == 0xFA00u && (hw2 & 0xF0F0u) == 0xF000u) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_MOV;
    out->rd = rd;
    out->rm = rn;
    out->rs = rm;
    out->is_imm = 0;
    out->sets_flags = 0;
    out->shift_type = 0; /* LSL */
    out->shift_by_reg = 1;
    return 0;
  }

  /* Q-OTTD-0bh: T32 ASR.W Rd,Rn,Rm. SDL fa41 f20b = asr.w r2,r1,r11
   * (llvm-mc [41,fa,0b,f2]). Amount is Rm[7:0]. S=0. ASRS (FA5x) and
   * LSR/ROR.W stay closed. Reject Rd/Rn/Rm=PC. */
  if ((hw1 & 0xFFF0u) == 0xFA40u && (hw2 & 0xF0F0u) == 0xF000u) {
    uint32_t rn = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    uint32_t rm = hw2 & 0xFu;
    if (rd == MANGO_REG_PC || rn == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_MOV;
    out->rd = rd;
    out->rm = rn;
    out->rs = rm;
    out->is_imm = 0;
    out->sets_flags = 0;
    out->shift_type = 2; /* ASR */
    out->shift_by_reg = 1;
    return 0;
  }

  /* Q-OTTD-0af: T32 CLZ Rd,Rm (DDI0597). OpenTTD SDL_main stop
   * pc=0x26f430 word 0xf080fab0 = clz r0,r0 (llvm-mc [0xb0,0xfa,0x80,0xf0]).
   * hw1 1111 1010 1011 Rm, hw2 1111 Rd 1000 Rm. Both Rm fields must match.
   * Reuse MANGO_OP_CLZ (flags untouched). Reject Rd/Rm=PC. Not REV/RBIT
   * (those are FA9x). */
  if ((hw1 & 0xFFF0u) == 0xFAB0u && (hw2 & 0xF0F0u) == 0xF080u) {
    uint32_t rm = hw1 & 0xFu;
    uint32_t rd = (hw2 >> 8) & 0xFu;
    if ((hw2 & 0xFu) != rm) {
      return -1; /* Rm copies disagree: UNPREDICTABLE */
    }
    if (rd == MANGO_REG_PC || rm == MANGO_REG_PC) {
      return -1;
    }
    out->op = MANGO_OP_CLZ;
    out->rd = rd;
    out->rm = rm;
    return 0;
  }

  /* Thumb coprocessor encodings (VFP/NEON) match the ARM instruction word
   * with cond=AL. hw1 is the high halfword. 1110 11xx only — LDM/STM
   * (1110 10xx) stay on the T32 paths above. */
  if ((hw1 & 0xFC00u) == 0xEC00u) {
    return mango_decode(((uint32_t)hw1 << 16) | hw2, out);
  }

  return -1;
}

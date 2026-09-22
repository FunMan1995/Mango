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

int mango_decode(uint32_t word, MangoInsn* out) {
  mango_insn_clear(out);
  out->cond = (word >> 28) & 0xF;

  if (out->cond == 0xF) {
    return -1; /* 0xF is ARMv5+'s unconditional-extension selector, not a real cond */
  }

  /* SVC/SWI: bits 27-24 = 1111, the rest is a legacy immediate EABI code
   * ignores; the actual syscall number is in r7 at execution time, not
   * decoded here, see mango_interp_run's SVC case in interp.c. */
  if (((word >> 24) & 0xF) == 0xF) {
    out->op = MANGO_OP_SVC;
    return 0;
  }

  /* BX Rm: cond 0001 0010 1111 1111 1111 0001 Rm */
  if (((word >> 20) & 0xFF) == 0x12 && ((word >> 4) & 0xFFFF) == 0xFFF1) {
    out->op = MANGO_OP_BX;
    out->rm = word & 0xF;
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
    if (rt == MANGO_REG_PC) {
      return -1; /* LDR/STR PC is an indirect branch, not in this subset */
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
    if (w && (reglist & (1u << rn))) {
      return -1; /* writeback with Rn in the list is UNPREDICTABLE */
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
  uint32_t s = (hw1 >> 10) & 1u;
  uint32_t j1 = (hw2 >> 13) & 1u;
  uint32_t j2 = (hw2 >> 11) & 1u;
  uint32_t i1 = (j1 ^ s) ^ 1u;
  uint32_t i2 = (j2 ^ s) ^ 1u;
  uint32_t imm6 = hw1 & 0x3Fu;
  uint32_t imm11 = hw2 & 0x7FFu;
  uint32_t imm = (s << 20) | (i1 << 19) | (i2 << 18) | (imm6 << 12) | (imm11 << 1);
  if (s) {
    imm |= 0xFFE00000u;
  }
  return imm;
}

int mango_decode_t32(uint16_t hw1, uint16_t hw2, MangoInsn* out) {
  mango_insn_clear(out);
  out->cond = 0xE;

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
      if (cond >= 0xEu) {
        return -1;
      }
      out->op = MANGO_OP_B;
      out->cond = cond;
      out->imm = mango_t32_bcond_imm(hw1, hw2);
      return 0;
    }
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

  return -1;
}

#include "mango/interp.h"

#include "mango/decoder.h"

/* Little-endian (real ARM32 Android/Linux), explicit shifts to stay strict-aliasing-safe. */
static uint32_t mango_load_u32_le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void mango_store_u32_le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static int mango_check_word_access(const MangoMemory* mem, uint32_t addr) {
  if ((addr % 4u) != 0) {
    return -1;
  }
  if ((uint64_t)addr + 4u > mem->size) { /* uint64_t so addr near UINT32_MAX can't wrap */
    return -1;
  }
  return 0;
}

static int mango_check_byte_access(const MangoMemory* mem, uint32_t addr) {
  return addr < mem->size ? 0 : -1;
}

static uint32_t mango_load_u16_le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static void mango_store_u16_le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static int mango_check_half_access(const MangoMemory* mem, uint32_t addr) {
  if ((addr % 2u) != 0) {
    return -1;
  }
  if ((uint64_t)addr + 2u > mem->size) {
    return -1;
  }
  return 0;
}

static uint32_t mango_sign_extend8(uint32_t b) { return (b & 0x80u) ? (b | 0xFFFFFF00u) : b; }

static uint32_t mango_sign_extend16(uint32_t h) { return (h & 0x8000u) ? (h | 0xFFFF0000u) : h; }

/* Real hardware: reading PC as an operand gives addr+8 in A32, addr+4 in T16. */
static uint32_t mango_read_reg(const MangoCpu* cpu, uint32_t insn_addr, uint32_t reg) {
  if (reg == MANGO_REG_PC) {
    return insn_addr + ((cpu->cpsr & MANGO_CPSR_T) ? 4u : 8u);
  }
  return cpu->r[reg];
}

/* T16 ADR and LDR-literal: (PC + 4) AND NOT 3. High-register ADD Rd, PC does not. */
static uint32_t mango_thumb_align_pc(const MangoCpu* cpu, const MangoInsn* insn, uint32_t value) {
  if ((cpu->cpsr & MANGO_CPSR_T) && insn->rn == MANGO_REG_PC && insn->is_imm) {
    return value & ~3u;
  }
  return value;
}

typedef struct MangoOp2 {
  uint32_t value;
  uint32_t carry; /* 0 or 1, only meaningful if update_c */
  int update_c;   /* 0 = logical S-bit ops leave C alone (LSL #0, Rs=0, rot=0) */
} MangoOp2;

/* ARM barrel shifter, including the #0 encodings (LSR/ASR #32, RRX) and
 * register-controlled amounts. ASR is hand-rolled: signed right-shift is
 * implementation-defined in C. amount is the 5-bit field (immediate) or
 * Rs[7:0] (register). */
static MangoOp2 mango_shift(uint32_t value, uint32_t shift_type, uint32_t amount, int by_reg,
                            uint32_t carry_in) {
  MangoOp2 o;
  o.value = value;
  o.carry = 0;
  o.update_c = 1;

  if (!by_reg) {
    if (shift_type == 0) { /* LSL */
      if (amount == 0) {
        o.update_c = 0;
        return o;
      }
      o.value = value << amount;
      o.carry = (value >> (32u - amount)) & 1u;
      return o;
    }
    if (shift_type == 1) { /* LSR; #0 means #32 */
      uint32_t n = amount == 0 ? 32u : amount;
      o.value = n == 32u ? 0u : value >> n;
      o.carry = (value >> (n - 1u)) & 1u;
      return o;
    }
    if (shift_type == 2) { /* ASR; #0 means #32 */
      uint32_t n = amount == 0 ? 32u : amount;
      if (n == 32u) {
        o.value = (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;
        o.carry = (value >> 31) & 1u;
      } else {
        uint32_t sign_fill = (value & 0x80000000u) ? (~0u << (32u - n)) : 0u;
        o.value = (value >> n) | sign_fill;
        o.carry = (value >> (n - 1u)) & 1u;
      }
      return o;
    }
    /* ROR; #0 means RRX */
    if (amount == 0) {
      o.value = (carry_in << 31) | (value >> 1);
      o.carry = value & 1u;
      return o;
    }
    o.value = (value >> amount) | (value << (32u - amount));
    o.carry = (value >> (amount - 1u)) & 1u;
    return o;
  }

  if (amount == 0) {
    o.update_c = 0;
    return o;
  }
  if (shift_type == 0) { /* LSL Rs */
    if (amount < 32u) {
      o.value = value << amount;
      o.carry = (value >> (32u - amount)) & 1u;
    } else if (amount == 32u) {
      o.value = 0;
      o.carry = value & 1u;
    } else {
      o.value = 0;
      o.carry = 0;
    }
    return o;
  }
  if (shift_type == 1) { /* LSR Rs */
    if (amount < 32u) {
      o.value = value >> amount;
      o.carry = (value >> (amount - 1u)) & 1u;
    } else if (amount == 32u) {
      o.value = 0;
      o.carry = (value >> 31) & 1u;
    } else {
      o.value = 0;
      o.carry = 0;
    }
    return o;
  }
  if (shift_type == 2) { /* ASR Rs */
    if (amount < 32u) {
      uint32_t sign_fill = (value & 0x80000000u) ? (~0u << (32u - amount)) : 0u;
      o.value = (value >> amount) | sign_fill;
      o.carry = (value >> (amount - 1u)) & 1u;
    } else {
      o.value = (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;
      o.carry = (value >> 31) & 1u;
    }
    return o;
  }
  /* ROR Rs: Rs[4:0]==0 and Rs[7:0]!=0 is rotate-by-32 (identity, C=Rm[31]) */
  {
    uint32_t low5 = amount & 31u;
    if (low5 == 0) {
      o.value = value;
      o.carry = (value >> 31) & 1u;
      return o;
    }
    o.value = (value >> low5) | (value << (32u - low5));
    o.carry = (value >> (low5 - 1u)) & 1u;
    return o;
  }
}

static MangoOp2 mango_eval_operand2(const MangoCpu* cpu, uint32_t addr, const MangoInsn* insn) {
  if (insn->is_imm) {
    MangoOp2 o;
    o.value = insn->imm;
    if (insn->shift_amount != 0) {
      o.update_c = 1;
      o.carry = insn->imm >> 31;
    } else {
      o.update_c = 0;
      o.carry = 0;
    }
    return o;
  }
  uint32_t value = mango_read_reg(cpu, addr, insn->rm);
  uint32_t amount =
      insn->shift_by_reg ? (mango_read_reg(cpu, addr, insn->rs) & 0xFFu) : insn->shift_amount;
  uint32_t carry_in = (cpu->cpsr & MANGO_CPSR_C) ? 1u : 0u;
  return mango_shift(value, insn->shift_type, amount, insn->shift_by_reg, carry_in);
}

/* NZCV for ADD/ADDS: result = lhs + rhs. */
static uint32_t mango_flags_for_add(uint32_t lhs, uint32_t rhs, uint32_t result) {
  uint32_t flags = 0;
  flags |= (result == 0) ? MANGO_CPSR_Z : 0;
  flags |= (result & 0x80000000u) ? MANGO_CPSR_N : 0;
  flags |= (result < lhs) ? MANGO_CPSR_C : 0;
  flags |= ((~(lhs ^ rhs) & (lhs ^ result)) & 0x80000000u) ? MANGO_CPSR_V : 0;
  return flags;
}

/* NZCV for SUB/SUBS/CMP: result = lhs - rhs. */
static uint32_t mango_flags_for_sub(uint32_t lhs, uint32_t rhs, uint32_t result) {
  uint32_t flags = 0;
  flags |= (result == 0) ? MANGO_CPSR_Z : 0;
  flags |= (result & 0x80000000u) ? MANGO_CPSR_N : 0;
  flags |= (lhs >= rhs) ? MANGO_CPSR_C : 0;
  flags |= ((lhs ^ rhs) & (lhs ^ result) & 0x80000000u) ? MANGO_CPSR_V : 0;
  return flags;
}

/* NZCV for ADC (result = lhs + rhs + carry_in); SBC/RSC reuse this too,
 * feeding it ~rhs (see their cases below), the same trick real ALU
 * hardware uses since A-B-1+C == A+~B+C in two's complement. */
static uint32_t mango_flags_for_adc(uint32_t lhs, uint32_t rhs, uint32_t carry_in,
                                    uint32_t result) {
  uint32_t flags = 0;
  uint64_t wide = (uint64_t)lhs + rhs + carry_in;
  flags |= (result == 0) ? MANGO_CPSR_Z : 0;
  flags |= (result & 0x80000000u) ? MANGO_CPSR_N : 0;
  flags |= (wide > 0xFFFFFFFFu) ? MANGO_CPSR_C : 0;
  flags |= ((~(lhs ^ rhs) & (lhs ^ result)) & 0x80000000u) ? MANGO_CPSR_V : 0;
  return flags;
}

/* NZ for AND/EOR/ORR/BIC/MVN/MOV/TST/TEQ, and MULS/MLAS. V is unaffected.
 * C comes from the shifter when update_c; otherwise C is left as-is
 * (LSL #0, Rs=0, unrotated immediate, MUL/MLA). */
static uint32_t mango_flags_for_logical(uint32_t old_cpsr, uint32_t result, uint32_t shifter_c,
                                        int update_c) {
  uint32_t n = (result & 0x80000000u) ? MANGO_CPSR_N : 0;
  uint32_t z = (result == 0) ? MANGO_CPSR_Z : 0;
  uint32_t c = update_c ? (shifter_c ? MANGO_CPSR_C : 0) : (old_cpsr & MANGO_CPSR_C);
  return (old_cpsr & MANGO_CPSR_V) | n | z | c;
}

static void mango_set_nzcv(MangoCpu* cpu, uint32_t flags) {
  cpu->cpsr &= ~(MANGO_CPSR_N | MANGO_CPSR_Z | MANGO_CPSR_C | MANGO_CPSR_V);
  cpu->cpsr |= flags;
}

static int mango_cond_holds(uint32_t cond, uint32_t cpsr) {
  int n = (cpsr & MANGO_CPSR_N) != 0;
  int z = (cpsr & MANGO_CPSR_Z) != 0;
  int c = (cpsr & MANGO_CPSR_C) != 0;
  int v = (cpsr & MANGO_CPSR_V) != 0;

  switch (cond) {
    case 0x0:
      return z; /* EQ */
    case 0x1:
      return !z; /* NE */
    case 0x2:
      return c; /* CS/HS */
    case 0x3:
      return !c; /* CC/LO */
    case 0x4:
      return n; /* MI */
    case 0x5:
      return !n; /* PL */
    case 0x6:
      return v; /* VS */
    case 0x7:
      return !v; /* VC */
    case 0x8:
      return c && !z; /* HI */
    case 0x9:
      return !c || z; /* LS */
    case 0xA:
      return n == v; /* GE */
    case 0xB:
      return n != v; /* LT */
    case 0xC:
      return !z && (n == v); /* GT */
    case 0xD:
      return z || (n != v); /* LE */
    case 0xE:
      return 1; /* AL */
    default:
      return 0;
  }
}

int mango_interp_run(MangoCpu* cpu, MangoMemory* mem, uint32_t stop_addr, uint32_t max_steps) {
  uint32_t addr = cpu->r[MANGO_REG_PC];

  for (uint32_t step = 0; step < max_steps; step++) {
    if (addr == stop_addr) {
      return 0;
    }

    int thumb = (cpu->cpsr & MANGO_CPSR_T) != 0;
    MangoInsn insn;
    uint32_t next_addr;

    if (thumb) {
      if ((addr % 2u) != 0 || mango_check_half_access(mem, addr) != 0) {
        return -1;
      }
      uint16_t hw = (uint16_t)mango_load_u16_le(mem->bytes + addr);
      if (mango_decode_t16(hw, &insn) != 0) {
        return -1;
      }
      next_addr = addr + 2u;
    } else {
      if (mango_check_word_access(mem, addr) != 0) {
        return -1;
      }
      if (mango_decode(mango_load_u32_le(mem->bytes + addr), &insn) != 0) {
        return -1;
      }
      next_addr = addr + 4u;
    }

    /* condition false = no-op, covers B/BX too, no per-case handling needed */
    if (mango_cond_holds(insn.cond, cpu->cpsr)) {
      switch (insn.op) {
        case MANGO_OP_MOV: {
          MangoOp2 op2 = mango_eval_operand2(cpu, addr, &insn);
          cpu->r[insn.rd] = op2.value;
          if (insn.sets_flags) {
            mango_set_nzcv(cpu,
                           mango_flags_for_logical(cpu->cpsr, op2.value, op2.carry, op2.update_c));
          }
          break;
        }

        case MANGO_OP_MVN: {
          MangoOp2 op2 = mango_eval_operand2(cpu, addr, &insn);
          uint32_t result = ~op2.value;
          cpu->r[insn.rd] = result;
          if (insn.sets_flags) {
            mango_set_nzcv(cpu,
                           mango_flags_for_logical(cpu->cpsr, result, op2.carry, op2.update_c));
          }
          break;
        }

        case MANGO_OP_AND:
        case MANGO_OP_EOR:
        case MANGO_OP_ORR:
        case MANGO_OP_BIC: {
          MangoOp2 op2 = mango_eval_operand2(cpu, addr, &insn);
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          uint32_t result = insn.op == MANGO_OP_AND   ? (lhs & op2.value)
                            : insn.op == MANGO_OP_EOR ? (lhs ^ op2.value)
                            : insn.op == MANGO_OP_ORR ? (lhs | op2.value)
                                                      : (lhs & ~op2.value);
          cpu->r[insn.rd] = result;
          if (insn.sets_flags) {
            mango_set_nzcv(cpu,
                           mango_flags_for_logical(cpu->cpsr, result, op2.carry, op2.update_c));
          }
          break;
        }

        case MANGO_OP_TST:
        case MANGO_OP_TEQ: {
          MangoOp2 op2 = mango_eval_operand2(cpu, addr, &insn);
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          uint32_t result = insn.op == MANGO_OP_TST ? (lhs & op2.value) : (lhs ^ op2.value);
          mango_set_nzcv(cpu, mango_flags_for_logical(cpu->cpsr, result, op2.carry, op2.update_c));
          break;
        }

        case MANGO_OP_ADD: {
          uint32_t rhs = mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t lhs = mango_thumb_align_pc(cpu, &insn, mango_read_reg(cpu, addr, insn.rn));
          uint32_t result = lhs + rhs;
          cpu->r[insn.rd] = result;
          if (insn.sets_flags) {
            mango_set_nzcv(cpu, mango_flags_for_add(lhs, rhs, result));
          }
          break;
        }

        case MANGO_OP_CMN: {
          uint32_t rhs = mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          mango_set_nzcv(cpu, mango_flags_for_add(lhs, rhs, lhs + rhs));
          break;
        }

        case MANGO_OP_ADC: {
          uint32_t rhs = mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          uint32_t carry_in = (cpu->cpsr & MANGO_CPSR_C) ? 1u : 0u;
          uint32_t result = lhs + rhs + carry_in;
          cpu->r[insn.rd] = result;
          if (insn.sets_flags) {
            mango_set_nzcv(cpu, mango_flags_for_adc(lhs, rhs, carry_in, result));
          }
          break;
        }

        case MANGO_OP_SUB: {
          uint32_t rhs = mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          uint32_t result = lhs - rhs;
          cpu->r[insn.rd] = result;
          if (insn.sets_flags) {
            mango_set_nzcv(cpu, mango_flags_for_sub(lhs, rhs, result));
          }
          break;
        }

        case MANGO_OP_RSB: {
          uint32_t rhs = mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          uint32_t result = rhs - lhs;
          cpu->r[insn.rd] = result;
          if (insn.sets_flags) {
            mango_set_nzcv(cpu, mango_flags_for_sub(rhs, lhs, result));
          }
          break;
        }

        case MANGO_OP_SBC: {
          uint32_t rhs = ~mango_eval_operand2(cpu, addr, &insn).value; /* A-B-1+C == A+~B+C */
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          uint32_t carry_in = (cpu->cpsr & MANGO_CPSR_C) ? 1u : 0u;
          uint32_t result = lhs + rhs + carry_in;
          cpu->r[insn.rd] = result;
          if (insn.sets_flags) {
            mango_set_nzcv(cpu, mango_flags_for_adc(lhs, rhs, carry_in, result));
          }
          break;
        }

        case MANGO_OP_RSC: {
          uint32_t op2 = mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t not_rn = ~mango_read_reg(cpu, addr, insn.rn); /* B-A-1+C == B+~A+C */
          uint32_t carry_in = (cpu->cpsr & MANGO_CPSR_C) ? 1u : 0u;
          uint32_t result = op2 + not_rn + carry_in;
          cpu->r[insn.rd] = result;
          if (insn.sets_flags) {
            mango_set_nzcv(cpu, mango_flags_for_adc(op2, not_rn, carry_in, result));
          }
          break;
        }

        case MANGO_OP_CMP: {
          uint32_t rhs = mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t lhs = mango_read_reg(cpu, addr, insn.rn);
          mango_set_nzcv(cpu, mango_flags_for_sub(lhs, rhs, lhs - rhs));
          break;
        }

        case MANGO_OP_MUL:
        case MANGO_OP_MLA: {
          uint32_t result = mango_read_reg(cpu, addr, insn.rm) * mango_read_reg(cpu, addr, insn.rs);
          if (insn.op == MANGO_OP_MLA) {
            result += mango_read_reg(cpu, addr, insn.rn);
          }
          cpu->r[insn.rd] = result;
          if (insn.sets_flags) {
            /* MULS/MLAS: C,V left as-is. */
            mango_set_nzcv(cpu, mango_flags_for_logical(cpu->cpsr, result, 0, 0));
          }
          break;
        }

        case MANGO_OP_SVC:
          return 1; /* cpu->r[PC] == addr still, caller thunks r7/r0-r6 and resumes, see interp.h */

        case MANGO_OP_B: {
          uint32_t pc_off = (cpu->cpsr & MANGO_CPSR_T) ? 4u : 8u;
          next_addr = addr + pc_off + insn.imm;
          break;
        }

        case MANGO_OP_BL:
          cpu->r[MANGO_REG_LR] = addr + 4u;
          next_addr = addr + 8u + insn.imm;
          break;

        case MANGO_OP_BX: {
          uint32_t dest = cpu->r[insn.rm];
          /* Odd stop-sentinels must stay equal to LR so a BX LR halt still
           * matches before fetch. Even stops (and any real target) interwork. */
          if (dest == stop_addr && (dest & 1u)) {
            next_addr = dest;
          } else if (dest & 1u) {
            cpu->cpsr |= MANGO_CPSR_T;
            next_addr = dest & ~1u;
          } else {
            cpu->cpsr &= ~MANGO_CPSR_T;
            next_addr = dest;
          }
          break;
        }

        case MANGO_OP_LDR:
        case MANGO_OP_STR: {
          if (insn.rd == MANGO_REG_PC) {
            return -1; /* indirect branch via LDR PC, not supported yet */
          }
          uint32_t offset = insn.is_imm ? insn.imm : mango_eval_operand2(cpu, addr, &insn).value;
          uint32_t base = mango_thumb_align_pc(cpu, &insn, mango_read_reg(cpu, addr, insn.rn));
          uint32_t wbaddr = insn.u ? base + offset : base - offset;
          uint32_t eaddr = insn.p ? wbaddr : base;

          if (insn.b) {
            if (mango_check_byte_access(mem, eaddr) != 0) {
              return -1;
            }
            if (insn.op == MANGO_OP_LDR) {
              cpu->r[insn.rd] = mem->bytes[eaddr]; /* zero-extended */
            } else {
              mem->bytes[eaddr] = (uint8_t)(cpu->r[insn.rd] & 0xFFu);
            }
          } else {
            if (mango_check_word_access(mem, eaddr) != 0) {
              return -1;
            }
            if (insn.op == MANGO_OP_LDR) {
              cpu->r[insn.rd] = mango_load_u32_le(mem->bytes + eaddr);
            } else {
              mango_store_u32_le(mem->bytes + eaddr, cpu->r[insn.rd]);
            }
          }
          if (insn.w) {
            cpu->r[insn.rn] = wbaddr;
          }
          break;
        }

        case MANGO_OP_LDRH:
        case MANGO_OP_STRH:
        case MANGO_OP_LDRSB:
        case MANGO_OP_LDRSH: {
          /* Extra load/store: Rm is never shifted, unlike regular LDR/STR. */
          uint32_t offset = insn.is_imm ? insn.imm : mango_read_reg(cpu, addr, insn.rm);
          uint32_t base = mango_read_reg(cpu, addr, insn.rn);
          uint32_t wbaddr = insn.u ? base + offset : base - offset;
          uint32_t eaddr = insn.p ? wbaddr : base;

          if (insn.op == MANGO_OP_LDRSB) {
            if (mango_check_byte_access(mem, eaddr) != 0) {
              return -1;
            }
            cpu->r[insn.rd] = mango_sign_extend8(mem->bytes[eaddr]);
          } else {
            if (mango_check_half_access(mem, eaddr) != 0) {
              return -1;
            }
            if (insn.op == MANGO_OP_STRH) {
              mango_store_u16_le(mem->bytes + eaddr, cpu->r[insn.rd]);
            } else if (insn.op == MANGO_OP_LDRH) {
              cpu->r[insn.rd] = mango_load_u16_le(mem->bytes + eaddr);
            } else {
              cpu->r[insn.rd] = mango_sign_extend16(mango_load_u16_le(mem->bytes + eaddr));
            }
          }
          if (insn.w) {
            cpu->r[insn.rn] = wbaddr;
          }
          break;
        }

        case MANGO_OP_LDRD:
        case MANGO_OP_STRD: {
          uint32_t offset = insn.is_imm ? insn.imm : mango_read_reg(cpu, addr, insn.rm);
          uint32_t base = mango_read_reg(cpu, addr, insn.rn);
          uint32_t wbaddr = insn.u ? base + offset : base - offset;
          uint32_t eaddr = insn.p ? wbaddr : base;
          if (mango_check_word_access(mem, eaddr) != 0 ||
              mango_check_word_access(mem, eaddr + 4u) != 0) {
            return -1;
          }
          if (insn.op == MANGO_OP_STRD) {
            mango_store_u32_le(mem->bytes + eaddr, cpu->r[insn.rd]);
            mango_store_u32_le(mem->bytes + eaddr + 4u, cpu->r[insn.rd + 1u]);
          } else {
            cpu->r[insn.rd] = mango_load_u32_le(mem->bytes + eaddr);
            cpu->r[insn.rd + 1u] = mango_load_u32_le(mem->bytes + eaddr + 4u);
          }
          if (insn.w) {
            cpu->r[insn.rn] = wbaddr;
          }
          break;
        }

        case MANGO_OP_SWP: {
          uint32_t eaddr = mango_read_reg(cpu, addr, insn.rn);
          uint32_t store_val = mango_read_reg(cpu, addr, insn.rm);
          if (insn.b) {
            if (mango_check_byte_access(mem, eaddr) != 0) {
              return -1;
            }
            uint32_t loaded = mem->bytes[eaddr];
            mem->bytes[eaddr] = (uint8_t)(store_val & 0xFFu);
            cpu->r[insn.rd] = loaded;
          } else {
            if (mango_check_word_access(mem, eaddr) != 0) {
              return -1;
            }
            uint32_t loaded = mango_load_u32_le(mem->bytes + eaddr);
            mango_store_u32_le(mem->bytes + eaddr, store_val);
            cpu->r[insn.rd] = loaded;
          }
          break;
        }

        case MANGO_OP_LDM:
        case MANGO_OP_STM: {
          /* Lowest-numbered register always lands at the lowest address,
           * regardless of IA/IB/DA/DB. P/U only choose the start address
           * and whether Rn moves; see ARM ARM LDM/STM addressing modes.
           * PUSH = STMDB sp!, POP = LDMIA sp!. */
          uint32_t count = 0;
          for (uint32_t i = 0; i < 16; i++) {
            if (insn.reglist & (1u << i)) {
              count++;
            }
          }
          uint32_t base = mango_read_reg(cpu, addr, insn.rn);
          uint32_t start = insn.u ? (insn.p ? base + 4u : base)
                                  : (insn.p ? base - 4u * count : base - 4u * count + 4u);
          if ((start % 4u) != 0) {
            return -1;
          }
          if ((uint64_t)start + (uint64_t)count * 4u > mem->size) {
            return -1;
          }

          uint32_t eaddr = start;
          int loaded_pc = 0;
          uint32_t new_pc = 0;
          for (uint32_t i = 0; i < 16; i++) {
            if ((insn.reglist & (1u << i)) == 0) {
              continue;
            }
            if (insn.op == MANGO_OP_STM) {
              mango_store_u32_le(mem->bytes + eaddr, mango_read_reg(cpu, addr, i));
            } else if (i == MANGO_REG_PC) {
              uint32_t value = mango_load_u32_le(mem->bytes + eaddr);
              if (value == stop_addr && (value & 1u)) {
                /* odd sentinel: keep the exact value so the run stops */
              } else if (value & 1u) {
                cpu->cpsr |= MANGO_CPSR_T;
                value &= ~1u;
              } else if (value & 2u) {
                return -1; /* unaligned ARM PC */
              } else {
                cpu->cpsr &= ~MANGO_CPSR_T;
              }
              loaded_pc = 1;
              new_pc = value;
            } else {
              cpu->r[i] = mango_load_u32_le(mem->bytes + eaddr);
            }
            eaddr += 4u;
          }
          if (insn.w) {
            cpu->r[insn.rn] = insn.u ? base + 4u * count : base - 4u * count;
          }
          if (loaded_pc) {
            next_addr = new_pc;
          }
          break;
        }

        default:
          return -1;
      }
    }

    addr = next_addr;
    cpu->r[MANGO_REG_PC] = addr;
  }

  return -1; /* step limit hit */
}

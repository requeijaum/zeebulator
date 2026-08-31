#include "core/cpu/arm_interpreter.h"

#include <bit>

namespace zeebulator {

namespace {

uint32_t RotateRight(uint32_t value, uint32_t amount) {
  amount &= 31;
  return amount == 0 ? value : (value >> amount) | (value << (32 - amount));
}

// Shared add/subtract-with-flags core: every data-processing arithmetic
// opcode (SUB/RSB/ADD/ADC/SBC/RSC/CMP/CMN) reduces to this by choosing
// operands and carry_in appropriately (SUB(a,b) = Add(a, ~b, 1), etc.)
uint32_t AddWithFlags(uint32_t a, uint32_t b, uint32_t carry_in,
                       bool& carry_out, bool& overflow_out) {
  uint64_t sum = static_cast<uint64_t>(a) + static_cast<uint64_t>(b) + carry_in;
  uint32_t result = static_cast<uint32_t>(sum);
  carry_out = (sum >> 32) != 0;
  overflow_out = ((~(a ^ b) & (a ^ result)) >> 31) & 1;
  return result;
}

// ARM shifter, used for both operand2 register shifts and LDR/STR
// register-offset shifts. `is_immediate_shift` selects between the
// immediate-shift-amount special cases (shift #0 has type-specific
// meaning, e.g. ROR #0 == RRX) and the register-shift-amount ones
// (amount 0 always means "no shift, carry unchanged").
uint32_t ShiftWithCarry(uint32_t value, uint32_t shift_type,
                         uint32_t shift_amount, bool carry_in,
                         bool& carry_out, bool is_immediate_shift) {
  switch (shift_type) {
    case 0:  // LSL
      if (shift_amount == 0) {
        carry_out = carry_in;
        return value;
      }
      if (shift_amount >= 32) {
        carry_out = (shift_amount == 32) ? (value & 1) : false;
        return 0;
      }
      carry_out = (value >> (32 - shift_amount)) & 1;
      return value << shift_amount;

    case 1:  // LSR
      if (shift_amount == 0) {
        if (is_immediate_shift) {  // LSR #0 encodes LSR #32
          carry_out = (value >> 31) & 1;
          return 0;
        }
        carry_out = carry_in;
        return value;
      }
      if (shift_amount >= 32) {
        carry_out = (shift_amount == 32) ? ((value >> 31) & 1) : false;
        return 0;
      }
      carry_out = (value >> (shift_amount - 1)) & 1;
      return value >> shift_amount;

    case 2: {  // ASR
      if (shift_amount == 0) {
        if (is_immediate_shift) {  // ASR #0 encodes ASR #32
          carry_out = (value >> 31) & 1;
          return carry_out ? 0xFFFFFFFFu : 0u;
        }
        carry_out = carry_in;
        return value;
      }
      if (shift_amount >= 32) {
        carry_out = (value >> 31) & 1;
        return carry_out ? 0xFFFFFFFFu : 0u;
      }
      carry_out = (value >> (shift_amount - 1)) & 1;
      return static_cast<uint32_t>(static_cast<int32_t>(value) >> shift_amount);
    }

    case 3:  // ROR / RRX
      if (shift_amount == 0) {
        if (is_immediate_shift) {  // ROR #0 encodes RRX
          carry_out = value & 1;
          return (static_cast<uint32_t>(carry_in) << 31) | (value >> 1);
        }
        carry_out = carry_in;
        return value;
      }
      if ((shift_amount & 31) == 0) {
        carry_out = (value >> 31) & 1;
        return value;
      }
      carry_out = (value >> ((shift_amount & 31) - 1)) & 1;
      return RotateRight(value, shift_amount);
  }
  carry_out = carry_in;
  return value;
}

}  // namespace

ArmInterpreter::ArmInterpreter() { Reset(); }

void ArmInterpreter::Reset() {
  regs_.fill(0);
  cpsr_ = 0;
}

bool ArmInterpreter::GetFlag(CpsrBit bit) const { return (cpsr_ >> bit) & 1; }

void ArmInterpreter::SetFlag(CpsrBit bit, bool value) {
  if (value) {
    cpsr_ |= (1u << bit);
  } else {
    cpsr_ &= ~(1u << bit);
  }
}

uint32_t ArmInterpreter::GetRegister(int index) const { return regs_[index]; }

void ArmInterpreter::SetRegister(int index, uint32_t value) {
  regs_[index] = value;
}

uint32_t ArmInterpreter::GetCpsr() const { return cpsr_; }
void ArmInterpreter::SetCpsr(uint32_t value) { cpsr_ = value; }

bool ArmInterpreter::Serialize(std::ostream& out) const {
  out.write(reinterpret_cast<const char*>(regs_.data()),
            static_cast<std::streamsize>(regs_.size() * sizeof(uint32_t)));
  out.write(reinterpret_cast<const char*>(&cpsr_), sizeof(cpsr_));
  if (!out.good()) return false;
  return memory_.Serialize(out);
}

bool ArmInterpreter::Deserialize(std::istream& in) {
  std::array<uint32_t, 16> new_regs{};
  uint32_t new_cpsr = 0;
  in.read(reinterpret_cast<char*>(new_regs.data()),
          static_cast<std::streamsize>(new_regs.size() * sizeof(uint32_t)));
  in.read(reinterpret_cast<char*>(&new_cpsr), sizeof(new_cpsr));
  if (!in.good()) return false;
  if (!memory_.Deserialize(in)) return false;
  regs_ = new_regs;
  cpsr_ = new_cpsr;
  return true;
}

void ArmInterpreter::SetCallOutRange(uint32_t base, uint32_t size) {
  call_out_base_ = base;
  call_out_size_ = size;
}

void ArmInterpreter::SetCallOutHandler(CallOutHandler handler) {
  call_out_handler_ = std::move(handler);
}

uint32_t ArmInterpreter::ReadOperandRegister(uint32_t index) const {
  // regs_[kPC] always holds the address of the instruction currently
  // being fetched/executed (see Step()); the ISA defines a register-read
  // of R15 as that address + 8, so the adjustment happens only here.
  return index == kPC ? regs_[kPC] + 8 : regs_[index];
}

bool ArmInterpreter::ConditionPassed(uint32_t cond) const {
  bool n = GetFlag(kCpsrN), z = GetFlag(kCpsrZ), c = GetFlag(kCpsrC),
       v = GetFlag(kCpsrV);
  switch (cond) {
    case 0x0: return z;                    // EQ
    case 0x1: return !z;                   // NE
    case 0x2: return c;                    // CS/HS
    case 0x3: return !c;                   // CC/LO
    case 0x4: return n;                    // MI
    case 0x5: return !n;                   // PL
    case 0x6: return v;                    // VS
    case 0x7: return !v;                   // VC
    case 0x8: return c && !z;              // HI
    case 0x9: return !c || z;              // LS
    case 0xA: return n == v;               // GE
    case 0xB: return n != v;               // LT
    case 0xC: return !z && (n == v);       // GT
    case 0xD: return z || (n != v);        // LE
    case 0xE: return true;                 // AL
    default: return false;                 // NV (reserved) — v1: never
  }
}

ArmInterpreter::Operand2Result ArmInterpreter::DecodeOperand2(
    uint32_t instr) const {
  bool current_carry = GetFlag(kCpsrC);
  if ((instr >> 25) & 1) {
    uint32_t rotate_imm = (instr >> 8) & 0xF;
    uint32_t imm8 = instr & 0xFF;
    uint32_t value = RotateRight(imm8, rotate_imm * 2);
    bool carry_out = (rotate_imm == 0) ? current_carry : ((value >> 31) & 1);
    return {value, carry_out};
  }

  uint32_t rm = instr & 0xF;
  uint32_t rm_val = ReadOperandRegister(rm);
  uint32_t shift_type = (instr >> 5) & 0x3;
  bool is_immediate_shift = ((instr >> 4) & 1) == 0;
  uint32_t shift_amount;
  if (is_immediate_shift) {
    shift_amount = (instr >> 7) & 0x1F;
  } else {
    uint32_t rs = (instr >> 8) & 0xF;
    shift_amount = ReadOperandRegister(rs) & 0xFF;
  }
  bool carry_out;
  uint32_t value = ShiftWithCarry(rm_val, shift_type, shift_amount,
                                   current_carry, carry_out, is_immediate_shift);
  return {value, carry_out};
}

void ArmInterpreter::ExecuteDataProcessing(uint32_t instr) {
  uint32_t opcode = (instr >> 21) & 0xF;
  bool s = ((instr >> 20) & 1) != 0;
  uint32_t rn = (instr >> 16) & 0xF;
  uint32_t rd = (instr >> 12) & 0xF;

  Operand2Result op2 = DecodeOperand2(instr);
  uint32_t rn_val = ReadOperandRegister(rn);

  uint32_t result = 0;
  bool write_result = true;
  bool carry_out = op2.carry_out;
  bool overflow_out = GetFlag(kCpsrV);

  switch (opcode) {
    case 0x0: result = rn_val & op2.value; break;                        // AND
    case 0x1: result = rn_val ^ op2.value; break;                        // EOR
    case 0x2: result = AddWithFlags(rn_val, ~op2.value, 1, carry_out, overflow_out); break;  // SUB
    case 0x3: result = AddWithFlags(op2.value, ~rn_val, 1, carry_out, overflow_out); break;  // RSB
    case 0x4: result = AddWithFlags(rn_val, op2.value, 0, carry_out, overflow_out); break;   // ADD
    case 0x5: result = AddWithFlags(rn_val, op2.value, GetFlag(kCpsrC), carry_out, overflow_out); break;  // ADC
    case 0x6: result = AddWithFlags(rn_val, ~op2.value, GetFlag(kCpsrC), carry_out, overflow_out); break; // SBC
    case 0x7: result = AddWithFlags(op2.value, ~rn_val, GetFlag(kCpsrC), carry_out, overflow_out); break; // RSC
    case 0x8: result = rn_val & op2.value; write_result = false; break;  // TST
    case 0x9: result = rn_val ^ op2.value; write_result = false; break;  // TEQ
    case 0xA: result = AddWithFlags(rn_val, ~op2.value, 1, carry_out, overflow_out); write_result = false; break;  // CMP
    case 0xB: result = AddWithFlags(rn_val, op2.value, 0, carry_out, overflow_out); write_result = false; break;   // CMN
    case 0xC: result = rn_val | op2.value; break;                        // ORR
    case 0xD: result = op2.value; break;                                 // MOV
    case 0xE: result = rn_val & ~op2.value; break;                       // BIC
    case 0xF: result = ~op2.value; break;                                // MVN
  }

  if (write_result) {
    if (rd == kPC) {
      regs_[kPC] = result;
      pc_updated_by_instruction_ = true;
    } else {
      regs_[rd] = result;
    }
  }

  if (s) {
    if (rd == kPC && write_result) {
      throw UnimplementedInstruction(
          "S=1 with Rd=R15 (SPSR restore) not supported");
    }
    SetFlag(kCpsrN, (result >> 31) & 1);
    SetFlag(kCpsrZ, result == 0);
    SetFlag(kCpsrC, carry_out);
    SetFlag(kCpsrV, overflow_out);
  }
}

void ArmInterpreter::ExecuteBranch(uint32_t instr) {
  bool link = (instr >> 24) & 1;
  int32_t offset = static_cast<int32_t>(instr << 8) >> 6;
  uint32_t fetch_addr = regs_[kPC];
  if (link) {
    regs_[kLR] = fetch_addr + 4;
  }
  regs_[kPC] = static_cast<uint32_t>(static_cast<int32_t>(fetch_addr) + 8 + offset);
  pc_updated_by_instruction_ = true;
}

void ArmInterpreter::ExecuteSingleDataTransfer(uint32_t instr) {
  bool immediate_offset = ((instr >> 25) & 1) == 0;
  bool pre_indexed = (instr >> 24) & 1;
  bool add_offset = (instr >> 23) & 1;
  bool byte_transfer = (instr >> 22) & 1;
  bool write_back = (instr >> 21) & 1;
  bool load = (instr >> 20) & 1;
  uint32_t rn = (instr >> 16) & 0xF;
  uint32_t rd = (instr >> 12) & 0xF;

  uint32_t offset;
  if (immediate_offset) {
    offset = instr & 0xFFF;
  } else {
    uint32_t rm = instr & 0xF;
    uint32_t rm_val = ReadOperandRegister(rm);
    uint32_t shift_type = (instr >> 5) & 0x3;
    uint32_t shift_amount = (instr >> 7) & 0x1F;
    bool carry_out_unused;
    offset = ShiftWithCarry(rm_val, shift_type, shift_amount, GetFlag(kCpsrC),
                             carry_out_unused, /*is_immediate_shift=*/true);
  }

  uint32_t base = ReadOperandRegister(rn);
  uint32_t transfer_address =
      pre_indexed ? (add_offset ? base + offset : base - offset) : base;

  if (load) {
    uint32_t value = byte_transfer ? memory_.Read8(transfer_address)
                                    : memory_.Read32(transfer_address);
    if (rd == kPC) {
      // LDR pc, ... interworks (selects ARM/Thumb from bit 0) on
      // ARMv5T and later, which ARMv6/ARM1136J-S includes -- the real
      // idiom compiled code compiled for interworking uses to return
      // from a function that may have been called via BLX from the
      // other instruction state.
      SetPcInterworking(value);
    } else {
      regs_[rd] = value;
    }
  } else {
    uint32_t value = ReadOperandRegister(rd);
    if (byte_transfer) {
      memory_.Write8(transfer_address, static_cast<uint8_t>(value));
    } else {
      memory_.Write32(transfer_address, value);
    }
  }

  if (!pre_indexed) {
    uint32_t new_base = add_offset ? base + offset : base - offset;
    regs_[rn] = new_base;
  } else if (write_back) {
    regs_[rn] = transfer_address;
  }
}

void ArmInterpreter::ExecuteBlockDataTransfer(uint32_t instr) {
  bool pre_indexed = (instr >> 24) & 1;
  bool up = (instr >> 23) & 1;
  bool write_back = (instr >> 21) & 1;
  bool load = (instr >> 20) & 1;
  uint32_t rn = (instr >> 16) & 0xF;
  uint32_t register_list = instr & 0xFFFF;

  int count = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    if ((register_list >> i) & 1) ++count;
  }

  uint32_t base = regs_[rn];
  // Standard ARM block-transfer address computation: registers always
  // land in increasing register-number -> increasing memory-address
  // order, regardless of direction; only the starting address differs
  // per addressing mode (IA/IB/DA/DB).
  uint32_t address = up ? (base + (pre_indexed ? 4 : 0))
                         : (base - count * 4 + (pre_indexed ? 0 : 4));

  for (uint32_t i = 0; i < 16; ++i) {
    if (!((register_list >> i) & 1)) continue;
    if (load) {
      uint32_t value = memory_.Read32(address);
      if (i == kPC) {
        // LDM/POP with pc in the register list interworks, same real
        // rule and rationale as ExecuteSingleDataTransfer's LDR pc.
        SetPcInterworking(value);
      } else {
        regs_[i] = value;
      }
    } else {
      memory_.Write32(address, ReadOperandRegister(i));
    }
    address += 4;
  }

  if (write_back) {
    regs_[rn] = up ? base + count * 4 : base - count * 4;
  }
}

void ArmInterpreter::ExecuteBranchExchange(uint32_t instr) {
  // BX: cond 0001 0010 1111 1111 1111 0001 Rm
  // BLX (register): identical except bit 5 is set (link).
  bool link = (instr >> 5) & 1;
  uint32_t rm = instr & 0xF;
  uint32_t target = ReadOperandRegister(rm);
  if (link) {
    regs_[kLR] = regs_[kPC] + 4;
  }
  SetPcInterworking(target);
}

void ArmInterpreter::SetPcInterworking(uint32_t target) {
  bool to_thumb = target & 1;
  SetFlag(kCpsrT, to_thumb);
  regs_[kPC] = to_thumb ? (target & ~1u) : (target & ~3u);
  pc_updated_by_instruction_ = true;
}

void ArmInterpreter::ExecuteHalfwordTransfer(uint32_t instr) {
  bool pre_indexed = (instr >> 24) & 1;
  bool add_offset = (instr >> 23) & 1;
  bool immediate_offset = (instr >> 22) & 1;
  bool write_back = (instr >> 21) & 1;
  bool load = (instr >> 20) & 1;
  uint32_t rn = (instr >> 16) & 0xF;
  uint32_t rd = (instr >> 12) & 0xF;
  uint32_t sh = (instr >> 5) & 0x3;

  uint32_t offset;
  if (immediate_offset) {
    offset = (((instr >> 8) & 0xF) << 4) | (instr & 0xF);
  } else {
    uint32_t rm = instr & 0xF;
    offset = ReadOperandRegister(rm);
  }

  uint32_t base = ReadOperandRegister(rn);
  uint32_t transfer_address =
      pre_indexed ? (add_offset ? base + offset : base - offset) : base;

  if (load) {
    uint32_t value;
    switch (sh) {
      case 1:  // LDRH: zero-extended halfword
        value = memory_.Read16(transfer_address);
        break;
      case 2: {  // LDRSB: sign-extended byte
        auto b = static_cast<int8_t>(memory_.Read8(transfer_address));
        value = static_cast<uint32_t>(static_cast<int32_t>(b));
        break;
      }
      case 3: {  // LDRSH: sign-extended halfword
        auto h = static_cast<int16_t>(memory_.Read16(transfer_address));
        value = static_cast<uint32_t>(static_cast<int32_t>(h));
        break;
      }
      default:
        throw UnimplementedInstruction(
            "Halfword transfer with SH=00 (multiply/swap space)");
    }
    if (rd == kPC) {
      regs_[kPC] = value;
      pc_updated_by_instruction_ = true;
    } else {
      regs_[rd] = value;
    }
  } else if (sh == 1) {  // STRH
    memory_.Write16(transfer_address,
                     static_cast<uint16_t>(ReadOperandRegister(rd)));
  } else if (sh == 2) {
    // LDRD: despite L=0 (this whole branch is normally "store"), SH=10
    // in the store-opcode space real-encodes the paired-register load
    // -- confirmed against real assembled output (`ldrd r8, [sp, #32]`
    // -> `e1cd82d0`, matching this exact L=0/SH=2 shape), not guessed.
    // Rd/Rd+1 real ABI convention requires Rd even; real compiled code
    // always respects that, so it's not re-validated here.
    regs_[rd] = memory_.Read32(transfer_address);
    regs_[rd + 1] = memory_.Read32(transfer_address + 4);
  } else {  // sh == 3: STRD
    memory_.Write32(transfer_address, ReadOperandRegister(rd));
    memory_.Write32(transfer_address + 4, ReadOperandRegister(rd + 1));
  }

  if (!pre_indexed) {
    uint32_t new_base = add_offset ? base + offset : base - offset;
    regs_[rn] = new_base;
  } else if (write_back) {
    regs_[rn] = transfer_address;
  }
}

void ArmInterpreter::ExecuteMultiply(uint32_t instr) {
  // Shared field across both forms (see Step()'s dispatch for how this
  // is reached): cond 000000 A S Rd Rn Rs 1001 Rm (short MUL/MLA) or
  // cond 00001 U A S RdHi RdLo Rs 1001 Rm (long UMULL/UMLAL/SMULL/
  // SMLAL) -- "Rs" sits at bits[11:8] in both encodings.
  bool is_long = (instr >> 23) & 1;
  bool accumulate = (instr >> 21) & 1;
  bool s = (instr >> 20) & 1;
  uint32_t rs = (instr >> 8) & 0xF;
  uint32_t rm = instr & 0xF;
  uint32_t rm_val = ReadOperandRegister(rm);
  uint32_t rs_val = ReadOperandRegister(rs);

  if (!is_long) {
    uint32_t rd = (instr >> 16) & 0xF;
    uint32_t rn = (instr >> 12) & 0xF;  // accumulate operand, SBZ if !accumulate
    uint32_t result = rm_val * rs_val;
    if (accumulate) result += ReadOperandRegister(rn);
    regs_[rd] = result;
    if (s) {
      SetFlag(kCpsrN, (result >> 31) & 1);
      SetFlag(kCpsrZ, result == 0);
      // C is UNPREDICTABLE per the ARM ARM for MUL/MLA with S=1 (V is
      // simply unaffected) -- left unchanged, matching common real-
      // hardware and emulator behavior, not a gap.
    }
    return;
  }

  bool is_signed = (instr >> 22) & 1;
  uint32_t rd_hi = (instr >> 16) & 0xF;
  uint32_t rd_lo = (instr >> 12) & 0xF;

  uint64_t product;
  if (is_signed) {
    int64_t signed_product = static_cast<int64_t>(static_cast<int32_t>(rm_val)) *
                              static_cast<int64_t>(static_cast<int32_t>(rs_val));
    product = static_cast<uint64_t>(signed_product);
  } else {
    product = static_cast<uint64_t>(rm_val) * static_cast<uint64_t>(rs_val);
  }
  if (accumulate) {
    uint64_t acc = (static_cast<uint64_t>(regs_[rd_hi]) << 32) | regs_[rd_lo];
    product += acc;
  }
  regs_[rd_lo] = static_cast<uint32_t>(product);
  regs_[rd_hi] = static_cast<uint32_t>(product >> 32);
  if (s) {
    SetFlag(kCpsrN, (product >> 63) & 1);
    SetFlag(kCpsrZ, product == 0);
    // C, V UNPREDICTABLE -- left unchanged, same rationale as above.
  }
}

void ArmInterpreter::ExecuteExtend(uint32_t instr) {
  // ARMv6 "Extend (and add)" instructions: cond 0110 op[24:20] Rn Rd
  // rotate[11:10] 00 0111 Rm. Confirmed real encoding (not hand-derived
  // from the ARM ARM's prose) by assembling each real mnemonic with
  // arm-none-eabi-as and reading back the actual bytes: UXTB/UXTAB =
  // op 0x0E, UXTH/UXTAH = 0x0F, SXTB/SXTAB = 0x0A, SXTH/SXTAH = 0x0B --
  // Rn=0b1111 selects the plain (non-accumulate) form, any other Rn
  // selects the accumulate form (Rd = Rn + extended Rm). Needed for
  // Super BurgerTime's real `AEEMod_New` (TASKS.md Phase 8): a real
  // `uxth r0, r0` at module offset 0xdc, previously unimplemented and
  // falling into the same "Undefined instruction space" catch-all as
  // truly-reserved media-instruction encodings.
  uint32_t op = (instr >> 20) & 0xF;
  uint32_t rn = (instr >> 16) & 0xF;
  uint32_t rd = (instr >> 12) & 0xF;
  uint32_t rotate = (instr >> 10) & 0x3;
  uint32_t rm = instr & 0xF;

  // is_immediate_shift=false: the Extend family's 2-bit rotate field
  // means true "no rotation" at 0, unlike the general shifter operand's
  // "ROR #0 encodes RRX" special case (see ShiftWithCarry's doc
  // comment) -- passing true here would wrongly rotate-through-carry
  // instead of leaving the value untouched.
  bool carry_out_unused;
  uint32_t rotated = ShiftWithCarry(ReadOperandRegister(rm), /*type=*/3 /*ROR*/, rotate * 8,
                                     GetFlag(kCpsrC), carry_out_unused,
                                     /*is_immediate_shift=*/false);
  uint32_t extended;
  switch (op) {
    case 0x0E: extended = rotated & 0xFF; break;    // UXTB/UXTAB
    case 0x0F: extended = rotated & 0xFFFF; break;  // UXTH/UXTAH
    case 0x0A:                                      // SXTB/SXTAB
      extended = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(rotated & 0xFF)));
      break;
    case 0x0B:  // SXTH/SXTAH
      extended =
          static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(rotated & 0xFFFF)));
      break;
    default:
      throw UnimplementedInstruction("Reserved media-instruction encoding");
  }
  regs_[rd] = (rn == 0xF) ? extended : ReadOperandRegister(rn) + extended;
}

uint32_t ArmInterpreter::ReadThumbOperandRegister(uint32_t index) const {
  // Thumb's PC-as-operand rule (format 5 hi-register ops are the only
  // way to read R15 in Thumb state) differs from ARM's: +4, word-
  // aligned, not +8 unaligned -- see ReadOperandRegister and the class
  // doc comment.
  return index == kPC ? ((regs_[kPC] + 4) & ~3u) : regs_[index];
}

void ArmInterpreter::ExecuteThumbMoveShiftedRegister(uint16_t instr) {
  // Format 1: 000 Op[2] Offset5[5] Rs[3] Rd[3] -- LSL/LSR/ASR Rd,Rs,#Offset5
  uint32_t op = (instr >> 11) & 0x3;  // 0=LSL, 1=LSR, 2=ASR (3 is Format 2)
  uint32_t offset5 = (instr >> 6) & 0x1F;
  uint32_t rs = (instr >> 3) & 0x7;
  uint32_t rd = instr & 0x7;
  bool carry_out;
  uint32_t result = ShiftWithCarry(regs_[rs], op, offset5, GetFlag(kCpsrC),
                                    carry_out, /*is_immediate_shift=*/true);
  regs_[rd] = result;
  SetFlag(kCpsrN, (result >> 31) & 1);
  SetFlag(kCpsrZ, result == 0);
  SetFlag(kCpsrC, carry_out);
}

void ArmInterpreter::ExecuteThumbAddSubtract(uint16_t instr) {
  // Format 2: 00011 I[1] Op[1] Rn/Imm3[3] Rs[3] Rd[3]
  bool immediate = (instr >> 10) & 1;
  bool subtract = (instr >> 9) & 1;
  uint32_t rn_or_imm = (instr >> 6) & 0x7;
  uint32_t rs = (instr >> 3) & 0x7;
  uint32_t rd = instr & 0x7;
  uint32_t operand2 = immediate ? rn_or_imm : regs_[rn_or_imm];
  bool carry_out, overflow_out;
  uint32_t result = subtract
      ? AddWithFlags(regs_[rs], ~operand2, 1, carry_out, overflow_out)
      : AddWithFlags(regs_[rs], operand2, 0, carry_out, overflow_out);
  regs_[rd] = result;
  SetFlag(kCpsrN, (result >> 31) & 1);
  SetFlag(kCpsrZ, result == 0);
  SetFlag(kCpsrC, carry_out);
  SetFlag(kCpsrV, overflow_out);
}

void ArmInterpreter::ExecuteThumbMovCmpAddSubImmediate(uint16_t instr) {
  // Format 3: 001 Op[2] Rd[3] Imm8[8] -- MOV/CMP/ADD/SUB Rd,#Imm8
  uint32_t op = (instr >> 11) & 0x3;
  uint32_t rd = (instr >> 8) & 0x7;
  uint32_t imm8 = instr & 0xFF;
  uint32_t rd_val = regs_[rd];
  uint32_t result = 0;
  bool write_result = true;
  bool carry_out = GetFlag(kCpsrC), overflow_out = GetFlag(kCpsrV);
  switch (op) {
    case 0: result = imm8; break;                                                              // MOV
    case 1: result = AddWithFlags(rd_val, ~imm8, 1, carry_out, overflow_out); write_result = false; break;  // CMP
    case 2: result = AddWithFlags(rd_val, imm8, 0, carry_out, overflow_out); break;              // ADD
    case 3: result = AddWithFlags(rd_val, ~imm8, 1, carry_out, overflow_out); break;             // SUB
  }
  if (write_result) regs_[rd] = result;
  SetFlag(kCpsrN, (result >> 31) & 1);
  SetFlag(kCpsrZ, result == 0);
  if (op != 0) {  // MOV#imm8 affects only N,Z; CMP/ADD/SUB affect C,V too.
    SetFlag(kCpsrC, carry_out);
    SetFlag(kCpsrV, overflow_out);
  }
}

void ArmInterpreter::ExecuteThumbAluOperation(uint16_t instr) {
  // Format 4: 010000 Op[4] Rs[3] Rd[3]
  uint32_t op = (instr >> 6) & 0xF;
  uint32_t rs = (instr >> 3) & 0x7;
  uint32_t rd = instr & 0x7;
  uint32_t rd_val = regs_[rd];
  uint32_t rs_val = regs_[rs];
  uint32_t result = 0;
  bool write_result = true;
  // Initialized to the current flags so ops that don't affect a given
  // flag (e.g. AND leaves C/V alone) re-write it unchanged below.
  bool carry_out = GetFlag(kCpsrC);
  bool overflow_out = GetFlag(kCpsrV);
  switch (op) {
    case 0x0: result = rd_val & rs_val; break;                                                    // AND
    case 0x1: result = rd_val ^ rs_val; break;                                                     // EOR
    case 0x2: result = ShiftWithCarry(rd_val, 0, rs_val & 0xFF, carry_out, carry_out, false); break;  // LSL
    case 0x3: result = ShiftWithCarry(rd_val, 1, rs_val & 0xFF, carry_out, carry_out, false); break;  // LSR
    case 0x4: result = ShiftWithCarry(rd_val, 2, rs_val & 0xFF, carry_out, carry_out, false); break;  // ASR
    case 0x5: result = AddWithFlags(rd_val, rs_val, GetFlag(kCpsrC), carry_out, overflow_out); break;   // ADC
    case 0x6: result = AddWithFlags(rd_val, ~rs_val, GetFlag(kCpsrC), carry_out, overflow_out); break;  // SBC
    case 0x7: result = ShiftWithCarry(rd_val, 3, rs_val & 0xFF, carry_out, carry_out, false); break;  // ROR
    case 0x8: result = rd_val & rs_val; write_result = false; break;                               // TST
    case 0x9: result = AddWithFlags(0, ~rs_val, 1, carry_out, overflow_out); break;                 // NEG
    case 0xA: result = AddWithFlags(rd_val, ~rs_val, 1, carry_out, overflow_out); write_result = false; break;  // CMP
    case 0xB: result = AddWithFlags(rd_val, rs_val, 0, carry_out, overflow_out); write_result = false; break;   // CMN
    case 0xC: result = rd_val | rs_val; break;                                                     // ORR
    case 0xD: result = rd_val * rs_val; break;  // MUL -- C,V UNPREDICTABLE, left unchanged
    case 0xE: result = rd_val & ~rs_val; break;                                                    // BIC
    case 0xF: result = ~rs_val; break;                                                             // MVN
  }
  if (write_result) regs_[rd] = result;
  SetFlag(kCpsrN, (result >> 31) & 1);
  SetFlag(kCpsrZ, result == 0);
  SetFlag(kCpsrC, carry_out);
  SetFlag(kCpsrV, overflow_out);
}

void ArmInterpreter::ExecuteThumbHiRegisterOperation(uint16_t instr) {
  // Format 5: 010001 Op[2] H1[1] H2[1] Rs[3] Rd[3] -- H1/H2 extend Rd/Rs
  // to the full r0-r15 range (low-register formats above can only name
  // r0-r7). Op 3 is BX/BLX; H1 there is the link bit, not a Hd extension.
  uint32_t op = (instr >> 8) & 0x3;
  bool h1 = (instr >> 7) & 1;
  bool h2 = (instr >> 6) & 1;
  uint32_t rs = (h2 ? 8u : 0u) | ((instr >> 3) & 0x7);
  uint32_t rd = (h1 ? 8u : 0u) | (instr & 0x7);

  if (op == 0x3) {  // BX/BLX
    uint32_t target = ReadThumbOperandRegister(rs);
    if (h1) {  // BLX: real ARMv6 ISA -- H1 selects BLX(1) vs BX(0) here.
      regs_[kLR] = (regs_[kPC] + 2) | 1;
    }
    SetPcInterworking(target);
    return;
  }

  uint32_t rd_val = ReadThumbOperandRegister(rd);
  uint32_t rs_val = ReadThumbOperandRegister(rs);
  switch (op) {
    case 0x0: {  // ADD -- no flags affected (unlike the low-register formats)
      uint32_t result = rd_val + rs_val;
      if (rd == kPC) {
        regs_[kPC] = result & ~1u;  // branches, but never interworks
        pc_updated_by_instruction_ = true;
      } else {
        regs_[rd] = result;
      }
      break;
    }
    case 0x1: {  // CMP -- sets flags like a full 32-bit compare, Rd not written
      bool carry_out, overflow_out;
      uint32_t result = AddWithFlags(rd_val, ~rs_val, 1, carry_out, overflow_out);
      SetFlag(kCpsrN, (result >> 31) & 1);
      SetFlag(kCpsrZ, result == 0);
      SetFlag(kCpsrC, carry_out);
      SetFlag(kCpsrV, overflow_out);
      break;
    }
    case 0x2: {  // MOV -- no flags affected
      if (rd == kPC) {
        regs_[kPC] = rs_val & ~1u;  // branches, but never interworks
        pc_updated_by_instruction_ = true;
      } else {
        regs_[rd] = rs_val;
      }
      break;
    }
  }
}

void ArmInterpreter::ExecuteThumbPcRelativeLoad(uint16_t instr) {
  // Format 6: 01001 Rd[3] Word8[8] -- LDR Rd,[PC,#Word8*4]
  uint32_t rd = (instr >> 8) & 0x7;
  uint32_t word8 = instr & 0xFF;
  uint32_t base = (regs_[kPC] + 4) & ~3u;
  regs_[rd] = memory_.Read32(base + word8 * 4);
}

void ArmInterpreter::ExecuteThumbLoadStoreRegisterOffset(uint16_t instr) {
  // Format 7: 0101 L[1] B[1] 0 Ro[3] Rb[3] Rd[3] -- STR/LDR{B} Rd,[Rb,Ro]
  bool load = (instr >> 11) & 1;
  bool byte = (instr >> 10) & 1;
  uint32_t ro = (instr >> 6) & 0x7;
  uint32_t rb = (instr >> 3) & 0x7;
  uint32_t rd = instr & 0x7;
  uint32_t addr = regs_[rb] + regs_[ro];
  if (load) {
    regs_[rd] = byte ? memory_.Read8(addr) : memory_.Read32(addr);
  } else if (byte) {
    memory_.Write8(addr, static_cast<uint8_t>(regs_[rd]));
  } else {
    memory_.Write32(addr, regs_[rd]);
  }
}

void ArmInterpreter::ExecuteThumbLoadStoreSignExtended(uint16_t instr) {
  // Format 8: 0101 H[1] S[1] 1 Ro[3] Rb[3] Rd[3] -- STRH/LDRH/LDRSB/LDRSH
  bool h = (instr >> 11) & 1;
  bool s = (instr >> 10) & 1;
  uint32_t ro = (instr >> 6) & 0x7;
  uint32_t rb = (instr >> 3) & 0x7;
  uint32_t rd = instr & 0x7;
  uint32_t addr = regs_[rb] + regs_[ro];
  if (!s && !h) {  // STRH
    memory_.Write16(addr, static_cast<uint16_t>(regs_[rd]));
  } else if (!s && h) {  // LDRH
    regs_[rd] = memory_.Read16(addr);
  } else if (s && !h) {  // LDRSB
    auto b = static_cast<int8_t>(memory_.Read8(addr));
    regs_[rd] = static_cast<uint32_t>(static_cast<int32_t>(b));
  } else {  // LDRSH
    auto hw = static_cast<int16_t>(memory_.Read16(addr));
    regs_[rd] = static_cast<uint32_t>(static_cast<int32_t>(hw));
  }
}

void ArmInterpreter::ExecuteThumbLoadStoreImmediateOffset(uint16_t instr) {
  // Format 9: 011 B[1] L[1] Offset5[5] Rb[3] Rd[3]
  bool byte = (instr >> 12) & 1;
  bool load = (instr >> 11) & 1;
  uint32_t offset5 = (instr >> 6) & 0x1F;
  uint32_t rb = (instr >> 3) & 0x7;
  uint32_t rd = instr & 0x7;
  uint32_t addr = regs_[rb] + (byte ? offset5 : offset5 * 4);
  if (load) {
    regs_[rd] = byte ? memory_.Read8(addr) : memory_.Read32(addr);
  } else if (byte) {
    memory_.Write8(addr, static_cast<uint8_t>(regs_[rd]));
  } else {
    memory_.Write32(addr, regs_[rd]);
  }
}

void ArmInterpreter::ExecuteThumbLoadStoreHalfword(uint16_t instr) {
  // Format 10: 1000 L[1] Offset5[5] Rb[3] Rd[3] -- STRH/LDRH Rd,[Rb,#Offset5*2]
  bool load = (instr >> 11) & 1;
  uint32_t offset5 = (instr >> 6) & 0x1F;
  uint32_t rb = (instr >> 3) & 0x7;
  uint32_t rd = instr & 0x7;
  uint32_t addr = regs_[rb] + offset5 * 2;
  if (load) {
    regs_[rd] = memory_.Read16(addr);
  } else {
    memory_.Write16(addr, static_cast<uint16_t>(regs_[rd]));
  }
}

void ArmInterpreter::ExecuteThumbSpRelativeLoadStore(uint16_t instr) {
  // Format 11: 1001 L[1] Rd[3] Word8[8] -- STR/LDR Rd,[SP,#Word8*4]
  bool load = (instr >> 11) & 1;
  uint32_t rd = (instr >> 8) & 0x7;
  uint32_t word8 = instr & 0xFF;
  uint32_t addr = regs_[kSP] + word8 * 4;
  if (load) {
    regs_[rd] = memory_.Read32(addr);
  } else {
    memory_.Write32(addr, regs_[rd]);
  }
}

void ArmInterpreter::ExecuteThumbLoadAddress(uint16_t instr) {
  // Format 12: 1010 SP[1] Rd[3] Word8[8] -- ADD Rd,(PC|SP),#Word8*4
  bool sp = (instr >> 11) & 1;
  uint32_t rd = (instr >> 8) & 0x7;
  uint32_t word8 = instr & 0xFF;
  uint32_t base = sp ? regs_[kSP] : ((regs_[kPC] + 4) & ~3u);
  regs_[rd] = base + word8 * 4;
}

void ArmInterpreter::ExecuteThumbAddOffsetToSp(uint16_t instr) {
  // Format 13: 10110000 S[1] SWord7[7] -- ADD SP,#+/-SWord7*4
  bool negative = (instr >> 7) & 1;
  uint32_t offset = (instr & 0x7F) * 4;
  regs_[kSP] = negative ? regs_[kSP] - offset : regs_[kSP] + offset;
}

void ArmInterpreter::ExecuteThumbPushPop(uint16_t instr) {
  // Format 14: 1011 L[1] 10 R[1] Rlist[8] -- PUSH/POP {Rlist{,LR/PC}}
  bool load = (instr >> 11) & 1;
  bool extra = (instr >> 8) & 1;  // R: store LR (push) / load PC (pop)
  uint32_t rlist = instr & 0xFF;

  if (load) {
    // POP: address increasing from SP; loading PC interworks (real
    // ARMv5T+/v6 rule, same as ARM-state LDR/LDM into PC).
    uint32_t addr = regs_[kSP];
    for (uint32_t i = 0; i < 8; ++i) {
      if ((rlist >> i) & 1) {
        regs_[i] = memory_.Read32(addr);
        addr += 4;
      }
    }
    if (extra) {
      uint32_t value = memory_.Read32(addr);
      addr += 4;
      regs_[kSP] = addr;
      SetPcInterworking(value);
      return;
    }
    regs_[kSP] = addr;
  } else {
    // PUSH: SP decrements by the full count up front (matching STMDB),
    // then registers land at increasing addresses, LR (if R=1) highest.
    int count = 0;
    for (uint32_t i = 0; i < 8; ++i) {
      if ((rlist >> i) & 1) ++count;
    }
    if (extra) ++count;
    uint32_t addr = regs_[kSP] - count * 4;
    regs_[kSP] = addr;
    for (uint32_t i = 0; i < 8; ++i) {
      if ((rlist >> i) & 1) {
        memory_.Write32(addr, regs_[i]);
        addr += 4;
      }
    }
    if (extra) {
      memory_.Write32(addr, regs_[kLR]);
    }
  }
}

void ArmInterpreter::ExecuteThumbMultipleLoadStore(uint16_t instr) {
  // Format 15: 1100 L[1] Rb[3] Rlist[8] -- STMIA/LDMIA Rb!,{Rlist}
  bool load = (instr >> 11) & 1;
  uint32_t rb = (instr >> 8) & 0x7;
  uint32_t rlist = instr & 0xFF;
  bool rb_in_list = (rlist >> rb) & 1;
  uint32_t addr = regs_[rb];
  for (uint32_t i = 0; i < 8; ++i) {
    if (!((rlist >> i) & 1)) continue;
    if (load) {
      regs_[i] = memory_.Read32(addr);
    } else {
      memory_.Write32(addr, regs_[i]);
    }
    addr += 4;
  }
  // Real, defined special case: if Rb is loaded as part of an LDM, the
  // loaded value wins over the writeback -- skip the writeback rather
  // than clobbering what was just loaded into it.
  if (!(load && rb_in_list)) {
    regs_[rb] = addr;
  }
}

void ArmInterpreter::ExecuteThumbConditionalBranch(uint16_t instr) {
  // Format 16: 1101 Cond[4] SOffset8[8]
  uint32_t cond = (instr >> 8) & 0xF;
  if (cond == 0xF) {
    throw UnimplementedInstruction("Thumb SWI (format 17) not supported");
  }
  if (cond == 0xE) {
    throw UnimplementedInstruction(
        "Thumb undefined instruction space (format 16, cond=1110)");
  }
  if (!ConditionPassed(cond)) return;
  auto soffset8 = static_cast<int8_t>(instr & 0xFF);
  regs_[kPC] = static_cast<uint32_t>(static_cast<int32_t>(regs_[kPC] + 4) +
                                      static_cast<int32_t>(soffset8) * 2);
  pc_updated_by_instruction_ = true;
}

void ArmInterpreter::ExecuteThumbUnconditionalBranch(uint16_t instr) {
  // Format 18: 11100 Offset11[11]
  uint32_t offset11 = instr & 0x7FF;
  // Sign-extends the 11-bit field and doubles it (halfword -> byte
  // offset) in one arithmetic shift: <<21 puts the field's sign bit at
  // bit 31, and >>20 (rather than >>21) nets an extra <<1.
  int32_t signed_offset = static_cast<int32_t>(offset11 << 21) >> 20;
  regs_[kPC] = static_cast<uint32_t>(static_cast<int32_t>(regs_[kPC] + 4) + signed_offset);
  pc_updated_by_instruction_ = true;
}

void ArmInterpreter::ExecuteThumbLongBranchWithLink(uint16_t instr) {
  // Format 19: two halfwords, both dispatched here (H = bits[12:11]):
  //   H=10 (first half):  LR = PC(fetch+4) + SignExtend(Offset11)<<12
  //   H=11 (second half, BL):  completes the branch, stays in Thumb
  //   H=01 (second half, BLX(1)): completes the branch, switches to ARM
  uint32_t h = (instr >> 11) & 0x3;
  uint32_t offset11 = instr & 0x7FF;

  if (h == 0x2) {
    // Same sign-extend-and-shift trick as Format 18, but shifted left
    // by 12 instead of 1: <<21 then >>9 nets <<12 after sign-extension.
    int32_t signed_high = static_cast<int32_t>(offset11 << 21) >> 9;
    regs_[kLR] = static_cast<uint32_t>(static_cast<int32_t>(regs_[kPC] + 4) + signed_high);
    return;
  }

  uint32_t target = regs_[kLR] + (offset11 << 1);
  uint32_t return_addr = (regs_[kPC] + 2) | 1;  // next instr; bit0 set for a later interworking return
  if (h == 0x1) {  // BLX(1): unconditional switch to ARM, word-aligned target.
    regs_[kLR] = return_addr;
    SetFlag(kCpsrT, false);
    regs_[kPC] = target & ~3u;
  } else {  // BL (h == 0x3): stays in Thumb.
    regs_[kLR] = return_addr;
    regs_[kPC] = target & ~1u;
  }
  pc_updated_by_instruction_ = true;
}

void ArmInterpreter::ExecuteThumb(uint16_t instr) {
  if ((instr & 0xF800) == 0x1800) {  // 00011xxx -- Format 2 (checked before
    ExecuteThumbAddSubtract(instr);  // Format 1's broader 000xx prefix)
  } else if ((instr & 0xE000) == 0x0000) {  // 000xxxxx -- Format 1
    ExecuteThumbMoveShiftedRegister(instr);
  } else if ((instr & 0xE000) == 0x2000) {  // 001xxxxx -- Format 3
    ExecuteThumbMovCmpAddSubImmediate(instr);
  } else if ((instr & 0xFC00) == 0x4000) {  // 010000xx -- Format 4
    ExecuteThumbAluOperation(instr);
  } else if ((instr & 0xFC00) == 0x4400) {  // 010001xx -- Format 5
    ExecuteThumbHiRegisterOperation(instr);
  } else if ((instr & 0xF800) == 0x4800) {  // 01001xxx -- Format 6
    ExecuteThumbPcRelativeLoad(instr);
  } else if ((instr & 0xF000) == 0x5000) {  // 0101xxxx -- Format 7/8
    if ((instr & 0x0200) == 0) {
      ExecuteThumbLoadStoreRegisterOffset(instr);
    } else {
      ExecuteThumbLoadStoreSignExtended(instr);
    }
  } else if ((instr & 0xE000) == 0x6000) {  // 011xxxxx -- Format 9
    ExecuteThumbLoadStoreImmediateOffset(instr);
  } else if ((instr & 0xF000) == 0x8000) {  // 1000xxxx -- Format 10
    ExecuteThumbLoadStoreHalfword(instr);
  } else if ((instr & 0xF000) == 0x9000) {  // 1001xxxx -- Format 11
    ExecuteThumbSpRelativeLoadStore(instr);
  } else if ((instr & 0xF000) == 0xA000) {  // 1010xxxx -- Format 12
    ExecuteThumbLoadAddress(instr);
  } else if ((instr & 0xFF00) == 0xB000) {  // 10110000 -- Format 13 (checked
    ExecuteThumbAddOffsetToSp(instr);       // before Format 14's broader mask)
  } else if ((instr & 0xF600) == 0xB400) {  // 1011x10x -- Format 14
    ExecuteThumbPushPop(instr);
  } else if ((instr & 0xF000) == 0xC000) {  // 1100xxxx -- Format 15
    ExecuteThumbMultipleLoadStore(instr);
  } else if ((instr & 0xF000) == 0xD000) {  // 1101xxxx -- Format 16
    ExecuteThumbConditionalBranch(instr);   // (SWI/undef handled inside)
  } else if ((instr & 0xF800) == 0xE000) {  // 11100xxx -- Format 18 (checked
    ExecuteThumbUnconditionalBranch(instr);  // before Format 19's broader mask)
  } else if ((instr & 0xE000) == 0xE000) {  // 111xxxxx, excluding Format 18
    ExecuteThumbLongBranchWithLink(instr);  // above -- Format 19's 3 halfwords
  } else {
    throw UnimplementedInstruction("Unrecognized Thumb instruction encoding");
  }
}

void ArmInterpreter::Step() {
  uint32_t fetch_addr = regs_[kPC];
  if (call_out_size_ != 0 && fetch_addr >= call_out_base_ &&
      fetch_addr < call_out_base_ + call_out_size_) {
    if (call_out_handler_) {
      call_out_handler_(*this, fetch_addr);
    }
    return;
  }

  pc_updated_by_instruction_ = false;

  if (GetFlag(kCpsrT)) {
    uint16_t thumb_instr = memory_.Read16(fetch_addr);
    ExecuteThumb(thumb_instr);
    if (!pc_updated_by_instruction_) {
      regs_[kPC] = fetch_addr + 2;
    }
    return;
  }

  uint32_t instr = memory_.Read32(fetch_addr);

  uint32_t cond = (instr >> 28) & 0xF;
  if (ConditionPassed(cond)) {
    uint32_t bits27_25 = (instr >> 25) & 0x7;

    if (bits27_25 == 0b101) {
      ExecuteBranch(instr);
    } else if (bits27_25 == 0b011 && (instr & 0x10) != 0) {
      // ARMv6 "Media instructions" space. Only the Extend (and add)
      // family (SXTB/SXTH/UXTB/UXTH and their accumulate forms) is
      // implemented -- see ExecuteExtend's doc comment for the real,
      // confirmed encoding. Everything else in this space (parallel
      // arithmetic, SIMD, saturate, USAD8, ...) is genuinely
      // unimplemented, not yet needed by any real game probed so far.
      uint32_t bits27_20 = (instr >> 20) & 0xFF;
      uint32_t bits9_4 = (instr >> 4) & 0x3F;
      bool is_extend = bits9_4 == 0b000111 &&
                        (bits27_20 == 0x6A || bits27_20 == 0x6B || bits27_20 == 0x6E ||
                         bits27_20 == 0x6F);
      if ((instr & 0x0FFF0FF0) == 0x06BF0F30) {
        // REV Rd, Rm: cond 0110 1011 1111 Rd 1111 0011 Rm -- real ARMv6
        // byte-reverse-word instruction, sharing this same "media
        // instructions" encoding space with the Extend family (same
        // bits[27:20]=0x6B as UXTB/UXTAB, disambiguated by bits[9:4]:
        // 0b110011 here vs. Extend's 0b000111). Found live in Heavy
        // Weapon: a real loop reads a word, byte-reverses it, writes it
        // back, repeated over a loaded data block -- a real endian-swap
        // routine.
        uint32_t rd = (instr >> 12) & 0xF;
        uint32_t rm = instr & 0xF;
        uint32_t val = ReadOperandRegister(rm);
        regs_[rd] = ((val & 0x000000FF) << 24) | ((val & 0x0000FF00) << 8) |
                    ((val & 0x00FF0000) >> 8) | ((val & 0xFF000000) >> 24);
      } else if ((instr & 0x0FFF0FF0) == 0x06BF0FB0) {
        // REV16 Rd, Rm: cond 0110 1011 1111 Rd 1111 1011 Rm -- REV's
        // sibling, swapping bytes within each 16-bit half independently
        // rather than reversing the whole word (bits[7:4]=1011 vs.
        // REV's 0011). Found live immediately after REV in this same
        // Heavy Weapon routine, in an identical loop shape but over
        // 16-bit halfwords instead of 32-bit words.
        uint32_t rd = (instr >> 12) & 0xF;
        uint32_t rm = instr & 0xF;
        uint32_t val = ReadOperandRegister(rm);
        regs_[rd] = ((val & 0x000000FF) << 8) | ((val & 0x0000FF00) >> 8) |
                    ((val & 0x00FF0000) << 8) | ((val & 0xFF000000) >> 8);
      } else if (!is_extend) {
        throw UnimplementedInstruction("Undefined instruction space");
      } else {
        ExecuteExtend(instr);
      }
    } else if (bits27_25 == 0b011) {
      ExecuteSingleDataTransfer(instr);
    } else if (bits27_25 == 0b010) {
      ExecuteSingleDataTransfer(instr);
    } else if (bits27_25 == 0b000 || bits27_25 == 0b001) {
      uint32_t opcode = (instr >> 21) & 0xF;
      uint32_t s_bit = (instr >> 20) & 1;
      uint32_t bit25 = (instr >> 25) & 1;
      uint32_t bit4 = (instr >> 4) & 1;
      uint32_t bit7 = (instr >> 7) & 1;

      // Must be checked BEFORE the opcode/S-bit "miscellaneous
      // instructions" reassignment below: bits[24:21] only mean "opcode"
      // for true data-processing-shaped instructions. Multiply/halfword
      // encodings reuse those same bit positions for P/U/I/W-type fields
      // that can numerically collide with the 0x8-0xB opcode range
      // (e.g. STRH's immediate/down-indexed form does exactly this) --
      // checking bit4/bit7 first routes those correctly regardless.
      bool is_test_opcode = opcode >= 0x8 && opcode <= 0xB;
      if (bit25 == 0 && bit4 == 1 && bit7 == 1) {
        // Shared bit pattern for multiply/swap (SH=00) and halfword/
        // signed transfer (SH=01/10/11) -- swap (see below) still stays
        // unimplemented.
        uint32_t sh = (instr >> 5) & 0x3;
        if (sh == 0) {
          // Multiply-family space, further split by bits[24:23]: 00 =
          // MUL/MLA, 01 = long multiply (UMULL/UMLAL/SMULL/SMLAL), 10 =
          // swap (SWP/SWPB, still unimplemented), 11 = reserved.
          uint32_t bits24_23 = (instr >> 23) & 0x3;
          if (bits24_23 == 0b00 || bits24_23 == 0b01) {
            ExecuteMultiply(instr);
          } else {
            throw UnimplementedInstruction("Swap (SWP/SWPB) or reserved multiply-space encoding");
          }
        } else {
          ExecuteHalfwordTransfer(instr);
        }
      } else if (is_test_opcode && s_bit == 0) {
        // This opcode range with S=0 is reassigned to the "miscellaneous
        // instructions" space (MRS/MSR/BX/BLX/CLZ/BXJ/...), not a real
        // TST/TEQ/CMP/CMN. BX/BLX Rm's bit pattern is:
        // cond 0001 0010 1111 1111 1111 001L Rm (L = link bit, bit 5) --
        // everything else in this space stays unimplemented.
        if ((instr & 0x0FFFFFD0) == 0x012FFF10) {
          ExecuteBranchExchange(instr);
        } else if ((instr & 0x0FFF0FF0) == 0x016F0F10) {
          // CLZ Rd, Rm: cond 0001 0110 1111 Rd 1111 0001 Rm -- real
          // ARMv5T+ instruction sharing this same "miscellaneous"
          // encoding space with MRS/MSR/BX/BLX. Found live in Zeebo
          // Sports Tênis (soft-float normalization code): counts
          // leading zero bits, 32 if the input is zero.
          uint32_t rd = (instr >> 12) & 0xF;
          uint32_t rm = instr & 0xF;
          regs_[rd] = static_cast<uint32_t>(std::countl_zero(ReadOperandRegister(rm)));
        } else if ((instr & 0x0FF0F090) == 0x01600080) {
          // SMULxy Rd, Rm, Rs: cond 0001 0110 Rd 0000 Rs 1yx0 Rm -- real
          // ARMv5TE DSP signed 16x16 multiply, sharing this same
          // "miscellaneous" encoding space. Found live in Zeeboids (a
          // per-entity update routine): x/y (bits 5/6) each pick the
          // bottom or top halfword of Rm/Rs respectively; both are
          // sign-extended to 32 bits before multiplying, and (unlike
          // SMLAxy) there's no accumulate term and flags are untouched.
          uint32_t rd = (instr >> 16) & 0xF;
          uint32_t rs = (instr >> 8) & 0xF;
          uint32_t rm = instr & 0xF;
          bool y = (instr >> 6) & 1;
          bool x = (instr >> 5) & 1;
          uint32_t rm_val = ReadOperandRegister(rm);
          uint32_t rs_val = ReadOperandRegister(rs);
          int32_t rm_half = static_cast<int16_t>(x ? (rm_val >> 16) : (rm_val & 0xFFFF));
          int32_t rs_half = static_cast<int16_t>(y ? (rs_val >> 16) : (rs_val & 0xFFFF));
          regs_[rd] = static_cast<uint32_t>(rm_half * rs_half);
        } else {
          throw UnimplementedInstruction(
              "Miscellaneous instruction space (MRS/MSR/etc.)");
        }
      } else {
        ExecuteDataProcessing(instr);
      }
    } else if (bits27_25 == 0b100) {
      bool s_bit = (instr >> 22) & 1;
      if (s_bit) {
        throw UnimplementedInstruction(
            "Block data transfer with S=1 (user-bank registers / "
            "exception return) not supported");
      }
      ExecuteBlockDataTransfer(instr);
    } else {
      throw UnimplementedInstruction("Coprocessor instruction / SWI");
    }
  }

  if (!pc_updated_by_instruction_) {
    regs_[kPC] = fetch_addr + 4;
  }
}

uint64_t ArmInterpreter::Run(uint64_t max_instructions) {
  uint64_t executed = 0;
  while (executed < max_instructions) {
    uint32_t fetch_addr = regs_[kPC];
    bool will_trap = call_out_size_ != 0 && fetch_addr >= call_out_base_ &&
                      fetch_addr < call_out_base_ + call_out_size_;
    Step();
    ++executed;
    if (will_trap) break;
  }
  return executed;
}

}  // namespace zeebulator

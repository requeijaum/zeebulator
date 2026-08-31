#pragma once

#include <array>
#include <istream>
#include <ostream>
#include <stdexcept>

#include "core/cpu/arm_core.h"

namespace zeebulator {

// Thrown for any instruction encoding this v1 interpreter doesn't yet
// implement. Deliberately loud rather than silently mis-executing —
// see TASKS.md Phase 1 for what's covered vs. deferred.
class UnimplementedInstruction : public std::runtime_error {
 public:
  explicit UnimplementedInstruction(const std::string& what)
      : std::runtime_error(what) {}
};

// v2 ARMv6 (ARM1136J-S) interpreter: ARM (A32) state plus Thumb (T16)
// state, with ARM/Thumb interworking. ARM state covers data processing,
// branch, single word/byte load-store, block data transfer (LDM/STM,
// user-bank/exception-return variants excluded), halfword/signed
// transfer (STRH/LDRH/LDRSB/LDRSH), BX/BLX, and multiply/
// multiply-accumulate (MUL/MLA, plus long multiply UMULL/UMLAL/SMULL/
// SMLAL). Swap (SWP/SWPB), PSR transfer, SWI, and coprocessor
// instructions raise UnimplementedInstruction.
//
// Thumb support was added after real disassembly of a second real game
// (Peggle, `peggle.mod` — see PHASE8_LOG.md) hit a real BX into Thumb
// code that Double Dragon's own `.mod` never exercised. Covers all 19
// real Thumb instruction format groups from the ARM Architecture
// Reference Manual's Thumb instruction set summary except software
// interrupt (format 17, SWI — unimplemented, matching ARM state's own
// SWI handling) and the format-16 SWI/undefined condition codes
// (0b1111/0b1110). Interworking (BX/BLX register-form in both states,
// Thumb's long-branch BLX(1), and loading PC directly via LDR/LDM in
// ARM state or POP in Thumb state) all check the target address's bit 0
// to select the resulting instruction state, per the real architecture
// rule introduced in ARMv5T and present in ARMv6. Plain, non-
// interworking writes to PC (ARM data-processing results, Thumb format
// 5 MOV/ADD/CMP with Rd=PC, and any branch computed from a fixed
// instruction-stream offset) never change instruction state — only an
// explicit interworking write does.
class ArmInterpreter : public IArmCore {
 public:
  ArmInterpreter();

  void Reset() override;
  void Step() override;
  uint64_t Run(uint64_t max_instructions) override;

  uint32_t GetRegister(int index) const override;
  void SetRegister(int index, uint32_t value) override;

  uint32_t GetCpsr() const override;
  void SetCpsr(uint32_t value) override;

  Memory& GetMemory() override { return memory_; }

  void SetCallOutRange(uint32_t base, uint32_t size) override;
  void SetCallOutHandler(CallOutHandler handler) override;

  // Save-state support (TASKS_TOOLING.md Phase B, stage 1): registers,
  // CPSR, and the full guest memory image (see Memory::Serialize).
  // Deliberately does NOT cover call_out_base_/call_out_size_/
  // call_out_handler_ -- those are wiring the harness (game_probe.cpp)
  // sets up once at construction, not resumable game state, and
  // call_out_handler_ (a std::function closing over the harness's own
  // locals) couldn't be meaningfully serialized anyway. A loaded state
  // is only valid to apply to a core the harness has already finished
  // setting up the same way.
  bool Serialize(std::ostream& out) const;
  bool Deserialize(std::istream& in);

 private:
  struct Operand2Result {
    uint32_t value;
    bool carry_out;
  };

  bool ConditionPassed(uint32_t cond) const;
  bool GetFlag(CpsrBit bit) const;
  void SetFlag(CpsrBit bit, bool value);

  // Reads a register the way the ISA reads it as an *operand* — R15 reads
  // as (address of the currently-executing instruction) + 8, per the ARM
  // architecture's pipeline-based PC semantics. regs_[kPC] itself always
  // holds the plain, unadjusted address (see .cpp for the invariant this
  // interpreter maintains), so this is the only place the +8 is added.
  uint32_t ReadOperandRegister(uint32_t index) const;

  Operand2Result DecodeOperand2(uint32_t instr) const;

  void ExecuteDataProcessing(uint32_t instr);
  void ExecuteBranch(uint32_t instr);
  void ExecuteSingleDataTransfer(uint32_t instr);
  void ExecuteBlockDataTransfer(uint32_t instr);
  void ExecuteBranchExchange(uint32_t instr);
  void ExecuteHalfwordTransfer(uint32_t instr);
  void ExecuteMultiply(uint32_t instr);
  void ExecuteExtend(uint32_t instr);

  // Sets PC from an interworking write (BX/BLX in either state, LDR/LDM
  // into PC in ARM state, POP into PC in Thumb state, and the BLX(1)
  // long-branch's implicit switch): bit 0 of `target` selects Thumb (1)
  // or ARM (0), the CPSR T-bit is updated to match, and PC is set to
  // `target` with bit 0 (Thumb) or bits[1:0] (ARM) cleared so it's
  // always correctly aligned for whichever state was just selected.
  void SetPcInterworking(uint32_t target);

  // Reads a register the way *Thumb* code reads it as an operand — R15
  // reads as ((address of the currently-executing instruction) + 4)
  // word-aligned, per the Thumb instruction set's PC semantics (distinct
  // from ARM state's +8, unaligned rule — see ReadOperandRegister).
  // Only reachable for R15 via Thumb format 5 (hi-register operations).
  uint32_t ReadThumbOperandRegister(uint32_t index) const;

  void ExecuteThumb(uint16_t instr);
  void ExecuteThumbMoveShiftedRegister(uint16_t instr);
  void ExecuteThumbAddSubtract(uint16_t instr);
  void ExecuteThumbMovCmpAddSubImmediate(uint16_t instr);
  void ExecuteThumbAluOperation(uint16_t instr);
  void ExecuteThumbHiRegisterOperation(uint16_t instr);
  void ExecuteThumbPcRelativeLoad(uint16_t instr);
  void ExecuteThumbLoadStoreRegisterOffset(uint16_t instr);
  void ExecuteThumbLoadStoreSignExtended(uint16_t instr);
  void ExecuteThumbLoadStoreImmediateOffset(uint16_t instr);
  void ExecuteThumbLoadStoreHalfword(uint16_t instr);
  void ExecuteThumbSpRelativeLoadStore(uint16_t instr);
  void ExecuteThumbLoadAddress(uint16_t instr);
  void ExecuteThumbAddOffsetToSp(uint16_t instr);
  void ExecuteThumbPushPop(uint16_t instr);
  void ExecuteThumbMultipleLoadStore(uint16_t instr);
  void ExecuteThumbConditionalBranch(uint16_t instr);
  void ExecuteThumbUnconditionalBranch(uint16_t instr);
  void ExecuteThumbLongBranchWithLink(uint16_t instr);

  std::array<uint32_t, 16> regs_{};
  uint32_t cpsr_ = 0;
  Memory memory_;

  // Set by an instruction that explicitly changes control flow (branch,
  // or a data-processing/load writing to R15), so Step() knows not to
  // also apply the default PC += 4.
  bool pc_updated_by_instruction_ = false;

  uint32_t call_out_base_ = 0;
  uint32_t call_out_size_ = 0;
  CallOutHandler call_out_handler_;
};

}  // namespace zeebulator

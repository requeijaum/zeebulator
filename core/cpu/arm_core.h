#pragma once

#include <cstdint>
#include <functional>

#include "core/memory/memory.h"

namespace zeebulator {

enum ArmRegister : int {
  kR0 = 0,
  kR1,
  kR2,
  kR3,
  kR4,
  kR5,
  kR6,
  kR7,
  kR8,
  kR9,
  kR10,
  kR11,
  kR12,
  kSP = 13,
  kLR = 14,
  kPC = 15,
};

// CPSR condition flag bit positions.
enum CpsrBit : uint32_t {
  kCpsrN = 31,  // Negative
  kCpsrZ = 30,  // Zero
  kCpsrC = 29,  // Carry
  kCpsrV = 28,  // Overflow
  kCpsrT = 5,   // Thumb state
};

// Interface to the emulated ARM1136J-S application core. See
// ARCHITECTURE.md 3.1. Implementations execute ARM (A32) instructions
// against a Memory instance and expose register/flag state to the rest of
// the emulator (loader for initial setup, HLE layer via the call-out
// trap hook).
class IArmCore {
 public:
  virtual ~IArmCore() = default;

  virtual void Reset() = 0;

  // Executes exactly one instruction (or dispatches one call-out trap).
  virtual void Step() = 0;

  // Executes up to `max_instructions`, stopping early if a call-out trap
  // fires. Returns the number of instructions actually executed.
  virtual uint64_t Run(uint64_t max_instructions) = 0;

  virtual uint32_t GetRegister(int index) const = 0;
  virtual void SetRegister(int index, uint32_t value) = 0;

  virtual uint32_t GetCpsr() const = 0;
  virtual void SetCpsr(uint32_t value) = 0;

  virtual Memory& GetMemory() = 0;

  // Call-out trapping (ARCHITECTURE.md 3.4): BREW's AEE interfaces are
  // vtables of function pointers. The loader/HLE layer points vtable
  // slots at addresses inside [trap_base, trap_base + trap_size) that are
  // never backed by real code. When PC enters that range instead of
  // fetching/decoding, the core invokes the registered handler with the
  // trapped address and yields control back to the caller of Step()/Run()
  // without treating it as a fault.
  using CallOutHandler = std::function<void(IArmCore& core, uint32_t address)>;
  virtual void SetCallOutRange(uint32_t base, uint32_t size) = 0;
  virtual void SetCallOutHandler(CallOutHandler handler) = 0;
};

}  // namespace zeebulator

// Hand-assembled ARM demo: sums 1..10 in a loop, entirely on the v1
// interpreter, with no BREW/loader involved. There's no I/O within the
// emulated program itself (no BREW yet) -- the host prints the result
// after Run() returns. Useful as a quick "does the CPU core actually
// work" sanity check while there's nothing playable yet.
//
//   0x00  MOV R0, #0      ; sum = 0
//   0x04  MOV R1, #1      ; i = 1
//   0x08  MOV R2, #11     ; limit = 11
//   0x0C  CMP R1, R2      ; loop:
//   0x10  BGE #0x20       ; if i >= limit, exit
//   0x14  ADD R0, R0, R1  ; sum += i
//   0x18  ADD R1, R1, #1  ; i++
//   0x1C  B    #0x0C      ; loop
//   0x20  ; done -- host stops calling Run() here, nothing fetched

#include <cstdio>

#include "core/cpu/arm_interpreter.h"

int main() {
  zeebulator::ArmInterpreter cpu;
  auto& mem = cpu.GetMemory();

  mem.Write32(0x00, 0xE3A00000);  // MOV R0, #0
  mem.Write32(0x04, 0xE3A01001);  // MOV R1, #1
  mem.Write32(0x08, 0xE3A0200B);  // MOV R2, #11
  mem.Write32(0x0C, 0xE1510002);  // CMP R1, R2
  mem.Write32(0x10, 0xAA000002);  // BGE #0x20
  mem.Write32(0x14, 0xE0800001);  // ADD R0, R0, R1
  mem.Write32(0x18, 0xE2811001);  // ADD R1, R1, #1
  mem.Write32(0x1C, 0xEAFFFFFA);  // B #0x0C

  // 3 setup instructions + 10 full loop iterations (5 instructions each)
  // + the final CMP/BGE pair that exits the loop.
  uint64_t executed = cpu.Run(3 + 10 * 5 + 2);

  uint32_t sum = cpu.GetRegister(zeebulator::kR0);
  std::printf("Zeebulator CPU demo: sum(1..10) computed on-core = %u\n", sum);
  std::printf("  (%llu instructions executed, expected 55)\n",
              static_cast<unsigned long long>(executed));
  return sum == 55 ? 0 : 1;
}

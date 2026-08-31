// Dev tool: loads a raw .mod file at a chosen base address and
// single-steps the interpreter, printing each instruction, to test
// whether .mod is a flat, headerless, position-independent ARM binary
// starting directly with code at file offset 0. Real-file validation
// only -- takes a path at runtime, never embeds game content.

#include <cstdio>
#include <fstream>
#include <iterator>

#include "core/cpu/arm_interpreter.h"
#include "core/loader/mod.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <file.mod> [num_instructions] [base_addr_hex]\n", argv[0]);
    return 1;
  }
  int count = argc >= 3 ? std::atoi(argv[2]) : 40;
  uint32_t base = argc >= 4 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 16)) : 0x00100000;

  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "error: couldn't open '%s'\n", argv[1]);
    return 1;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  std::printf("loaded %zu bytes, base=0x%08x\n", data.size(), base);

  zeebulator::ArmInterpreter cpu;
  zeebulator::LoadMod(cpu, data, base);
  cpu.SetRegister(zeebulator::kSP, base + 0x00200000);  // arbitrary stack, well past the module

  for (int i = 0; i < count; ++i) {
    uint32_t pc = cpu.GetRegister(zeebulator::kPC);
    uint32_t instr = cpu.GetMemory().Read32(pc);
    std::printf("[%3d] pc=0x%08x instr=0x%08x  ", i, pc, instr);
    try {
      cpu.Step();
      std::printf("-> r0=%08x r1=%08x sp=%08x lr=%08x cpsr=%08x\n",
                  cpu.GetRegister(zeebulator::kR0), cpu.GetRegister(zeebulator::kR1),
                  cpu.GetRegister(zeebulator::kSP), cpu.GetRegister(zeebulator::kLR),
                  cpu.GetCpsr());
    } catch (const std::exception& e) {
      std::printf("STOPPED: %s\n", e.what());
      break;
    }
  }
  return 0;
}

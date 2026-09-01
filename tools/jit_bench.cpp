// Phase 9a Phase 4: microbenchmark. Runs a real .mod's guest stream on one
// core for a fixed number of steps and reports instructions/second.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <vector>
#include "core/cpu/arm_interpreter.h"
#include "core/cpu/dynarmic_arm_core.h"
#include "core/loader/mod.h"
using namespace zeebulator;
static std::vector<uint8_t> R(const char*p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void seed(IArmCore&c,const std::vector<uint8_t>&m){
  c.Reset();LoadMod(c,m,0x100000);c.GetMemory().Write32(0x100000-4,0x100000);
  c.SetRegister(kSP,0x100000+(uint32_t)m.size()+0x200000);c.SetRegister(kLR,0xF0000000);
  c.SetRegister(kPC,0x100000);c.SetCallOutRange(0xF0000000,0x10000);
  c.SetCallOutHandler([](IArmCore&x,uint32_t){x.SetRegister(kR0,0);x.SetRegister(kPC,x.GetRegister(kLR));});}
static double bench(IArmCore&c,const std::vector<uint8_t>&m,uint64_t steps,uint64_t&done){
  seed(c,m);auto t0=std::chrono::steady_clock::now();uint64_t i=0;
  // Block mode: Run() executes JIT blocks between call-outs. On a trap, service
  // it (handler sets PC=LR) and continue until budget or top-level return.
  // The reference interpreter may hit an UnimplementedInstruction on a deep
  // real stream (coprocessor/SWI without the full HLE); measure up to there.
  try{
    while(i<steps){uint32_t pc=c.GetRegister(kPC);if(pc==0xF0000000&&i>0)break;
      uint64_t ran=c.Run(steps-i);if(ran==0)break;i+=ran;}
  }catch(const std::exception&){/* stop at first unsupported insn */}
  auto t1=std::chrono::steady_clock::now();done=i;
  return std::chrono::duration<double>(t1-t0).count();}
int main(int argc,char**argv){
  if(argc<2){std::fprintf(stderr,"usage: %s <mod> [steps]\n",argv[0]);return 2;}
  uint64_t steps=argc>=3?std::strtoull(argv[2],0,0):2000000;
  auto m=R(argv[1]);
  ArmInterpreter I;DynarmicArmCore J;
  uint64_t di=0,dj=0;
  double ti=bench(I,m,steps,di);
  // Cap the JIT to exactly the interpreter's executed count for a fair,
  // identical-workload comparison (the interpreter may stop early on an
  // unsupported instruction; the JIT no-ops it, so bound it to di).
  double tj=bench(J,m,di>0?di:steps,dj);
  double mi=di/ti/1e6, mj=dj/tj/1e6;
  printf("interp: %llu steps in %.3fs = %.2f MIPS\n",(unsigned long long)di,ti,mi);
  printf("jit   : %llu steps in %.3fs = %.2f MIPS\n",(unsigned long long)dj,tj,mj);
  printf("speedup(jit/interp) = %.2fx\n",mi>0?mj/mi:0.0);
  return 0;}

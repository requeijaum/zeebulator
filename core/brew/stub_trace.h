#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_core.h"

namespace zeebulator {

// Stub que diz o proprio nome quando e chamado.
//
// Um `Stub` mudo devolve 0 e some. Isso e invisivel por construcao: o
// titulo chama um metodo que nao existe, recebe zero, e segue desenhando
// nada -- sem crash, sem wander, sem uma linha de log. Foi assim que o
// cluster IDLE ficou opaco por rodadas.
//
// Aqui cada slot nao implementado se identifica sob ZEEB_STUB_TRACE=1,
// com interface, slot, contagem e argumentos. O comportamento (r0=0) e
// identico ao stub mudo, entao ligar o trace nao muda execucao nenhuma.
inline HleRuntime::HleFunction LoggedStub(const char* iface, int slot, const char* name) {
  return [iface, slot, name](IArmCore& core) {
    if (std::getenv("ZEEB_STUB_TRACE") != nullptr) {
      static std::map<std::string, uint64_t> hits;
      std::string key = std::string(iface) + "::" + name;
      uint64_t n = ++hits[key];
      // Amostra em vez de inundar: primeiras ocorrencias e depois decadas.
      if (n == 1 || n == 10 || n == 100 || (n % 1000) == 0) {
        std::printf("[stub] %-28s slot=%-3d hits=%llu  r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x\n",
                    key.c_str(), slot, static_cast<unsigned long long>(n),
                    core.GetRegister(kR0), core.GetRegister(kR1),
                    core.GetRegister(kR2), core.GetRegister(kR3));
      }
    }
    core.SetRegister(kR0, 0);
  };
}

}  // namespace zeebulator

#include "core/brew/interface_object.h"

#include <cstdio>
#include <map>
#include <string>

namespace zeebulator {
namespace {

// Registro de todas as regioes de vtable ja escritas, para detectar duas
// classes ocupando o mesmo endereco.
//
// Motivo real: AEECLSID_SOUND usava vtable=0x8006E000 e a colecao da Z-Wheel
// (0x0100104f) foi registrada nos mesmos enderecos. A vtable do ISOUND foi
// sobrescrita e o jogo estourou em pc=0x8006e008 -- um sintoma que parece bug
// de CPU ou do guest, e levou a conclusao errada de que o problema era o
// scaffold da colecao. As faixas de vtable/objeto sao atribuidas a mao e nada
// checava sobreposicao.
//
// Aviso, nao erro fatal: um frontend pode legitimamente reconstruir o mesmo
// objeto (por exemplo ao recarregar), e transformar isso em falha quebraria
// casos que hoje funcionam. O que importa e que a colisao apareca no log em vez
// de virar um estouro sem explicacao.
struct VtableRegion {
  uint32_t end;
  std::string owner;
};

std::map<uint32_t, VtableRegion>& VtableRegistry() {
  static std::map<uint32_t, VtableRegion> registry;
  return registry;
}

void NoteVtableRegion(uint32_t start, uint32_t slot_count, const std::string& owner) {
  const uint32_t end = start + slot_count * 4;
  auto& registry = VtableRegistry();
  for (const auto& [other_start, other] : registry) {
    if (start < other.end && other_start < end) {
      if (other_start == start && other.owner == owner) continue;  // mesmo dono, reconstrucao
      std::fprintf(stderr,
                   "[hle] AVISO: vtable 0x%08x..0x%08x (%s) se sobrepoe a "
                   "0x%08x..0x%08x (%s) -- uma das duas sera sobrescrita\n",
                   start, end, owner.c_str(), other_start, other.end,
                   other.owner.c_str());
      break;
    }
  }
  registry[start] = VtableRegion{end, owner};
}

}  // namespace

uint32_t BuildInterfaceObject(Memory& memory, HleRuntime& hle,
                               uint32_t vtable_address, uint32_t object_address,
                               const std::vector<HleRuntime::HleFunction>& methods) {
  NoteVtableRegion(vtable_address, static_cast<uint32_t>(methods.size()), "<sem rotulo>");
  for (size_t i = 0; i < methods.size(); ++i) {
    uint32_t sentinel = hle.Register(methods[i]);
    memory.Write32(vtable_address + static_cast<uint32_t>(i) * 4, sentinel);
  }
  memory.Write32(object_address, vtable_address);
  return object_address;
}

uint32_t BuildInterfaceObjectLabeled(
    Memory& memory, HleRuntime& hle, uint32_t vtable_address,
    uint32_t object_address,
    const std::vector<HleRuntime::HleFunction>& methods,
    const std::string& interface_name,
    const std::vector<const char*>& method_names) {
  NoteVtableRegion(vtable_address, static_cast<uint32_t>(methods.size()), interface_name);
  for (size_t i = 0; i < methods.size(); ++i) {
    char buf[128];
    const char* mname =
        (i < method_names.size() && method_names[i]) ? method_names[i] : nullptr;
    if (mname) {
      std::snprintf(buf, sizeof(buf), "%s::slot%zu %s", interface_name.c_str(),
                    i, mname);
    } else {
      std::snprintf(buf, sizeof(buf), "%s::slot%zu", interface_name.c_str(), i);
    }
    uint32_t sentinel = hle.RegisterLabeled(methods[i], std::string(buf));
    memory.Write32(vtable_address + static_cast<uint32_t>(i) * 4, sentinel);
  }
  memory.Write32(object_address, vtable_address);
  return object_address;
}

}  // namespace zeebulator

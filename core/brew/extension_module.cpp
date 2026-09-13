#include "core/brew/extension_module.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "core/loader/mif.h"
#include "core/loader/mod.h"

namespace zeebulator {

namespace {

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streampos end = in.tellg();
  if (end < 0) return false;
  in.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(end));
  if (!out->empty()) in.read(reinterpret_cast<char*>(out->data()), end);
  return static_cast<bool>(in);
}

bool LogEnabled() { return std::getenv("ZEEB_LOG_EXTENSION") != nullptr; }

}  // namespace

std::vector<BrewExtensionEntry> ScanBrewExtensionCatalog(const std::string& nand_root) {
  namespace fs = std::filesystem;
  std::vector<BrewExtensionEntry> out;
  std::error_code ec;
  const fs::path mif_dir = fs::path(nand_root) / "mif";
  const fs::path mod_dir = fs::path(nand_root) / "mod";
  if (!fs::is_directory(mif_dir, ec)) return out;

  fs::directory_iterator it(mif_dir, ec);
  const fs::directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code file_ec;
    if (!it->is_regular_file(file_ec) || file_ec) continue;
    const fs::path mif_path = it->path();
    std::string ext = mif_path.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".mif") continue;

    std::vector<uint8_t> data;
    if (!ReadWholeFile(mif_path.string(), &data)) continue;
    const std::vector<uint32_t> provided =
        ExtractMifExtensionClassIds(data.data(), data.size());
    if (provided.empty()) continue;

    // O .mod da extensao mora em <nand>/mod/<mesmo numero do .mif>/. Nao ha
    // regra de nome para o arquivo em si (o do 12875 chama-se imicro3d.mod),
    // entao pega-se o unico .mod da pasta.
    const fs::path folder = mod_dir / mif_path.stem();
    std::string mod_path;
    std::error_code dir_ec;
    fs::directory_iterator mit(folder, dir_ec);
    for (; !dir_ec && mit != end; mit.increment(dir_ec)) {
      std::string mext = mit->path().extension().string();
      for (char& c : mext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (mext == ".mod") { mod_path = mit->path().string(); break; }
    }
    if (mod_path.empty()) continue;

    for (uint32_t cls : provided) {
      out.push_back(BrewExtensionEntry{cls, mif_path.string(), mod_path});
    }
  }
  return out;
}

namespace {

// Le um .mif, e se ele for de extensao devolve as entradas (classe -> .mod da
// pasta `folder`). Vazio em qualquer outro caso.
std::vector<BrewExtensionEntry> EntriesFor(const std::filesystem::path& mif_path,
                                            const std::filesystem::path& folder) {
  namespace fs = std::filesystem;
  std::vector<BrewExtensionEntry> out;
  std::vector<uint8_t> data;
  if (!ReadWholeFile(mif_path.string(), &data)) return out;
  const std::vector<uint32_t> provided =
      ExtractMifExtensionClassIds(data.data(), data.size());
  if (provided.empty()) return out;

  // Nao ha regra de nome para o .mod em si (12875 traz imicro3d.mod; o pacote do
  // Kingdom Hearts traz swv21brew.mod), entao pega-se o unico .mod da pasta.
  std::string mod_path;
  std::error_code ec;
  fs::directory_iterator it(folder, ec);
  const fs::directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    std::string ext = it->path().extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".mod") { mod_path = it->path().string(); break; }
  }
  if (mod_path.empty()) return out;
  for (uint32_t cls : provided) {
    out.push_back(BrewExtensionEntry{cls, mif_path.string(), mod_path});
  }
  return out;
}

}  // namespace

std::vector<BrewExtensionEntry> ScanBrewExtensionsForModule(const std::string& game_mod_path) {
  namespace fs = std::filesystem;
  std::vector<BrewExtensionEntry> out;
  std::error_code ec;
  const fs::path mod_abs = fs::absolute(fs::path(game_mod_path), ec);
  if (ec || !mod_abs.has_parent_path()) return out;
  const fs::path own_dir = mod_abs.parent_path();
  if (!own_dir.has_parent_path()) return out;
  const fs::path parent = own_dir.parent_path();

  fs::directory_iterator it(parent, ec);
  const fs::directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code dir_ec;
    if (!it->is_directory(dir_ec) || dir_ec) continue;
    const fs::path folder = it->path();
    const std::string name = folder.filename().string();
    // Os dois lugares medidos onde o .mif de uma pasta de modulo pode estar.
    const fs::path candidates[] = {
        parent / (name + ".mif"),
        parent.has_parent_path() ? parent.parent_path() / "mif" / (name + ".mif")
                                 : fs::path(),
    };
    for (const fs::path& cand : candidates) {
      if (cand.empty()) continue;
      std::error_code exists_ec;
      if (!fs::is_regular_file(cand, exists_ec) || exists_ec) continue;
      for (BrewExtensionEntry& e : EntriesFor(cand, folder)) out.push_back(e);
      break;
    }
  }
  return out;
}

BrewExtensionLoader::BrewExtensionLoader(IArmCore& cpu, HleRuntime& hle)
    : cpu_(cpu), hle_(hle) {}

void BrewExtensionLoader::AddEntries(const std::vector<BrewExtensionEntry>& entries) {
  for (const BrewExtensionEntry& e : entries) {
    if (e.cls_id == 0 || e.mod_path.empty()) continue;
    providers_.emplace(e.cls_id, e.mod_path);
  }
}

namespace {

void LogEntries(const std::vector<BrewExtensionEntry>& entries) {
  if (!LogEnabled()) return;
  for (const BrewExtensionEntry& e : entries) {
    std::fprintf(stderr, "[extension] %s fornece 0x%08x -> %s\n", e.mif_path.c_str(), e.cls_id,
                 e.mod_path.c_str());
  }
}

}  // namespace

size_t BrewExtensionLoader::ScanNandRoot(const std::string& nand_root) {
  const std::vector<BrewExtensionEntry> entries = ScanBrewExtensionCatalog(nand_root);
  const size_t before = providers_.size();
  AddEntries(entries);
  LogEntries(entries);
  return providers_.size() - before;
}

size_t BrewExtensionLoader::ScanForModule(const std::string& game_mod_path) {
  const std::vector<BrewExtensionEntry> entries = ScanBrewExtensionsForModule(game_mod_path);
  const size_t before = providers_.size();
  AddEntries(entries);
  LogEntries(entries);
  return providers_.size() - before;
}

const BrewExtensionLoader::LoadedModule* BrewExtensionLoader::EnsureLoaded(
    const std::string& mod_path) {
  auto cached = loaded_.find(mod_path);
  if (cached != loaded_.end()) return &cached->second;
  if (load_failed_.count(mod_path)) return nullptr;

  std::vector<uint8_t> image;
  if (!ReadWholeFile(mod_path, &image) || image.empty()) {
    std::fprintf(stderr, "[extension] nao consegui ler %s\n", mod_path.c_str());
    load_failed_[mod_path] = true;
    return nullptr;
  }
  if (image.size() >= kExtensionModuleStride) {
    // Honesto em vez de silencioso: a base seguinte colidiria com este modulo.
    std::fprintf(stderr,
                 "[extension] %s tem %zu bytes, maior que o passo de %u entre bases -- "
                 "recusado (aumente kExtensionModuleStride)\n",
                 mod_path.c_str(), image.size(), kExtensionModuleStride);
    load_failed_[mod_path] = true;
    return nullptr;
  }

  const uint32_t base = next_base_;
  // LoadMod tambem aponta o PC para a base (contrato dele, ver core/loader/mod.h).
  // Aqui ele roda DENTRO de um handler de HLE, com o convidado parado no meio de
  // uma chamada, entao o PC corrente e restaurado na hora -- a entrada no
  // convidado acontece so depois, e so via CallArmFunctionPreservingContext.
  const uint32_t saved_pc = cpu_.GetRegister(kPC);
  LoadMod(cpu_, image, base);
  cpu_.SetRegister(kPC, saved_pc);
  // ROPI: o codigo compilado le a tabela de helpers da stdlib em (base - 4),
  // exatamente como o modulo principal (core/brew/mod_runtime.h). A tabela e a
  // MESMA -- sao funcoes da plataforma, nao do modulo.
  cpu_.GetMemory().Write32(base - 4, static_base_table_);
  cpu_.NotifyCodeChanged(base, static_cast<uint32_t>(image.size()));

  // AEEMod_Load(IShell *ps, void *ph, IModule **ppMod) fica no offset 0 do .mod
  // (exigencia real do BREW, ja validada por este projeto para o modulo
  // principal). A chamada preserva o contexto porque estamos, quase sempre,
  // dentro do CreateInstance do proprio jogo.
  const uint32_t pp_mod = kExtensionScratchBase;
  cpu_.GetMemory().Write32(pp_mod, 0);
  uint32_t load_result = 0;
  try {
    load_result = hle_.CallArmFunctionPreservingContext(base, shell_ptr_, 0, pp_mod, 0);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[extension] AEEMod_Load de %s lancou: %s\n", mod_path.c_str(),
                 e.what());
    load_failed_[mod_path] = true;
    return nullptr;
  }
  const uint32_t module_ptr = cpu_.GetMemory().Read32(pp_mod);
  if (module_ptr == 0) {
    std::fprintf(stderr,
                 "[extension] AEEMod_Load de %s devolveu %u sem ponteiro de modulo\n",
                 mod_path.c_str(), load_result);
    load_failed_[mod_path] = true;
    return nullptr;
  }
  if (LogEnabled()) {
    std::fprintf(stderr, "[extension] %s carregado em 0x%08x (%zu bytes), modulo=0x%08x\n",
                 mod_path.c_str(), base, image.size(), module_ptr);
  }
  next_base_ += kExtensionModuleStride;
  LoadedModule m;
  m.base = base;
  m.size = static_cast<uint32_t>(image.size());
  m.module_ptr = module_ptr;
  auto inserted = loaded_.emplace(mod_path, m);
  return &inserted.first->second;
}

std::vector<std::pair<uint32_t, uint32_t>> BrewExtensionLoader::LoadedRanges() const {
  std::vector<std::pair<uint32_t, uint32_t>> out;
  out.reserve(loaded_.size());
  for (const auto& [path, m] : loaded_) {
    (void)path;
    out.emplace_back(m.base, m.size);
  }
  return out;
}

uint32_t BrewExtensionLoader::CreateInstance(uint32_t cls_id) {
  auto provider = providers_.find(cls_id);
  if (provider == providers_.end()) return 0;
  const LoadedModule* module_info = EnsureLoaded(provider->second);
  if (module_info == nullptr) return 0;

  // IModule::CreateInstance e o slot 2 da vtable (AddRef=0, Release=1,
  // CreateInstance=2), a mesma ordem ja usada para o modulo principal.
  Memory& mem = cpu_.GetMemory();
  const uint32_t vtable = mem.Read32(module_info->module_ptr);
  const uint32_t create_instance_fn = mem.Read32(vtable + 2 * 4);
  if (create_instance_fn == 0) {
    std::fprintf(stderr, "[extension] modulo 0x%08x sem IModule::CreateInstance\n",
                 module_info->module_ptr);
    return 0;
  }
  const uint32_t pp_obj = kExtensionScratchBase + 4;
  mem.Write32(pp_obj, 0);
  uint32_t status = 0;
  try {
    status = hle_.CallArmFunctionPreservingContext(create_instance_fn, module_info->module_ptr,
                                                    shell_ptr_, cls_id, pp_obj);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[extension] CreateInstance(0x%08x) lancou: %s\n", cls_id, e.what());
    return 0;
  }
  const uint32_t obj = mem.Read32(pp_obj);
  if (status != 0 || obj == 0) {
    // Falha honesta: o modulo existe mas recusou a classe. Serve tambem como a
    // verificacao semantica que a regra do .mif sozinha nao da (ver mif.h).
    std::fprintf(stderr, "[extension] %s recusou 0x%08x (status=%u, obj=0x%08x)\n",
                 provider->second.c_str(), cls_id, status, obj);
    return 0;
  }
  if (LogEnabled()) {
    std::fprintf(stderr, "[extension] 0x%08x -> objeto 0x%08x (vtable 0x%08x)\n", cls_id, obj,
                 mem.Read32(obj));
  }
  return obj;
}

}  // namespace zeebulator

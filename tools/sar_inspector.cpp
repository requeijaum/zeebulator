// Ferramenta de desenvolvimento: lista (e opcionalmente extrai) o conteudo de
// um container SAR ("SWVARC") real -- os assets do chessbots. Recebe o caminho
// em tempo de execucao: nenhum byte de jogo entra no repositorio. O formato
// esta descrito em core/loader/sar.h.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/loader/sar.h"

namespace {

// Assinatura reconhecida no inicio do payload: e a checagem que prova que a
// extracao caiu no offset certo, e nao "quase certo".
const char* Signature(const std::vector<uint8_t>& p) {
  if (p.size() >= 8 && std::memcmp(p.data(), "\x89PNG\r\n\x1a\n", 8) == 0) return "PNG";
  if (p.size() >= 12 && std::memcmp(p.data(), "\xabJSR184\xbb\r\n\x1a\n", 12) == 0) {
    return "M3G/JSR184";
  }
  if (p.size() >= 12 && std::memcmp(p.data(), "\xabSWVARC\xbb\r\n\x1a\n", 12) == 0) {
    return "SAR aninhado";
  }
  if (p.size() >= 4 && std::memcmp(p.data(), "cmid", 4) == 0) return "CMX (cmid)";
  if (p.size() >= 4 && std::memcmp(p.data(), "MThd", 4) == 0) return "MIDI";
  if (p.size() >= 12 && std::memcmp(p.data(), "RIFF", 4) == 0 &&
      std::memcmp(p.data() + 8, "QLCM", 4) == 0) {
    return "QCP (RIFF/QLCM)";
  }
  if (p.size() >= 4 && std::memcmp(p.data(), "RIFF", 4) == 0) return "RIFF";
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <arquivo.sar> [--extract <out_dir>]\n", argv[0]);
    return 1;
  }

  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "error: nao consegui abrir '%s'\n", argv[1]);
    return 1;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  const size_t file_size = data.size();

  // Parse devolve optional e nao lanca: um arquivo que nao e um SAR sai por
  // aqui com mensagem, nunca por excecao.
  auto archive = zeebulator::SarArchive::Parse(std::move(data));
  if (!archive) {
    std::fprintf(stderr, "error: '%s' nao e um SAR valido (%zu bytes)\n", argv[1], file_size);
    return 1;
  }

  const bool extract = argc >= 4 && std::string(argv[2]) == "--extract";
  const std::string out_dir = extract ? argv[3] : "";

  std::printf("%s: %zu bytes, %zu entradas, timestamp=0x%08x hash=0x%08x\n", argv[1], file_size,
              archive->Entries().size(), archive->Timestamp(), archive->HeaderHash());

  uint64_t covered = archive->Entries().empty() ? 0 : archive->Entries()[0].payload_offset;
  std::printf("  indice ocupa %llu bytes ate o inicio dos payloads\n",
              static_cast<unsigned long long>(covered));
  size_t failed = 0;
  for (const auto& entry : archive->Entries()) {
    auto blob = archive->Extract(entry);
    if (!blob) {
      ++failed;
      std::printf("  %-32s FALHOU\n", entry.name.c_str());
      continue;
    }
    std::printf("  %-32s offset=%-9u size=%-9u name_hash=%08x content_hash=%08x %s\n",
                entry.name.c_str(), entry.payload_offset, entry.payload_size, entry.name_hash,
                entry.content_hash, Signature(*blob));
    if (entry.payload_offset != covered) {
      std::printf("    AVISO: payload nao e contiguo (esperado offset %llu)\n",
                  static_cast<unsigned long long>(covered));
    }
    covered += entry.payload_size;
    if (extract) {
      std::ofstream out(out_dir + "/" + entry.name, std::ios::binary);
      out.write(reinterpret_cast<const char*>(blob->data()),
                static_cast<std::streamsize>(blob->size()));
    }
  }
  std::printf("%llu de %zu bytes cobertos pelo indice + payloads%s\n",
              static_cast<unsigned long long>(covered), file_size,
              covered == file_size ? " (arquivo consumido por completo)" : " (SOBRA!)");
  return failed == 0 ? 0 : 1;
}

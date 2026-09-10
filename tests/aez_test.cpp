#include "core/loader/aez.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

std::vector<uint8_t> BuildEntry(const std::string& name, const std::vector<uint8_t>& payload,
                                uint32_t decompressed, uint32_t compressed_field) {
  std::vector<uint8_t> out;
  out.push_back(static_cast<uint8_t>(name.size()));
  out.insert(out.end(), name.begin(), name.end());
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(decompressed >> (i * 8)));
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(compressed_field >> (i * 8)));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

}  // namespace

// Uma entrada armazenada crua (marca 0xFFFFFFFF) e lida de volta intacta.
// Esta e exatamente a regra que faltava no primeiro levantamento do formato:
// sem ela a leitura de res.aez parava na 19a entrada de 182.
TEST(AezArchiveTest, StoredEntryRoundTrip) {
  const std::vector<uint8_t> payload = {'A', 'E', 'M', 'e', 's', 'h', 0x00, 0x17};
  auto raw = BuildEntry("/data/meshes/x.aem", payload,
                        static_cast<uint32_t>(payload.size()), 0xFFFFFFFFu);
  auto archive = zeebulator::AezArchive::Parse(raw);
  ASSERT_TRUE(archive.has_value());
  ASSERT_EQ(archive->Entries().size(), 1u);
  ASSERT_EQ(archive->Entries()[0].name, std::string("/data/meshes/x.aem"));
  ASSERT_TRUE(archive->Entries()[0].stored);
  auto data = archive->Extract(archive->Entries()[0]);
  ASSERT_TRUE(data.has_value());
  ASSERT_EQ(data->size(), payload.size());
  ASSERT_EQ((*data)[0], 'A');
}

// CONTROLE NEGATIVO: bytes aleatorios nao podem virar um arquivo valido.
TEST(AezArchiveTest, RejectsRandomBytes) {
  std::vector<uint8_t> junk;
  uint32_t state = 12345;
  for (int i = 0; i < 4096; ++i) {
    state = state * 1103515245u + 12345u;
    junk.push_back(static_cast<uint8_t>(state >> 16));
  }
  ASSERT_FALSE(zeebulator::AezArchive::Parse(junk).has_value());
}

// CONTROLE NEGATIVO: um arquivo truncado no meio do payload deve ser recusado,
// nao lido fora dos limites.
TEST(AezArchiveTest, RejectsTruncatedPayload) {
  const std::vector<uint8_t> payload(64, 0xAB);
  auto raw = BuildEntry("/a.bin", payload, 64, 64);
  raw.resize(raw.size() - 8);  // corta o fim do payload
  ASSERT_FALSE(zeebulator::AezArchive::Parse(raw).has_value());
}

// CONTROLE NEGATIVO: nome com bytes nao imprimiveis nao e um AEZ.
TEST(AezArchiveTest, RejectsBinaryName) {
  std::vector<uint8_t> raw;
  raw.push_back(4);
  raw.insert(raw.end(), {0x00, 0x01, 0x02, 0x03});
  for (int i = 0; i < 8; ++i) raw.push_back(0);
  ASSERT_FALSE(zeebulator::AezArchive::Parse(raw).has_value());
}

// Sobra de bytes no fim significa geometria errada -- um AEZ valido termina
// exatamente no fim do arquivo.
TEST(AezArchiveTest, RejectsTrailingGarbage) {
  const std::vector<uint8_t> payload(16, 0x5A);
  auto raw = BuildEntry("/b.bin", payload, 16, 0xFFFFFFFFu);
  raw.push_back(0x99);
  ASSERT_FALSE(zeebulator::AezArchive::Parse(raw).has_value());
}

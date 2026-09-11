#pragma once

#include <cstdint>
#include <cstdio>

namespace zeebulator {

// Contador central de desenho.
//
// Existe porque o emulador media "chegou ao event loop" e tratava isso como
// sucesso. Esse sinal nao distingue jogo rodando de jogo parado: o cluster
// IDLE inteiro alcancava o event loop com a thread principal estacionada e
// zero pixels desenhados. Sem contar draw call nao da para responder a
// pergunta que importa -- o titulo nao desenha, ou desenha e nos perdemos?
//
// Observacao pura: nenhum contador altera comportamento. Impressao fica
// atras de ZEEB_DRAW_STATS para nao poluir o default.
struct DrawStats {
  uint64_t gl_draw_arrays = 0;
  uint64_t gl_clear = 0;
  uint64_t gl_swap = 0;
  uint64_t gl_tex_image = 0;
  uint64_t disp_bitblt = 0;
  uint64_t disp_draw_text = 0;
  uint64_t disp_draw_rect = 0;
  uint64_t disp_update = 0;

  static DrawStats& Instance() {
    static DrawStats s;
    return s;
  }

  uint64_t TotalDraws() const {
    return gl_draw_arrays + disp_bitblt + disp_draw_text + disp_draw_rect;
  }

  void Print(const char* tag) const {
    std::printf("[draw] %s gl{draw=%llu clear=%llu swap=%llu tex=%llu} "
                "disp{blt=%llu text=%llu rect=%llu upd=%llu} total_draws=%llu\n",
                tag,
                static_cast<unsigned long long>(gl_draw_arrays),
                static_cast<unsigned long long>(gl_clear),
                static_cast<unsigned long long>(gl_swap),
                static_cast<unsigned long long>(gl_tex_image),
                static_cast<unsigned long long>(disp_bitblt),
                static_cast<unsigned long long>(disp_draw_text),
                static_cast<unsigned long long>(disp_draw_rect),
                static_cast<unsigned long long>(disp_update),
                static_cast<unsigned long long>(TotalDraws()));
  }
};

}  // namespace zeebulator

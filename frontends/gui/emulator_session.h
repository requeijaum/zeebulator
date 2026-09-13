#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

#include "frontends/gui/game_library.h"

namespace zeebulator::gui {

enum class SessionState {
  kIdle,
  kRunning,
  kPaused,
};

// Ciclo de vida da emulacao, sem nenhuma dependencia de UI.
//
// Existe separado da janela por dois motivos: e a parte que pode deixar processo
// ORFAO se estiver errada (requisito RF-6), e e a parte que a Fase 2 vai trocar
// quando a emulacao rodar no mesmo processo -- a interface nao deve mudar quando
// isso acontecer.
class EmulatorSession {
 public:
  EmulatorSession() = default;
  ~EmulatorSession();
  EmulatorSession(const EmulatorSession&) = delete;
  EmulatorSession& operator=(const EmulatorSession&) = delete;

  // Inicia um titulo. Devolve false e nao muda de estado quando a entrada nao e
  // iniciavel, quando ja existe sessao, ou quando a linha de comando sai vazia.
  bool Start(const GameEntry& entry, const std::string& emulator_binary);

  // Mesma coisa, com a linha de comando pronta. Existe para poder testar o ciclo
  // de vida com um processo de verdade (/bin/sleep) sem depender do emulador.
  // `env` e aplicado no processo filho antes do exec. Existe porque o
  // emulador recebe orcamento de passos e chaves de bisseccao por variavel de
  // ambiente, e sem isto a GUI sobe todo jogo com o mesmo ambiente -- o que faz
  // titulos como o cnk2 serem dados como mortos (ver GameConfig).
  bool StartCommand(const std::vector<std::string>& argv, const std::string& title,
                    const std::map<std::string, std::string>& env = {});

  // Colhe o filho se ele terminou. Precisa ser chamado periodicamente pela UI;
  // sem isso o processo vira zumbi.
  void Poll();

  // Para a sessao: SIGTERM, espera curta, e SIGKILL se ainda estiver de pe.
  // Devolve true se a sessao existia.
  bool Stop();

  // Alterna pausa. Usa SIGSTOP/SIGCONT, que param e retomam de verdade o
  // processo -- pausar so na interface deixaria o emulador consumindo CPU.
  bool TogglePause();

  SessionState state() const { return state_; }
  pid_t pid() const { return pid_; }
  const std::string& title() const { return title_; }
  double elapsed_seconds() const;
  const std::string& last_error() const { return last_error_; }

  // Texto do estado para a UI. Fica aqui e nao na janela para o teste poder
  // verificar que o estado nunca aparece vazio nem contraditorio.
  std::string StatusLine() const;

 private:
  void Reset();

  pid_t pid_ = -1;
  SessionState state_ = SessionState::kIdle;
  std::string title_;
  std::string last_error_;
  double started_at_ = 0.0;   // segundos desde o inicio do processo
  double stopped_at_ = 0.0;
};

}  // namespace zeebulator::gui

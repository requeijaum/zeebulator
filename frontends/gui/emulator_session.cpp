#include "frontends/gui/emulator_session.h"

#include <map>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace zeebulator::gui {

namespace {

double NowSeconds() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Espera o filho terminar por ate `timeout_ms`. Devolve true se ele saiu.
bool WaitForExit(pid_t pid, int timeout_ms) {
  const int step_ms = 10;
  for (int waited = 0; waited < timeout_ms; waited += step_ms) {
    int status = 0;
    const pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) return true;
    if (done == -1 && errno == ECHILD) return true;  // ja colhido
    usleep(step_ms * 1000);
  }
  return false;
}

}  // namespace

EmulatorSession::~EmulatorSession() {
  // Garantia de que fechar a janela nao deixa emulador rodando: e o requisito
  // RF-6, e um destrutor que so confiasse no usuario clicar em "Parar" nao o
  // cumpriria.
  if (pid_ > 0) Stop();
}

bool EmulatorSession::Start(const GameEntry& entry, const std::string& emulator_binary) {
  last_error_.clear();
  if (state_ != SessionState::kIdle) {
    last_error_ = "ja existe uma sessao; pare antes de iniciar outra";
    return false;
  }
  if (!entry.launchable) {
    last_error_ = "titulo nao iniciavel: " + entry.status_reason;
    return false;
  }
  // BuildLaunchSpec, e nao BuildLaunchArgs: o ambiente por jogo (orcamento de
  // passos, flags ZEEB_*) faz parte do lancamento. Ver GameConfig.
  const LaunchSpec spec = BuildLaunchSpec(entry, emulator_binary);
  const std::vector<std::string>& argv = spec.argv;
  if (argv.empty()) {
    last_error_ = "nao consegui montar a linha de comando deste titulo";
    return false;
  }
  return StartCommand(argv, entry.name, spec.env);
}

bool EmulatorSession::StartCommand(const std::vector<std::string>& argv,
                                   const std::string& title,
                                   const std::map<std::string, std::string>& env) {
  if (argv.empty() || argv[0].empty()) {
    last_error_ = "linha de comando vazia";
    return false;
  }
  if (state_ != SessionState::kIdle) {
    last_error_ = "ja existe uma sessao; pare antes de iniciar outra";
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    last_error_ = std::string("fork falhou: ") + std::strerror(errno);
    return false;
  }
  if (pid == 0) {
    // Filho: monta argv e exec. Nao retorna em caso de sucesso.
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const std::string& a : argv) raw.push_back(const_cast<char*>(a.c_str()));
    raw.push_back(nullptr);
    // Ambiente por jogo, no filho. setenv ANTES do exec, e nao execve com um
    // environ montado a mao: assim o filho herda o ambiente do usuario (DISPLAY,
    // XDG_*, ZEEB_DATA_DIR) e so acrescenta o que e especifico do titulo.
    for (const auto& kv : env) {
      if (!kv.first.empty()) setenv(kv.first.c_str(), kv.second.c_str(), /*overwrite=*/1);
    }
    execv(raw[0], raw.data());
    std::fprintf(stderr, "nao consegui executar '%s': %s\n", raw[0], std::strerror(errno));
    _exit(127);
  }
  pid_ = pid;
  state_ = SessionState::kRunning;
  title_ = title;
  started_at_ = NowSeconds();
  stopped_at_ = 0.0;
  return true;
}

void EmulatorSession::Poll() {
  if (pid_ <= 0) return;
  int status = 0;
  const pid_t done = waitpid(pid_, &status, WNOHANG);
  if (done == pid_) {
    const bool ok = (status == 0) || (WIFEXITED(status) && WEXITSTATUS(status) == 0);
    last_error_ = ok ? "" : "a sessao terminou com erro";
    Reset();
  }
}

bool EmulatorSession::Stop() {
  if (pid_ <= 0) return false;
  const pid_t pid = pid_;
  // Um processo pausado com SIGSTOP nao trata SIGTERM ate ser retomado: mandar
  // SIGCONT antes do SIGTERM evita depender do SIGKILL e do timeout.
  if (state_ == SessionState::kPaused) kill(pid, SIGCONT);
  kill(pid, SIGTERM);
  if (!WaitForExit(pid, 1500)) {
    kill(pid, SIGKILL);
    WaitForExit(pid, 1000);
  }
  Reset();
  last_error_ = "";
  return true;
}

bool EmulatorSession::TogglePause() {
  if (pid_ <= 0) return false;
  if (state_ == SessionState::kRunning) {
    if (kill(pid_, SIGSTOP) != 0) {
      last_error_ = std::string("nao consegui pausar: ") + std::strerror(errno);
      return false;
    }
    state_ = SessionState::kPaused;
    return true;
  }
  if (state_ == SessionState::kPaused) {
    if (kill(pid_, SIGCONT) != 0) {
      last_error_ = std::string("nao consegui retomar: ") + std::strerror(errno);
      return false;
    }
    state_ = SessionState::kRunning;
    return true;
  }
  return false;
}

double EmulatorSession::elapsed_seconds() const {
  if (pid_ <= 0) return 0.0;
  // Enquanto pausado o tempo fica congelado: o relogio do usuario mede emulacao,
  // nao tempo de parede, e um contador que anda com o jogo parado seria mentira.
  if (state_ == SessionState::kPaused) return stopped_at_ - started_at_;
  return NowSeconds() - started_at_;
}

std::string EmulatorSession::StatusLine() const {
  char buf[256];
  switch (state_) {
    case SessionState::kIdle:
      if (last_error_.empty()) return "Nenhuma sessao em execucao";
      return "Nenhuma sessao em execucao (ultimo aviso: " + last_error_ + ")";
    case SessionState::kRunning:
    case SessionState::kPaused: {
      const double e = elapsed_seconds();
      const int total = static_cast<int>(e);
      std::snprintf(buf, sizeof(buf), "%s: %s (pid %d) %02d:%02d", 
                    state_ == SessionState::kPaused ? "Pausado" : "Rodando",
                    title_.c_str(), static_cast<int>(pid_), total / 60, total % 60);
      return buf;
    }
  }
  return "estado desconhecido";
}

void EmulatorSession::Reset() {
  pid_ = -1;
  state_ = SessionState::kIdle;
  title_.clear();
}

}  // namespace zeebulator::gui

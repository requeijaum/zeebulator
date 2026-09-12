#include "frontends/gui/emulator_session.h"

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
  const std::vector<std::string> argv = BuildLaunchArgs(entry, emulator_binary);
  if (argv.empty()) {
    last_error_ = "nao consegui montar a linha de comando deste titulo";
    return false;
  }
  return StartCommand(argv, entry.name);
}

bool EmulatorSession::StartCommand(const std::vector<std::string>& argv,
                                   const std::string& title) {
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

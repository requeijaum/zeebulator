#include "frontends/gui/emulator_session.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

using namespace zeebulator::gui;

namespace {

// Um "emulador" de mentira para exercitar o ciclo de vida sem depender do
// emulador de verdade nem abrir janela: /bin/sleep tem exatamente as
// propriedades que importam aqui -- processo de longa duracao, que responde a
// SIGTERM e que aceita SIGSTOP/SIGCONT.
std::vector<std::string> SleepCommand(const std::string& seconds) {
  return {"/bin/sleep", seconds};
}

bool ProcessExists(pid_t pid) {
  return pid > 0 && kill(pid, 0) == 0;
}

void WaitABit(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

TEST(EmulatorSession, StartsIdle) {
  EmulatorSession s;
  EXPECT_EQ(s.state(), SessionState::kIdle);
  EXPECT_EQ(s.pid(), -1);
  EXPECT_EQ(s.elapsed_seconds(), 0.0);
  // O estado nunca pode aparecer vazio na UI, nem em repouso.
  EXPECT_FALSE(s.StatusLine().empty());
}

TEST(EmulatorSession, RefusesATitleThatIsNotLaunchable) {
  EmulatorSession s;
  GameEntry e;
  e.mod_path = "/x/y.mod";
  e.launchable = false;
  e.status_reason = "ClsId desconhecido";
  EXPECT_FALSE(s.Start(e, "/bin/true"));
  EXPECT_EQ(s.state(), SessionState::kIdle);
  // A recusa precisa dizer POR QUE: "nao iniciavel" mudo foi o defeito que
  // escondeu 11 titulos dados como mortos por ClsId errado.
  EXPECT_NE(s.last_error().find("ClsId desconhecido"), std::string::npos);
  EXPECT_NE(s.StatusLine().find("ClsId desconhecido"), std::string::npos);
}

TEST(EmulatorSession, StartsStopsAndLeavesNoProcessBehind) {
  EmulatorSession s;
  ASSERT_TRUE(s.StartCommand(SleepCommand("30"), "teste"));
  EXPECT_EQ(s.state(), SessionState::kRunning);
  const pid_t pid = s.pid();
  ASSERT_GT(pid, 0);
  EXPECT_TRUE(ProcessExists(pid));
  EXPECT_NE(s.StatusLine().find("Rodando"), std::string::npos);

  EXPECT_TRUE(s.Stop());
  EXPECT_EQ(s.state(), SessionState::kIdle);
  // Requisito RF-6: nao pode sobrar processo. Conferido por sinal 0 no pid.
  EXPECT_FALSE(ProcessExists(pid));
}

TEST(EmulatorSession, DestructorStopsARunningSession) {
  pid_t pid = -1;
  {
    EmulatorSession s;
    ASSERT_TRUE(s.StartCommand(SleepCommand("30"), "teste"));
    pid = s.pid();
    ASSERT_TRUE(ProcessExists(pid));
  }  // fechar a janela cai aqui
  EXPECT_FALSE(ProcessExists(pid));
}

TEST(EmulatorSession, PauseAndResumeReallySignalTheProcess) {
  EmulatorSession s;
  ASSERT_TRUE(s.StartCommand(SleepCommand("30"), "teste"));
  const pid_t pid = s.pid();
  ASSERT_TRUE(s.TogglePause());
  EXPECT_EQ(s.state(), SessionState::kPaused);
  EXPECT_NE(s.StatusLine().find("Pausado"), std::string::npos);
  // O processo continua existindo enquanto pausado: pausar nao e parar.
  EXPECT_TRUE(ProcessExists(pid));
  ASSERT_TRUE(s.TogglePause());
  EXPECT_EQ(s.state(), SessionState::kRunning);
  s.Stop();
}

TEST(EmulatorSession, ElapsedClockFreezesWhilePaused) {
  EmulatorSession s;
  ASSERT_TRUE(s.StartCommand(SleepCommand("30"), "teste"));
  WaitABit(300);
  const double before = s.elapsed_seconds();
  ASSERT_GT(before, 0.0);
  ASSERT_TRUE(s.TogglePause());
  const double at_pause = s.elapsed_seconds();
  WaitABit(400);
  // O relogio tem de ficar parado: um contador que anda com o jogo pausado
  // seria mentira sobre o que o usuario esta vendo.
  EXPECT_DOUBLE_EQ(s.elapsed_seconds(), at_pause);
  s.Stop();
}

TEST(EmulatorSession, PollReapsASessionThatEndedOnItsOwn) {
  EmulatorSession s;
  ASSERT_TRUE(s.StartCommand(SleepCommand("0"), "curto"));
  EXPECT_EQ(s.state(), SessionState::kRunning);
  WaitABit(400);
  s.Poll();
  EXPECT_EQ(s.state(), SessionState::kIdle);
  EXPECT_EQ(s.pid(), -1);
}

TEST(EmulatorSession, RefusesASecondSessionWhileOneIsRunning) {
  EmulatorSession s;
  ASSERT_TRUE(s.StartCommand(SleepCommand("30"), "primeiro"));
  EXPECT_FALSE(s.StartCommand(SleepCommand("30"), "segundo"));
  EXPECT_NE(s.last_error().find("ja existe"), std::string::npos);
  s.Stop();
}

TEST(EmulatorSession, StopIsSafeWhenIdleAndRepeated) {
  EmulatorSession s;
  EXPECT_FALSE(s.Stop());       // nao havia sessao
  EXPECT_FALSE(s.TogglePause());  // e pausar tambem nao faz sentido
  ASSERT_TRUE(s.StartCommand(SleepCommand("30"), "teste"));
  EXPECT_TRUE(s.Stop());
  EXPECT_FALSE(s.Stop());       // segundo Stop nao pode explodir
}

TEST(EmulatorSession, EmptyCommandIsRefusedWithAReason) {
  EmulatorSession s;
  EXPECT_FALSE(s.StartCommand({}, "nada"));
  EXPECT_FALSE(s.last_error().empty());
  EXPECT_EQ(s.state(), SessionState::kIdle);
}

// --- Ambiente por jogo chega mesmo ao processo filho -------------------------
//
// O teste nao olha a intencao do codigo, olha o EFEITO: sobe um filho de
// verdade (via /bin/sh) que ESCREVE no arquivo o que ve no proprio ambiente, e
// le o arquivo. Existe porque a GUI passou a precisar mandar orcamento de
// passos por titulo -- sem isto o cnk2 e dado como morto com "exceeded
// 64000000 steps without returning" -- e uma flag que nao chega ao filho e
// indistinguivel de uma flag que ninguem escreveu.
TEST(EmulatorSession, EnvReachesTheChildProcess) {
  const std::string out = "/tmp/zeeb_env_probe.txt";
  std::remove(out.c_str());
  EmulatorSession s;
  ASSERT_TRUE(s.StartCommand({"/bin/sh", "-c",
                              std::string("printf '%s' \"$ZEEB_MAX_STEPS\" > ") + out},
                             "probe", {{"ZEEB_MAX_STEPS", "186486543"}}));
  for (int i = 0; i < 200 && !std::ifstream(out).good(); ++i) WaitABit(10);
  WaitABit(50);
  std::ifstream in(out);
  std::string valor;
  in >> valor;
  std::remove(out.c_str());
  EXPECT_EQ(valor, "186486543") << "o ambiente por jogo nao chegou ao filho";
}

// O filho NAO pode perder o ambiente do usuario: DISPLAY, XDG_* e ZEEB_DATA_DIR
// vem do processo pai e sao o que faz o emulador achar video e estado.
TEST(EmulatorSession, ChildKeepsTheInheritedEnvironment) {
  setenv("ZEEB_TESTE_PAI", "herdado", 1);
  const std::string out = "/tmp/zeeb_env_herdado.txt";
  std::remove(out.c_str());
  EmulatorSession s;
  ASSERT_TRUE(s.StartCommand({"/bin/sh", "-c",
                              std::string("printf '%s' \"$ZEEB_TESTE_PAI\" > ") + out},
                             "probe"));
  for (int i = 0; i < 200 && !std::ifstream(out).good(); ++i) WaitABit(10);
  WaitABit(50);
  std::ifstream in(out);
  std::string valor;
  in >> valor;
  std::remove(out.c_str());
  unsetenv("ZEEB_TESTE_PAI");
  EXPECT_EQ(valor, "herdado") << "o filho perdeu o ambiente herdado do pai";
}

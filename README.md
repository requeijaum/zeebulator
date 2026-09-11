# Zeebulator

[![Build & Tests](https://img.shields.io/badge/tests-587%2F587%20passing-brightgreen.svg)]()
[![Compatibility](https://img.shields.io/badge/compatibility-96.8%25%20(61%2F63%20titles)-blue.svg)](docs/COMPATIBILIDADE.md)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20macOS-informational.svg)]()
[![License](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

**Zeebulator** é um emulador de alto nível (HLE — *High-Level Emulation*) de código aberto para o console **Zeebo** (Brasil, México e Indonésia, 2009–2011), desenvolvido sobre o chipset **Qualcomm MSM7201A** e o sistema operacional móvel **Qualcomm BREW 4.0.2**.

Diferente de consoles de hardware fixo, o Zeebo opera como uma plataforma de telefonia inteligente conectada executando binários ARM nativos sobre a camada de abstração de componentes (AEE) do BREW e gráficos OpenGL ES 1.0/1.1. O Zeebulator reimplementa nativamente as interfaces C/C++ do BREW e drivers de sistema, **eliminando a necessidade de BIOS ou dumps protegidos por direitos autorais** para execução de jogos.

---

## Estado Atual do Projeto

O Zeebulator atingiu paridade de compatibilidade e arquitetura com o corpus oficial da NAND:

- **61 de 63 títulos oficiais (96,8%)** ultrapassam `AEEMod_Load`, instanciam seu applet via `CreateInstance` e alcançam o laço principal de eventos (`EVT_APP_START` concluído ou laço interativo do shell).
- **26 títulos com laço ativo de jogo comprovado** em execução cooperativa de CPU (40 a 60 quadros/tiques por segundo).
- **Suporte ao Z-Wheel (Shell Oficial)**: o aplicativo de sistema da TecToy (`tectoy.mod`) monta preferências SQLite, decodifica a árvore visual de widgets, carrega os 15 itens do carrossel do catálogo e renderiza a tela completa com o palco superior e o roller inferior.
- **Suporte a Homebrew e Injeções**: Compatibilidade validada com títulos da comunidade OpenZeebo e ports BREW clássicos (ex.: *Kingdom Hearts V-CAST*).
- **587 testes unitários e de integração automatizados** passando com 100% de sucesso (`ctest`).

Consulte o censo autoritativo completo e detalhado em [**`docs/COMPATIBILIDADE.md`**](docs/COMPATIBILIDADE.md).

---

## Arquitetura e Subsistemas

### 1. Núcleo de Execução ARM e Memória
- **Interpretador Otimizado**: Caminhos rápidos de leitura e escrita direta de 16 e 32 bits (`Read32`, `Read16`), atingindo throughput sustentado de **~48 MIPS** em CPU x86_64 moderna.
- **Instruções DSP ARMv5TE**: Suporte nativo a instruções de multiplicação de saturação (ex.: `SMLABB`, `SMLABT`, `SMULBB`).
- **Interworking ARM/Thumb**: Mascaramento estrito de alinhamento de PC de 16 bits (bit 0 para Thumb, bit 1 mascarado).
- **Coprocessador CP15**: Emulação funcional de registradores de controle do ARM11 (leitura de ID de arquitetura e configuração de cache/MMU).
- **Aritmética Soft-Float**: Implementação estrita de operações de ponto flutuante conforme a ABI do ARM BREW.

### 2. Runtime HLE do Qualcomm BREW 4.0.2
- **`IShell`**: Gestão completa de applets, temporizadores (`SetTimer`, `CancelTimer`, `Resume`), agendamento cooperativo de threads (`IThread`), resolução de tipos MIME (`DetectType`, `GetHandler`) e despacho de eventos inter-módulos (`SendEvent`).
- **`IFileMgr` e `IFile`**: Sistema de arquivos virtual (VFS) com normalização POSIX/FAT32, suporte a caminhos relativos de irmãos (`../`), resolução lexical sem distinção de maiúsculas e minúsculas e mapeamento transparente de diretórios.
- **`IDisplay` e `IBitmap`**: Gestão de framebuffer 2D de software, criação de bitmaps compatíveis (`CreateCompatibleBitmap`), `IDIB` com geometria 640×480 RGB565 e clonagem de superfícies.
- **`IGraphics` e `IGLES11` / `QEGL`**: Suporte completo a OpenGL ES 1.1 da Qualcomm, propagação de extensões Qualcomm/Adreno (`GL_AMD_compressed_ATC_texture`, `EGL_QUALCOMM_swap_control`, `eglGetColorBufferQUALCOMM`), armadilhas de `eglGetProcAddress` e retorno estrito `EGL_SUCCESS = 0x3000`.
- **`IHID`**: Integração de controladores com mapeamento do Zeebo Pad (Z-Pad), suporte a botões direcionais, analógicos e disparo de eventos `AEE_EVENT_KEY`.
- **`ISQLMgr` e `ISQLDatabase`**: Camada SQL real suportada pela amalgamação SQLite 3.46.1 integrada, compatível com bancos `tt_prefs.db`, `tt_dlqueue.db` e `asset_cache`.
- **Família de Widgets da Interface**: Implementação completa dos 24 slots do framework de widgets e formulários (acessador de propriedades `0x800`/`0x801`, `IVectorModel`, `OwnerDrawWidget`, `StageWidget` e fontes TrueType).

### 3. Carregadores de Contêineres e Arquivos
- **AEZ (Fishlabs)**: Carregador nativo com suporte a blocos descompactados brutos (`compressed_size == 0xFFFFFFFF`) e blocos zlib/deflate. 100% dos `.aez` do catálogo validados.
- **FUFS (`.vfs`)**: Parser reverso de cabeçalhos de 12 bytes e algoritmo de hash polinomial insensível a maiúsculas (`h = h * 67 + (toupper(c) - 113)`).
- **SAR (`SWVARC`)**: Suporte a contêineres de recursos da Superscape.
- **PAKZ e BAR**: Extração e montagem direta de arquivos de recursos padrão BREW.
- **MIF (`Module Information File`)**: Extrator automático de `ClassID`s a partir da tabela de seções do cabeçalho binário, eliminando incompatibilidades de lançamento.

### 4. Áudio e Multimídia
- **Mixer Estéreo com Resampling em Tempo Real**: Conversão linear de frequências dinâmicas para o dispositivo de saída SDL2.
- **Gravação e Diagnóstico**: Dump de áudio em tempo de execução via `ZEEB_DUMP_AUDIO=saida.wav`.
- **Família Multimídia**: Mapeamento de classes de decodificação para MP3 (`0x01005502`), MIDI (`0x01005501`), QCP (`0x01005503`), PCM (`0x01005511`), ADPCM, AMR, AAC, MMF e PMD.

### 5. Backend de Vídeo e Ferramentas de Inspeção
- **Aceleração OpenGL & Captura FBO**: Apresentação acelerada por hardware via SDL2 e captura de telas 3D via `glReadPixels` invertido em PPM para análise precisa de títulos OpenGL ES.
- **Servidor de Controle NDJSON**: Canal de telemetria e controle por socket TCP para automação de testes, comandos de avanço (`step`), estado (`state`) e captura instantânea (`screenshot`).
- **Espelhamento HTTP e UI de Depuração**: Visualizador web integrado via HTTP (porta padrão `48750`) com visualização de tela, log e estado da CPU.

---

## Compilação

### Requisitos
- **Compilador C++17 ou superior** (`gcc` 9+, `clang` 10+ ou MSVC 2019+)
- **CMake** 3.16 ou superior
- **SDL2** (desenvolvimento)
- **Bibliotecas de sistema**: OpenGL / GLES, zlib, liblzma (`xz`)

#### Ubuntu / Debian
```sh
sudo apt update
sudo apt install build-essential cmake libsdl2-dev libgl1-mesa-dev zlib1g-dev liblzma-dev
```

### Passo a Passo de Compilação

```sh
# Clone o repositório
git clone https://github.com/requeijaum/zeebulator.git
cd zeebulator

# Configure e construa
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Execute os testes automatizados
ctest --test-dir build --output-on-failure -j$(nproc)
```

### Opções de CMake
- `-DZEEBULATOR_BUILD_STANDALONE=ON` — Constrói o frontend standalone interativo SDL2 (`zeebulator_standalone`).
- `-DZEEBULATOR_BUILD_LIBRETRO=ON` — Constrói o núcleo libretro para RetroArch (`zeebulator_libretro.so`).
- `-DZEEBULATOR_BUILD_TESTS=ON` — Constrói a suíte de testes unitários com GoogleTest (`zeebulator_tests`).

---

## Execução e Ferramentas

### 1. Sonda de Execução e Jogos (`zeebulator_game_probe`)
A ferramenta de referência para execução, teste e automação do corpus:

```sh
# Executar um título (auto-descobre arquivos auxiliares .bar, .mif e boot.pkg)
./build/tools/zeebulator_game_probe /caminho/para/jogo.mod

# Execução com controle remoto e captura de áudio
ZEEB_DUMP_AUDIO=partida.wav ZEEB_CONTROL_PORT=48900 ./build/tools/zeebulator_game_probe /caminho/para/jogo.mod
```

### 2. Ferramentas de Inspeção de Arquivos
O projeto acompanha utilitários de engenharia reversa para formatos específicos do Zeebo:
- `zeebulator_mif_inspector` — Inspeciona metadados e extrai ClassIDs de arquivos `.mif`.
- `zeebulator_sar_inspector` — Lista e extrai arquivos de contêineres Superscape `.sar`.
- `zeebulator_bar_inspector` — Inspeciona recursos e imagens de arquivos BREW `.bar`.
- `zeebulator_pakz_inspector` — Inspeciona e valida arquivos compactados `.pakz`.
- `zeebulator_obm1_inspector` — Inspeciona modelos e malhas `.obm1`.

---

## Controles Padrão (Teclado)

| Botão Zeebo Pad (Z-Pad) | Tecla no Computador |
|---|---|
| **D-Pad (Direcionais)** | Setas direcionais / W, A, S, D |
| **Botão 1 (A)** | Tecla `J` / `Enter` |
| **Botão 2 (B)** | Tecla `K` / `Backspace` |
| **Botão 3 (C)** | Tecla `U` |
| **Botão 4 (D)** | Tecla `I` |
| **Gatilho Esquerdo (L)** | Tecla `Q` |
| **Gatilho Direito (R)** | Tecla `E` |
| **Home / Menu** | Tecla `Escape` |

---

## Estrutura do Repositório

```text
zeebulator/
├── core/
│   ├── arm/         # Interpretador ARMv5TE/ARM11, interworking Thumb e CPU core
│   ├── audio/       # Mixer estéreo, reamostragem e pipeline de áudio SDL2
│   ├── brew/        # Runtime HLE (IShell, IFileMgr, IDisplay, SqlHle, etc.)
│   ├── gl/          # Tradução OpenGL ES 1.1 e QEGL para OpenGL desktop
│   ├── loader/      # Parsers para ELF, MOD, AEZ, FUFS (.vfs), SAR, MIF, PAKZ
│   └── memory/      # Gestão de memória do guest, páginas e fast-paths
├── frontends/
│   └── standalone/  # Frontend interativo SDL2 e backend de renderização
├── tools/           # zeebulator_game_probe e inspetores de formato
├── tests/           # Suíte de testes unitários automatizados (GoogleTest)
├── docs/            # Relatórios de compatibilidade e documentação técnica
└── third_party/     # SQLite 3.46.1, Zydis, Zycore, Dynarmic e fmt
```

---

## Licença

Este projeto é distribuído sob os termos da licença **GNU General Public License v3.0 (GPLv3)**. Consulte o arquivo [LICENSE](LICENSE) para mais detalhes.

Marcas registradas, nomes de produtos e referências ao Zeebo, Qualcomm e BREW pertencem aos seus respectivos proprietários e são utilizados apenas para fins de preservação histórica, estudo técnico e interoperabilidade.

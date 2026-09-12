# Zeebulator — Roadmap Técnico de Engenharia & Compatibilidade

Documento vivo e exaustivo de planejamento estratégico, fases de entrega, auditoria formal de conformidade com o BREW SDK e pendências técnicas mapeadas para levar o **Zeebulator** da inicialização básica até a jogabilidade genuína em todo o catálogo comercial do Zeebo.

**Estado verificável desta revisão** — `ctest`: 684 passam, 0 falham, 2 pulados
(dependem de corpus ausente). Último ciclo: `f40379d`, `ea24662`, `bff18be`.
Medição de referência da Z-Wheel (27 s, `tectoy.mod`, ClsId 17237912):
telas 2D de abertura com verde 5 130 / amarelo 1 146 / azul 5 203 px nos
primeiros 8 s, e palco 3D com 149 824 px pretos a partir dos 9 s.
Toda afirmação aqui é acompanhada do número que a sustenta; onde não há
medição, o item fica marcado como pendente em vez de concluído.

---

## 1. Visão Geral das Fases

```
[ Fase 1: Fundação & ABI Core ] (Concluída)
               │
               ▼
[ Fase 2: Robustez de Memória & Parsers ] (Concluída - Auditoria 59336ea)
               │
               ▼
[ Fase 3: Desbloqueio da Z-Wheel & Pipeline Visual 2D/3D ] (Fase Atual)
     telas 2D de abertura renderizando; palco 3D preservado;
     pendencia bloqueante: ordem de canais do ATITC
               │
               ▼
[ Fase 4: Gráficos Avançados & Composição Offscreen ] (Planejada)
               │
               ▼
[ Fase 5: Concorrência, Servidores & Segurança ] (Planejada)
               │
               ▼
[ Fase 6: Expansão do Catálogo & Playability ] (Ongoing / Final)
```

---

## 2. Fase 1: Fundação, ABI BREW & Loader (Concluída)

- [x] Decodificação e execução ARM1136 / Thumb com suporte diferencial (`ArmInterpreter` vs `Dynarmic`).
- [x] Containers base: GGZ, MIF, BAR, PKG, detecção inicial de FUFS (.vfs).
- [x] Contrato canônico de chamada Qualcomm AEE (IShell, IDisplay, IFile, IBitmap).
- [x] Registros NID e tabelas canônicas de identificadores SDK.
- [x] Entrega de eventos síncronos via `IShell_SendEvent` preservando contexto (`CallArmFunctionPreservingContext`).

---

## 3. Fase 2: Robustez de Memória, Limites e Parsers (Concluída — Commit `59336ea`)

Resultado da auditoria integral do código com verificação estrita contra o SDK Qualcomm:
- [x] **Segurança de Memória no Host**:
  - `IFile::Write`: Aritmética de 64-bit e cotas explícitas (64 MiB), prevenindo overflow de inteiros e escrita fora do buffer no host.
  - Vtable `IBitmap`: Alocação completa de 16 slots (64 bytes), impedindo que métodos sobrescrevam o corpo do `IDIB`.
  - Parsers BAR e BRF: Bounds checking estrito contra wraps de sub-tabelas, sentinelas e registros.
  - Desserialização de `Memory`: Limite rígido de páginas (256 MiB máx) e validação de índices de página.
- [x] **ABI & Contratos de Recursos**:
  - `IShell::LoadResDataEx`: Contrato completo implementado (retorno como ponteiro de dados/sentinela `-1`, verificação de capacidade antes de cópia).
  - BRF: Decodificação exata de tipos (strings UTF-16 com BOM LE/BE, blobs binários).
  - Códigos de erro alinhados ao `AEEError.h` (`EBADPARM=14`, `ENOMEMORY=2`, `ECLASSNOTSUPPORT=3`, `EUNSUPPORTED=20`).
- [x] **Reentrância & Lifetimes**:
  - `IThread`: Refcounting canônico implementado, prevenindo double-free de threads e liberando recursos apenas na contagem zero.
  - Streams (`IMemAStream`, `IUnzipAStream`): Cancelamento funcional de callbacks e expurgo no `Release`.
  - Notificações de Mídia: Rastreamento por geração de objeto, impedindo entrega de eventos obsoletos a instâncias reutilizadas.
  - Preservação integral de registradores em chamadas aninhadas (`UnzipStreamHle::Expand`, `ModRuntime::SortPointerArrayImpl`).
- [x] **Isolamento de Persistência**:
  - Remoção de qualquer escrita de `.userdata`, `.savestate` ou `.sqldb` dentro da mídia da ROM.
  - Destino padronizado exclusivamente em caminhos XDG (`~/.local/share/zeebulator/` ou `ZEEB_DATA_DIR`), com importação somente-leitura do legado.
- [x] **Mídia & Áudio**:
  - Rejeição limpa de WAV com `sample_rate == 0`, prevenindo laço infinito no mixer de áudio.
  - Cotas em buffers MMD e descompressão gzip em memória.

---

## 4. Fase 3: Desbloqueio da Z-Wheel & Pipeline Visual (Fase Atual)

Foco: Levar o menu principal da Z-Wheel do quadro branco para a renderização real e interatividade.

- [x] **Separação de Instâncias de Widgets**:
  - Substituição do singleton `kWidgetObject` por factories independentes para as 9 classes visuais da Z-Wheel.
  - Identidade única de ponteiros por instância (base `0x86000000`).
  - Resolução do erro imediato `EUNABLETOLOAD (6)` no formulário do z-pad.
- [x] **Superfícies de Bitmap Compatível**:
  - `CreateCompatibleBitmap` isolado por superfície, com buffer RGB565 e geometria dedicados.
- [x] **Pbuffer EGL**:
  - Suporte inicial a `eglCreatePbufferSurface` respeitando atributos de dimensões (640x330 medido).
  - `egglGetColorBufferQUALCOMM` implementado entregando ponteiro RGB565 cru.
- [x] **Investigação do `SetDrawHandler` (Slot 16) — RESOLVIDO**:
  - Corrigido `snprintf` (slot 0x144 da `AEEHelperFuncs`) que mantinha SQL estático no buffer de query do `PREFSDB_GetRecords`.
  - Implementado fallback entre BRF companheiros (`tectoy_pt.brf` -> `tectoyli.brf` -> `tectoy.brf`), permitindo resolução do recurso 1178 (Z-Pad).
  - Corrigido slot 15 do widget retornando `this` (fábrica de bitmap do `DrawRollerExt`), destravando `CreateCompatibleBitmap` e os callbacks de desenho `SetDrawHandler` (0x14250c) do carrossel/roller!
- [ ] **Distinção de Formas no Slot 16**:
  - Tratar a variante sem struct `slot16(this)` medida em `0x22d58` versus a variante com struct `&{fn, ctx, dtor}` sem interpretar `r1 != 0` arbitrário como ponteiro de função.
- [x] **Readback GL para Pbuffer** (commits `3e53f00`, `9fe12c0`):
  - `GlBackend::ReadPixelsRgba` + FBO dedicado do pbuffer. Uma superfície EGL de
    pbuffer precisa do FBO **dela**: compartilhar o FBO de apresentação criava
    realimentação e o readback devolvia a própria tela (batia 100% com as linhas
    150–480 apresentadas). Medido depois: `readback=ok` 226/0 contra 0/226 antes.
  - Separação de `SwapBuffers()` (só `eglSwapBuffers` real) de
    `PresentGlFrameWithoutSwapMark()`. O latch `HasRealGlActivity` era ligado pelo
    nosso próprio present sintético e desligava para sempre o present 2D de
    software — a tela ia de **2 cores** (branco + contador de FPS) para **732
    cores** com o palco composto em (0,50) 640×330.
- [ ] **Árvore Hierárquica de Widgets & Ciclo de Vida**:
  - Manter relacionamentos pai-filho e geometria relativa de acordo com o slot 5.
  - Validação estrita de ponteiros de geometria no slot 5 (descartar leituras em páginas não mapeadas).
  - Gerenciamento de ciclo de vida completo no slot 17 (fontes `AEECLSID_FONTSYS` e modelos `0x8000`) com AddRef/Release reais.
  - Suporte ao slot 14 (Attach) com retenção de imagem/modelo associado.
- [ ] **Semântica Específica por Classe de Widget**:
  - Classe `0x01028e2a`: definir texto no slot 6.
  - Extent/visibilidade reais e implementação de `GetExtent` para cálculo de layout.
  - Implementação da classe `0x01028e3c`: slots 3 (dois blocos de saída) e 5 (halfword de passo).
  - Slot 8 (`GetParent` vs tocador de animações da Z-Wheel).
- [x] **Despacho Ordenado de Entrada na Interface**:
  - Implementado avanço automático da tela de instruções do Z-Pad via `EVT_KEY` (0x100) com `AVK_0` (0xe030), transitando deterministamente para o carrossel do menu principal.

---

### 4.1 Pipeline de Imagem de Recurso (commits `f40379d`, `ea24662`, `bff18be`)

- [x] **Decodificador BMP real** (`core/loader/bmp.{h,cpp}`, 21 testes):
  - Validado pixel a pixel contra decodificador independente nos **65 BMPs reais**
    do corpus. 16 bpp `BI_RGB` é **555, não 565** (com 555 o erro máximo é 1
    unidade; lendo 565 chega a 132). 32 bpp `BI_RGB` sai opaco porque os 13
    arquivos reais têm o 4º byte **zero** em todos os pixels.
  - `biSizeImage`/`bfSize` ignorados: o recurso 5007 real declara 2 bytes a mais
    que a geometria.
- [x] **Estado fixo do GLES1 deixa de ser stub silencioso** (18 testes):
  - 14 funções implementadas de verdade (`glCullFace`, `glActiveTexture`,
    `glPixelStorei`, `glMaterialxv`, `glLightxv`, `glStencilFunc`, …).
  - **Armadilha do decorador, segunda ocorrência**: `GlTextureRecordingBackend`
    engolia `TexEnvMode` (4135 chamadas de `glTexEnvx` por execução morriam ali),
    exatamente como antes engolia `ReadPixelsRgba`. Regra do projeto: toda
    virtual nova de `GlBackend` exige override que encaminha **e** teste.
  - `glCullFace` alterna `GL_BACK` (147×) e `GL_FRONT` (146×) sem nunca desligar
    `GL_CULL_FACE`: são dois passes por quadro. A/B com `ZEEB_GL_NO_CULLFACE=1`
    muda 24 442 pixels (8%) em x=84..555, y=259..379 — o raio duplo da roda.
- [x] **`ISHELL_LoadResObject` real, um objeto por recurso**:
  - Antes: um objeto falso único para todo recurso, `GetInfo` mentindo 640×480 e
    `Draw` que não desenhava nada. Pior que stub honesto — o jogo recebia
    "sucesso" e seguia.
  - Formato do payload medido nos `.brf` reais: `[u16 header_len][mime NUL][bytes]`,
    com `header_len` contando o próprio u16.
  - Recursos reais que passam a existir: `opening_low.gif` 640×480, PNG 576×313
    (id 5008) e BMP 214×34 (id 5007).
- [x] **Desenho do conteúdo dos ImageWidget**:
  - O jogo entrega a imagem por `IInterfaceModel::SetIPtr` e **nunca** chama
    `IImage::Draw` — num BREW real quem desenha o conteúdo do widget é a
    biblioteca do aparelho, que aqui somos nós. Medido: widget slot 12 =
    `GetModel(AEEIID_IInterfaceModel 0x0101593c)`; no retorno, slot 5 =
    `SetIPtr(pIImage, AEEIID_IImage 0x01013110)`.
  - Resultado nos 8 primeiros segundos: de `cores=1` / 307 200 px brancos para
    `cores=256` com **verde 5 130**, **amarelo 1 146** e **azul 5 203** px
    simultâneos — a assinatura da bandeira.
- [x] **Duas colisões de endereço de objeto HLE** (ambas achadas por medição):
  - Os objetos `IImage` nasciam em `0x8006C000`, que **é** `kWidgetVtable`: o
    primeiro recurso decodificado destruía a vtable de todos os widgets e o
    applet saltava para `0x8006A000` (`bx r1` em `0x17ecf8`).
  - O objeto de fallback e o primeiro recurso nasciam no mesmo endereço, então
    todo recurso que não decodificava devolvia o GIF de abertura: o roller
    pintava esse GIF 640×480 sobre a tela inteira 69× (`lr=0x0011ff28`, dentro de
    `DrawRollerExt`).
  - **Regra do projeto**: antes de atribuir qualquer faixa de vtable/objeto,
    conferir colisão com `grep` no arquivo inteiro. Já é a terceira vez que uma
    faixa reaproveitada produz um sintoma que parece defeito de CPU.

### 4.2 Pendências abertas com evidência medida

- [ ] **Ordem de canais do ATITC** (`core/loader/atitc.cpp`) — *prioridade alta*:
  - As texturas ATITC `512×256` decodificam com **124 905 px** `(239,138,41)`;
    o baseline antigo `zw_shot_exit.ppm` tinha **211 136 px azuis** e zero
    laranja, dominante `(41,142,206)`. É o mesmo pixel com **R e B trocados**.
  - Cadeia inteira já descartada: texturas não comprimidas corretas, readback
    `RGBA→RGB565` correto (`R` vem de `rgba[+0]`), imagens novas corretas (PNG
    casa em RGB com erro **2,28/255**). O defeito está no decode ATITC.
  - **Não corrigido de propósito**: `atitc.cpp` tem testes que fixam a ordem
    atual e o formato é usado por outros títulos. Antes de inverter é preciso
    provar qual ordem é a verdadeira com um decodificador independente ou com a
    arte equivalente não comprimida — senão só se troca o defeito de lugar.
- [ ] **Geometria dos ImageWidget**:
  - Tudo é desenhado em `(0,0)` porque `widget_geometry` não tem entrada para
    esses objetos. Cor e orientação estão certas; o lugar não.
  - Pista medida: as posições chegam por `EVT_WDG_SETPROPERTY` (0x801) com
    wParam `0x152`, `0x153`, `0x130`, `0x140`.
- [ ] **Composição 2D/3D por região suja**:
  - Necessária quando o roller desenhar a arte 214×34 no lugar certo: hoje a
    camada 2D é um quad de tela cheia e apagaria o palco GL.
- [ ] **`AEECLSID_LCT_SIMCARDCTL` (0x01006c01)** sem implementação. O jogo trata
  a falha (retorna 0x27) e segue para o menu — lacuna honesta, não bloqueio.
- [ ] **`class_id = -1` da consulta da roda é do próprio jogo**, não nosso:
  `mvn r2, #0` hardcoded no chamador `0x127c1c`. A consulta principal devolve 0
  linhas também contra o banco real, e os nomes dos itens ficam vazios em
  `item + 0x114`. Falta descobrir o caminho que deveria preencher a lista.
- [ ] **Desempenho: 12 FPS medidos** (esperado 30/60). Suspeitos principais: o
  `glReadPixels` por quadro (640×330 RGBA = 845 KB) e as 211 200 chamadas de
  `Memory::Write16` por quadro em `SyncSurfaceColorBuffer`.
- [ ] **Entrada por HID**: as teclas chegam ao `HandleEvent` e voltam 0; o jogo
  usa o caminho do joystick (`Joystick.c:157 1 Joysticks connected`,
  `Joystick.c:183 No keyboard reported`). Falta verificar a entrega de eventos de
  botão do `IHIDDevice`.
- [ ] **`tt_dlqueue.db` cresce indevidamente**: 49 linhas DBINFO contra 14 do
  banco real — inserimos uma linha por execução.

### 4.3 Interruptores de bissecção disponíveis

Nenhuma hipótese deste ciclo foi aceita sem A/B. Chaves de ambiente ativas:
`ZEEB_NO_RES_IMAGE`, `ZEEB_2D_ONLY`, `ZEEB_TEX_DUMP`, `ZEEB_LOG_OOR`,
`ZEEB_LOG_SNPRINTF`, `ZEEB_GL_NO_CULLFACE`, `ZEEB_GL_UNLIT`, `ZEEB_GL_NO_STENCIL`,
`ZEEB_GL_NO_PIXELSTORE`, `ZEEB_GL_NO_MULTITEX`, `ZEEB_NO_PBUFFER_FBO`,
`ZEEB_NO_COLORBUF_READBACK`, `ZEEB_EGL_DUMP`, `ZEEB_GL_TRACE`, `ZEEB_LOG_GPU`.

---

## 5. Fase 4: Gráficos Avançados & Composição Offscreen (Planejada)

- [ ] **`IDisplay::SetDestination` Funcional**:
  - Fazer com que `DrawText`, `DrawRect` e `BitBlt` desenhem na superfície ativa selecionada (`destination_ptr_`), em vez de sempre escreverem direto no framebuffer primário.
- [ ] **Conformidade de Tipos em `IDisplay_DrawText`**:
  - Tratar a discrepância entre `AECHAR = uint16_t` do SDK oficial e strings narrow de 8-bit como quirk específica de títulos comprovados (ex: *Double Dragon*), em vez de regra global.
- [ ] **Extensões Fixed-Function do OpenGL ES 1.1**:
  - Implementação das extensões restantes de combinadores de textura e estados de renderização exigidos por títulos 3D.
- [x] **Contrato `SetupNativeImage` no Runtime**:
  - Preencher `AEEImageInfo` em R2 e out-param `*pbRealloc` em R3 conforme `AEEStdLib.h:90-91` (evita free indevido de ponteiro interno a pBuffer).
- [ ] **Refcounting do Device Bitmap**:
  - `IDisplay::GetDeviceBitmap` deve respeitar `AddRef`/`Release` legítimos de acordo com `AEEIBase.h`.

---

## 6. Fase 5: Concorrência, Servidores & Higiene de Arquitetura (Planejada)

- [ ] **Eliminação de Data Races nos Servidores de Inspeção**:
  - `mirror_server` / `/api/mem`: Leitura atômica ou via snapshot sob sincronização com a thread principal de emulação (evitando concorrência com `unordered_map` e escritas de CPU/JIT).
- [ ] **Shutdown Não-Bloqueante dos Servidores de Controle**:
  - Chamar `shutdown()` nos sockets clientes aceitos para destravar loops em `recv()` síncrono durante `Stop()`.
- [x] **Quotas Restantes de Desserialização e Parsers**:
  - Tetos de alocação e verificação estrita em `Mixer::Deserialize` (1024 vozes, 64M amostras máx) e `DeserializeGlTextureLog` (64k entradas, 64M texels máx).
  - Proteção contra overflow/duração extrema (limite de 1 hora / 64M amostras) em `RenderMidiToPcm` e `SoundFontSynth`.
  - Validação da contagem exata de trilhas declaradas em `ParseMidi`.
- [ ] **Validação Estrita de VFS e Sandbox**:
  - Bloquear travessia via symlinks dentro de pacotes `.mod` que apontem para arquivos fora da árvore do jogo.
- [ ] **Polimento de `IFileMgr` e Sistema de Arquivos**:
  - Corrigir criação indevida de handles de diretório sintéticos quando caminhos terminam com barra (`/` ou `\`).
  - `IFILEMGR_EnumInit`: Respeitar filtro de diretório (`bDirs`) e subpastas.
  - `IFILEMGR_GetFreeSpace`: Descontar espaço ocupado por arquivos gravados pelo jogador.
  - Implementar de forma real os slots 13 a 20 (`ResolvePath`, `GetFreeSpaceEx`, etc.).
- [ ] **Integridade do SQL HLE**:
  - [x] Esgotamento de scratch tratado no `PushScratchString`: aborta com falha limpa em vez de passar ponteiro nulo para colunas TEXT não-nulas.
  - [ ] Prevenir que callbacks de linha em `ISQL_Exec` façam o fechamento (`DbRelease`) imediato da conexão com statements ativos (`SQLITE_BUSY`).
- [ ] **Scheduler & Preempção de Timers**:
  - Tratar adequadamente timers preemptados que realizam yield (`tr.yielded`), preservando sua continuação em vez de sobrescrever com a rotina anterior.
- [ ] **ABI Formal de `IShell_SendEvent`**:
  - Selecionar a ABI de 6 parâmetros (`wFlags`, `clsApp`, `evt`, `wParam`, `dwParam`) conforme a versão negociada do BREW em vez de heurística sobre o valor numérico do CLSID.

---

## 7. Fase 6: Catálogo Geral & Jogabilidade Genuína (Ongoing)

- [ ] **Cluster `LOOP_NOFRAME`**:
  - Identificar os gatilhos ausentes de apresentação de quadro em títulos que entram no laço principal mas não realizam `Update()` / `eglSwapBuffers`.
- [ ] **Decodificação de Formatos Proprietários**:
  - Engenharia reversa dos formatos PZX/WBL do motor Nexus2 (*Zenonia*).
  - Codecs de áudio específicos da Qualcomm (QCP, AMR).
- [ ] **Arqueologia EFS2 / NAND**:
  - Reversão da tabela de gnodes para associar nomes de arquivos às cadeias de clusters já extraídas.
- [ ] **Critério de Saída Global**:
  - Jogabilidade comprovada (gráficos íntegros, entrada responsiva, áudio estável e persistência funcional) em pelo menos 80% do catálogo comercial do Zeebo, sem stubs silenciosos ou constantes forçadas.

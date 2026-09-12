# Zeebulator — Roadmap Técnico de Engenharia & Compatibilidade

Documento vivo e exaustivo de planejamento estratégico, fases de entrega, auditoria formal de conformidade com o BREW SDK e pendências técnicas mapeadas para levar o **Zeebulator** da inicialização básica até a jogabilidade genuína em todo o catálogo comercial do Zeebo.

---

## 1. Visão Geral das Fases

```
[ Fase 1: Fundação & ABI Core ] (Concluída)
               │
               ▼
[ Fase 2: Robustez de Memória & Parsers ] (Concluída - Auditoria 59336ea)
               │
               ▼
[ Fase 3: Desbloqueio da Z-Wheel & Pipeline Visual 2D/3D ] (Fase Atual / Em Andamento)
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
- [ ] **Readback GL para Pbuffer**:
  - Implementar o readback do framebuffer OpenGL do host para a memória guest no ponteiro do pbuffer, permitindo que a composição 2D/3D híbrida funcione.
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

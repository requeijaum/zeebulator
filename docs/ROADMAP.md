# Zeebulator — Roadmap Técnico de Engenharia & Compatibilidade

Documento vivo de planejamento estratégico, fases de entrega e pendências técnicas para levar o **Zeebulator** da inicialização básica até a jogabilidade genuína em todo o catálogo comercial do Zeebo.

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

- [x] Decodificação e execução ARM1136 / Thumb com suporte diferencial (ArmInterpreter vs Dynarmic).
- [x] Containers base: GGZ, MIF, BAR, PKG, FUFS (detecção).
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
- [ ] **Investigação do `SetDrawHandler` (Slot 16)**:
  - *Gap*: O erro 6 sumiu, mas o applet ainda não chama o slot 16 para registrar o callback de desenho da roda/carrossel.
  - *Ação*: Mapear o fluxo pós-inicialização do applet e investigar o que impede o agendamento da renderização da interface.
- [ ] **Readback GL para Pbuffer**:
  - Implementar o readback do framebuffer OpenGL do host para a memória guest no ponteiro do pbuffer, permitindo que composição 2D/3D híbrida funcione.
- [ ] **Árvore Hierárquica de Widgets & Ciclo de Vida**:
  - Manter relacionamentos pai-filho e geometria relativa de acordo com o slot 5.
  - Gerenciamento de ciclo de vida completo no slot 17 (fontes `AEECLSID_FONTSYS` e modelos `0x8000`).
- [ ] **Despacho Ordenado de Entrada na Interface**:
  - Roteamento prioritário de eventos `EVT_KEY` aos handlers de tela ativos antes do applet.

---

## 5. Fase 4: Gráficos Avançados & Composição Offscreen (Planejada)

- [ ] **`IDisplay::SetDestination` Funcional**:
  - Fazer com que `DrawText`, `DrawRect` e `BitBlt` desenhem na superfície ativa selecionada (`destination_ptr_`), em vez de sempre escreverem direto no framebuffer primário.
- [ ] **Conformidade de Tipos em `IDisplay_DrawText`**:
  - Tratar a discrepância entre `AECHAR = uint16_t` do SDK oficial e strings narrow de 8-bit como quirk específica de títulos comprovados, em vez de regra global.
- [ ] **Extensões Fixed-Function do OpenGL ES 1.1**:
  - Implementação das extensões restantes de combinadores de textura e estados de renderização exigidos por títulos 3D.

---

## 6. Fase 5: Concorrência, Servidores & Higiene de Arquitetura (Planejada)

- [ ] **Eliminação de Data Races nos Servidores de Inspeção**:
  - `mirror_server` / `/api/mem`: Leitura atômica ou via snapshot sob sincronização com a thread principal de emulação.
- [ ] **Shutdown Não-Bloqueante**:
  - Encerramento seguro de sockets de controle mesmo com clientes conectados ociosos.
- [ ] **Quotas Restantes de Desserialização**:
  - Aplicar checagens de integridade e tetos de alocação nos desserializadores do `Mixer` e `GlTextureLog`.
- [ ] **Validação Estrita de VFS**:
  - Impedir resolução de symlinks que apontem para fora da raiz do pacote do jogo.
- [ ] **Preenchimento de Slots de `IFileMgr`**:
  - Implementar ou manter rejeição canônica detalhada nos slots 13 a 20 (`ResolvePath`, `GetFreeSpaceEx`, etc.).

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

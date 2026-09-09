# Zeebulator — TODO & Gaps vs Oráculo (zeebx)

Documento vivo de mapeamento de gaps, quick wins e lições aprendidas a partir do oráculo de engenharia reversa (`zeebx`).
Foco em melhorias incrementais HLE que permitam destravar o boot e execução correta do catálogo comercial do Zeebo.

---

## 1. Concluído & Integrado (Quick Wins Recentes)

- [x] **VFS Case-Insensitive**: Fallback `strcasecmp` no `VirtualFilesystem::Find` e resolução case-insensitive em disco no `MissResolver` (`tools/game_probe.cpp`). Corrige a discrepância dos 10 ports da Data East (`font.fnz` vs `font.FNZ`, ROMs em minúsculas vs maiúsculas no pacote).
- [x] **Tratamento de `ALLOC_NO_ZMEM` (0x80000000)**: `ModRuntime::Allocate` agora mascara o bit alto `size &= ~0x80000000u`. O SDK oficial do BREW (`AEEStdLib.h`) define `MALLOC(size)` como `size | ALLOC_NO_ZMEM`; anteriormente isso causava alocações gigantescas de 2 GB que falhavam silenciosamente.
- [x] **Canonicidade de Class IDs no `nid_table.cpp`**: Mapeamento dos identificadores numéricos oficiais para nomes do SDK:
  - `0x01001002` = `AEECLSID_HEAP` (`IHeap`)
  - `0x01002001` = `AEECLSID_GRAPHICS` (`IGraphics`)
  - `0x0100100c` = `AEECLSID_MEMASTREAM` (`IMemAStream`)
  - `0x0100100f` = `AEECLSID_LICENSE` (`ILicense`)
  - `0x01041207` = `AEECLSID_SIGNAL_CB_FACTORY` (`ISignalCBFactory`)
  - `0x01005511` = `AEECLSID_MEDIAPCM` (`IMedia` PCM/WAV)
  - `0x0101eb0b` = `AEEIID_FORCEFEED` (streaming de PNG decoder)
  - `0x01026e23` = `AEECLSID_PNGDECODER` (`IImageDecoder`)
  - `0x01030766` = `AEECLSID_PNGDECODER_BREW` (`IImageDecoder` BREW)
  - `0x01004004` = `AEECLSID_PNG` (`IImage`)
  - `0x01005000` = `AEECLSID_WEB` (`IWeb`)
  - `0x01001015` = `AEECLSID_MD5` (`IHash`)
  - `0x0102cce1` = `AEECLSID_CIPHER_FACTORY` (`ICipherFactory`)
- [x] **Helper 0xc0 (`GetAppInstance`) Conectado ao `ppObj`**: Interceptação do `ppObj` em `IModule::CreateInstance`. O slot 0xc0 agora devolve o ponteiro legítimo do applet criado em vez de uma struct estática desvinculada.
- [x] **AEECallback Unpacking em `SetTimer` / `CancelTimer`**: Quando `pfnNotify == pUser`, o parâmetro é um `AEECallback*` com `pfn` no offset +16 e `pUser` no offset +20. Suporte implementado em `IShellHle`.
- [x] **`IShell::GetDeviceInfo` Canônico Completo**: Implementado o preenchimento de `AEEDeviceInfo` seguindo fielmente a especificação do BREW SDK e o mapeamento do oráculo `zeebx` (`machine.rs:2690`). Popula `cxScreen=640`, `cyScreen=480`, `cxScrollBar=8`, `wEncoding=AEE_ENC_ISOLATIN1 (3)`, `wColorDepth=16`, `dwRAM=64MB` (`+24`), e campos estendidos (`wStructSize=64`, `wMaxPath=64`, `dwPlatformID=0`). Evita stalls em jogos como *Double Dragon* e *Bejeweled Twist*.
- [x] **`ISHELL_DetectType` Canônico (Vtable Slot 43)**: Substituição da heurística empírica de toggle (35/0) pelo contrato canônico do BREW SDK 4.0.2 / oráculo `zeebx`. Quando chamado sem dados (`cpBuf == NULL && cpszName == NULL`), responde com `*pdwSize = 16` (`DETECT_TYPE_BYTES`) e retorna `ENEEDMORE` (35). Na segunda chamada com o cabeçalho, realiza detecção de MIME por magic bytes (PNG, JPEG, GIF, BMP, MIDI, MP3, WAV, AMR) com fallback por extensão de nome e internamento de strings de resposta.
- [x] **Desacoplamento de `EVT_APP_RESUME` no `game_probe`**: O envio de `EVT_APP_RESUME` agora é condicionado a títulos first-party (`zeebulator::compat::IsFirstPartyTitle` — faixa `0x0108eff0..0x0108ffff`), como a série *Zeebo Extreme* e *Zeebo Sports*, onde a máquina de estados requer a transição pós-timer. Títulos third-party (como o cluster Data East: *Caveman Ninja*, *Spin Master*, *BurgerTime*, etc.) deixam de colidir no falso-positivo do guard `0x10350c` e inicializam threads cooperativas no laço autônomo.

---

## 2. Em Andamento (Cluster Data East & Boot de Jogos)

- [ ] **Spinlock de Tempo Cooperativo (`note_spin`)**:
  - *Lição do Zeebx*: Jogos como Zeebo Sports Peteca, Quake e Data East entram em laços de espera ocupada lendo `aee_GetUpTimeMS`.
  - *Quick Win*: Se o guest fizer mais de 64 leituras seguidas de relógio sem realizar trabalho de renderização/I/O, avançar o relógio virtual por passos (`SPIN_STEP_US = 250`) em vez de queimar milhões de instruções sem progresso temporal.
- [ ] **Implementação Básica de `IHeap` (`0x01001002`)**:
  - *Lição do Zeebx*: Vários jogos consultam o método `CheckAvail` (slot 6 da vtable) antes de alocar recursos. Se não implementado, o jogo aborta ou desenha "Memory is insufficient".

---

## 3. Próximos Gaps Mapeados (Módulos / Interfaces Faltantes)

- [ ] **`IMD5` / `IHash` (`0x01001015`)**:
  - Utilizado na inicialização para checagem de integridade de assets por jogos mais recentes.
- [ ] **`IMemAStream` (`0x0100100c`)**:
  - Envelopamento de ponteiros de memória guest em fluxos `IAStream` para consumo direto por decodificadores de imagem e áudio.

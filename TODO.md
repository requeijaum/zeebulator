# Zeebulator — TODO & Gaps vs Oráculo (zeebx)

Documento vivo de mapeamento de gaps, quick wins e lições aprendidas a partir do oráculo de engenharia reversa (`zeebx`).
Foco em melhorias incrementais HLE que permitam destravar o boot e execução correta do catálogo comercial do Zeebo.

---

## 1. Concluído & Integrado (Quick Wins Recentes)

- [x] **`IHash` / `AEECLSID_MD5` (`0x01001015`) — Implementação Real**: `HashHle` (`core/brew/hash_hle.{h,cpp}`) implementa MD5 completo (RFC 1321) com o contrato canônico do BREW SDK (`Reset`/`Update`/`GetDigest`/`GetDigestSize`/`AddRef`/`Release`). Testado contra os vetores de teste oficiais do RFC 1321 (`""` → `d41d8cd9...`, `"abc"` → `900150983cd2...`), incluindo acumulação de `Update` cruzando a fronteira de bloco de 64 bytes. Confirmado como a ÚLTIMA API faltante em todo o corpus de referência de 61 títulos do `zeebx` (o próprio log do oráculo registra `ponteiros inválidos recebidos: IHash::Update` para Zeeboids). Rodando o probe do Zeeboids após a implementação, esse ponteiro inválido desaparece completamente do log.
- [x] **VFS: Resolução de Subdiretório em Miss-Resolver Lazy**: O miss-resolver de assets soltos (`tools/game_probe.cpp`) rejeitava qualquer nome de arquivo contendo `/`, então nunca conseguia servir arquivos pedidos por caminho relativo com subpasta (ex: `zeeboiddata/version.txt` do Zeeboids, presente em disco mas nunca resolvido). Causa raiz mais profunda: `VirtualFilesystem::Find()` (`core/brew/virtual_filesystem.cpp`) já colapsava todo miss para um basename plano ANTES de consultar o resolver, descartando o prefixo do subdiretório. Corrigido em duas camadas: (1) `Find()` agora tenta o caminho relativo canônico completo primeiro, caindo para basename como antes; (2) o resolver do harness agora suporta um nível de subdiretório case-insensitive. Título beneficiado confirmado: Zeeboids (arquivo antes "arquivos não encontrados", agora resolvido).
- [x] **`IHeap` (`0x01001002`) Implementado**: `HeapHle` (`core/brew/heap_hle.{h,cpp}`) implementa `CheckAvail` (slot 6) e o restante do contrato mínimo de `IHeap` — jogos que verificam memória disponível antes de alocar recursos (evitando o abort/"Memory is insufficient") agora recebem uma resposta plausível. Testado em `tests/heap_hle_test.cpp`.
- [x] **`IThread` — Spinlock/Cooperativo Básico**: `ThreadHle` (`core/brew/thread_hle.{h,cpp}`) dá suporte a criação/yield/resume cooperativos de threads guest, endereçando o padrão de espera ocupada em `aee_GetUpTimeMS` descrito na seção "Em Andamento" anterior deste documento. Testado em `tests/thread_hle_test.cpp` (herdado do commit anterior, agora integrado ao build).
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

## 2. Em Investigação (Novo Gap Real — Zeeboids)

- [ ] **`OpenFile("%s%s", ...)` — string de formato não expandida chegando literal no `IFileMgr::OpenFile`**:
  - *Sintoma*: Após corrigir IHash e o subdiretório de assets, Zeeboids ainda chama repetidamente `IFileMgr::Test("%s%s")` / `OpenFile("%s%s")` — o literal do format string em si, não a string substituída — e recebe MISS.
  - *Investigação até agora*: O call site real (via trace ao vivo, `LR=0x0014cb1c`, `guest addr` real = `0x14cb1c`, base do módulo confirmado em runtime como `0x00100000`, não `0xf000`) cai dentro de uma rotina wrapper em `0x0014ca38` (RE'd via `objdump -m arm --adjust-vma=0x100000`), com múltiplos call sites (`0x10968c`, `0x1096e0`, `0x1195fc`, etc.) — um fopen-style helper que recebe `r0=<contexto>`, `r1=<algo em stack, mode?>` e chama através de slots da static-base table (mesmo padrão STRCPY/STRSTR documentado no `mod_runtime.h`).
  - *Hipótese corrente*: um sprintf-family call ANTERIOR (upstream desse wrapper, ainda não localizado) falha silenciosamente ou nunca é executado, deixando o ponteiro do buffer de destino apontando ainda para o literal do format string original em vez do resultado formatado — passado adiante intacto até o `OpenFile`.
  - *Próximo passo concreto*: instrumentar um trace watchpoint na leitura/escrita do endereço do buffer de destino (não apenas do LR de `OpenFile`) para capturar QUAL call site de sprintf (0x13c ou possivelmente um slot ainda não mapeado) deveria ter escrito ali e não escreveu — ou então confirmar se o argumento de entrada da wrapper já chega errado desde o applet (nesse caso o gap está a montante, na formação da string do nome do arquivo em si, possivelmente ligado ao helper `GetAppContext` slot 0xc0 devolvendo um contexto por-jogo com um campo de path-prefix ainda não populado).
  - *Escopo do impacto*: pelo menos Zeeboids; padrão fopen-wrapper reaparece em vários call sites (~7+ localizados via `bl 0x14ca38`), sugerindo que, se for um bug genérico do sprintf (não específico de Zeeboids), pode afetar outros títulos que constroem paths dinamicamente com `%s` duplo.

---

## 3. Próximos Gaps Mapeados (Módulos / Interfaces Faltantes)

- [ ] **`IMemAStream` (`0x0100100c`)**:
  - Envelopamento de ponteiros de memória guest em fluxos `IAStream` para consumo direto por decodificadores de imagem e áudio.
- [ ] **Spinlock de Tempo Cooperativo — Refinamento (`note_spin`)**:
  - *Lição do Zeebx*: Jogos como Zeebo Sports Peteca, Quake e Data East entram em laços de espera ocupada lendo `aee_GetUpTimeMS`. O `ThreadHle` básico já existe (ver seção 1); falta o passo de avanço de relógio virtual em `SPIN_STEP_US` quando o guest fizer mais de 64 leituras seguidas de relógio sem trabalho real de renderização/I/O — ainda não implementado.

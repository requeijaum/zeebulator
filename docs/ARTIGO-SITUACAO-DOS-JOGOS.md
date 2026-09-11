# Situacao real de cada jogo no Zeebulator

Este artigo e o registro honesto do estado do emulador por titulo. Ele separa
tres coisas que o projeto ja confundiu no passado:

1. **Carga**: o modulo carrega, o applet e criado e o laco de eventos roda.
2. **Imagem**: existe quadro medido, em pixels e cores, com video SDL real.
3. **Jogo**: o titulo avanca de estado com entrada do jogador e continua
   desenhando depois disso.

Boot nao e jogabilidade. Toda afirmacao aqui vem de medicao ou de leitura de
arquivo; onde nao houver evidencia, o texto diz que nao ha.

## Metodo

- Video e audio reais, `DISPLAY=:0`, sem driver offscreen.
- Captura por X11, contando pixels nao pretos e cores distintas. Cerca de
  500 px e 2 cores e apenas o overlay de FPS, ou seja, tela vazia.
- Entrada por canal de controle do proprio probe, que injeta o mesmo evento
  HID que o teclado gera.
- Analise estatica de containers e strings antes de qualquer desmontagem.
- Comparacao com quatro fontes independentes: headers do SDK BREW MP,
  Zeebx (Rust), Zeemu (C++) e Zeebo-LLE.


## Resumo Geral do Catálogo (63 Títulos de Execução + 4 Pastas de Recursos)

A análise combinada de bytecode, assets e telemetria revelou que o catálogo oficial do Zeebo
está dividido em grandes famílias arquiteturais. O comportamento diante de emulação é quase
sempre uniforme dentro da mesma família:

1. **TTD Middleware (Tectoy Digital, 17 títulos)**: Jogos como *Zeebo Sports Tênis*, *Vôlei*,
   *Peteca*, *Zeeboids*, *Rolimaz*, etc. Usam `ttd_file_mgr`, `ttd_packer` e pacotes `.pakz`.
   A engine cria threads cooperativas e aloca blocos gigantescos de heap (~23 MB via `MALLOC`).
   Status: o boot e thread loops estão ativos, mas a apresentação final depende de inicialização
   de superfícies e estados de suspensão no `ModRuntime`.
2. **Arcade Emulation Core (Data East / SNK, 10 títulos)**: *Caveman Ninja*, *Spinmaster*,
   *Street Hoop*, *Magical Drop III*, *Karnov's Revenge*, *Bad Dudes*, etc.
   Na verdade, são emuladores de arcade executando dentro do BREW! Eles carregam ROMsets reais de
   Neo Geo e DECO (inclusive `boot.pkg` de BIOS). Falhas históricas decorriam de `AEEBitmapInfo`
   com layout truncado corrompendo a pilha e ausência de suporte adequado a ROMs no VFS.
3. **Polarbit FUFS (6 títulos)**: *Crash Nitro Kart 3D*, *Armageddon Squad (ASQ)*, *Iron Sight*,
   *Reckless Racing*, *Raging Thunder 2*, etc. Utilizam contêineres `.vfs` com tabelas indexadas
   por hash polinomial case-insensitive.
4. **Gamevil / WIPI (*Zenonia*)**: O motor Nexus2/WBL interpreta arquivos `.pzx` e desenha diretamente
   em superfícies `IDIB` e bitmaps compatíveis em RGB565. Bloqueios de áudio e busca por substrings
   em `AEEHelperFuncs` slot `0xd8` (`strstr`) foram corrigidos nesta sessão, permitindo carregamento
   de mapas e início de gameplay.
5. **PopCap (3 títulos)**: *Peggle*, *Zuma's Revenge*, *Bejeweled Twist*. Dependem do formato `IDIB`
   em 16-bit RGB565 e chamadas complexas de `LoadResDataEx`.
6. **Namco Networks (3 títulos)**: *Pac-Mania*, *Ridge Racer*, *Tekken 2*. O *Pac-Mania* usa
   OpenGL ES 1.1 direto (`DrawElements`) enviando coordenadas diretamente nos arrays de vértices,
   além de uploads `TexSubImage2D` de sprites.

---

## Tabela de Situação Diagnóstica Detalhada

| ID / Pasta | Título | Família / Engine | Status Medido no Zeebulator | Próxima Ação Técnica |
|---|---|---|---|---|
| `277455` | *Zenonia* | Gamevil WIPI (Nexus2) | **Entra no jogo; não certificado como jogável**. Mapas carregam após `strstr` e áudio não-reentrante; sprites/frames ainda apresentam artefatos. | Medir transparência, blits e estabilidade em sessão longa |
| `276212` | *Pac-Mania* | Namco GLES 1.1 | **Renderizando Título / Labirinto** (307.200 px / 9.769 cores, sprites em vértices) | Investigar binding e sub-regiões de textura de fantasmas/Pac-Man |
| `279369` | *Alien Breaker Deluxe* | Vega Mobile | **Renderizando Splash / Menu** (303.042 px / 7.910 cores) | Conectar atlas ATITC diretamente ao pipeline de desenho de texto |
| `274754` | *Double Dragon* | Brizo Interactive (.ggz) | **Renderizando** (109.788 px / 262 cores, loop de timer ativo a 31 FPS) | Validar transição após tela de título |
| `263019` | *ChessBots* | Superscape Swerve (.sar) | **Renderizando com distorção** (64.644 px / 3 cores) | Corrigir matrizes de projeção em `Frustumf` e geometrias Swerve |
| `277534` | *Zeebo Sports Tênis* | TTD Middleware | Loop de eventos ativo, tela em branco (307.200 px / 2 cores) | Analisar ciclo de vida da thread principal e buffer de quadro |
| `278212` | *Zeebo Sports Vôlei* | TTD Middleware | Loop de eventos ativo, tela em branco | Idem ao Tênis (compartilham a mesma base `ttd_packer`) |
| `279159` | *Zeebo Sports Peteca* | TTD Middleware | Loop de eventos ativo, tela em branco | Idem ao Tênis |
| `279382` | *Zeeboids* | TTD Middleware | Loop de eventos ativo, tela em branco | Idem ao Tênis |
| `278962` | *Peggle* | PopCap | Loop de eventos ativo, quads sólidos detectados | Implementar apresentação de quads genéricos no backend |
| `274802` | *Quake* | id Tech / Tectoy | Loop de eventos ativo, splash carregado | Investigar inicialização do contexto de software rasterizer |
| `274755` | *Z-Wheel (Menu)* | Rocket Mobile / Tectoy | **Boot parcial, avançou nesta sessão**: `EVT_APP_START` retorna sem exceção, o formulário de animação e o `LoadResStringEx` deixaram de falhar; **tela branca pura (307.200 px / 1 cor)** e nenhum desenho. Bloqueio medido: formulário do z-pad devolve `EUNABLETOLOAD` (6). | Ver a seção "Z-Wheel: o que foi medido" abaixo |
| `277495` | *Opera Mini (reksio)*| Opera Software | Loop de eventos ativo (Requer stack de sockets/rede) | Fornecer bridge HLE para sockets TCP/IP |

---

## Z-Wheel: o que foi medido (sessão atual)

Titulo: `mod/274755/tectoy.mod`, ClsId `17237912` (`0x0107...`), applet
`0x01070798`. Todas as execucoes sao passivas (`ZEEB_DISABLE_INPUT=1`), sem JIT,
com video SDL real e `DISPLAY=:0`.

### Antes e depois, medido

| Medicao | Antes | Depois das correcoes desta sessao |
|---|---|---|
| `IModule::CreateInstance` | terminava em excursao para `pc=0` e o `HandleEvent` lancava excecao | termina OK, sem excecao |
| `HandleEvent(EVT_APP_START)` | excecao dentro do handler | retorna |
| `Couldn't create animation video form (1)` (`tectoymain.c:807`) | presente | **desapareceu** |
| `LoadResStringEx fail for ID (1002)` | presente | **desapareceu** no primeiro ciclo |
| Quadro capturado | preto | **branco puro**, 307.200 px / 1 cor |

### As correcoes, e por que cada uma e defensavel

1. **`preloaded.cfg` existe e esta vazio.** Quem cria esse arquivo e o console, na
   area do usuario; nenhum pacote o traz. O guest faz `IFileMgr::Test` ->
   `GetInfo` -> `OpenFile` e, recebendo "nao existe", sai do caminho da lista de
   pre-instalados. `fontsize.map` **nao** entra nessa lista: medicao independente
   (varredura dos 128 MiB da NAND) confirma que ele nao existe em lugar nenhum, e
   o documento da roda chega a mesma conclusao por outro caminho.
2. **Slots 4 e 16 do widget devolvem o registro anterior.** O BREW encadeia
   tratadores: o tratador novo le, da propria estrutura do chamador, para onde
   desviar o que nao trata. Sem a devolucao, a estrutura continua descrevendo o
   tratador recem-instalado e o desvio vira recursao.
3. **`AEECLSID_MEDIAUTIL` (`0x0100550d`) registrada.** Nao e codec, e a fabrica
   de objetos de midia -- por isso faltava na lista de classes de formato. O SDK
   que temos traz interface e implementacao de referencia
   (`platform/media/inc/AEEMediaUtil.h`, `.../src/mediautil/AEEMediaUtil.c`).
   `CreateMedia` cria o objeto e aplica o `AEEMediaData` pelo mesmo caminho que o
   guest usaria, porque o proprio SDK define
   `IMedia_SetMediaData(p,pmd)` como `SetMediaParm(p, MM_PARM_MEDIA_DATA, (int32)pmd, 0)`
   (`platform/media/inc/AEEIMedia.h`). Quem pede essa classe no corpus:
   `tectoy.mod` (3 sitios), `rocketweb.mod` (2), `allstarcards.mod` (1),
   `quake.mod` (1).
4. **Estado do widget indexado por `(this, id)`.** As duas tabelas do acessador
   eram indexadas so pelo id, como se existisse um widget unico. A
   instrumentacao `ZEEB_LOG_WIDGET_ALL=1` (nova) mostra o guest falando com
   `0x8006d000`, `0x8006d100`, `0x8006d140` e `0x8006c000` na mesma execucao, e o
   item `0x5000` e o que cada formulario usa para pendurar o proprio conteudo.
   Efeito medido no boot: nenhum ainda; a correcao entra porque o estado
   compartilhado e comprovadamente errado.

### Fatos do SDK que esta investigacao confirmou (fonte primaria)

| Fato | Onde esta escrito |
|---|---|
| `AEECLSID_DOWNLOAD` **e** `0x01000000` | `platform/system/inc/AEEClassIDs.h`: `#define AEECLSID_DOWNLOAD (AEECLSID_PRIV)`, `AEECLSID_PRIV (QVERSION)`, `QVERSION 0x01000000` |
| Codigos de erro | `platform/system/inc/AEEError.h`: `SUCCESS 0`, `EFAILED 1`, `ENOMEMORY 2`, `ECLASSNOTSUPPORT 3`, **`EUNABLETOLOAD 6`**, `EBADCLASS 10`, **`EBADPARM 14`**, `EUNSUPPORTED 20` |
| `IMediaUtil` tem 6 slots | `AEEMediaUtil.h`: `AddRef, Release, QueryInterface, CreateMedia, EncodeMedia, CreateMediaEx` |
| `IMedia_SetMediaData` nao e slot | `AEEIMedia.h`: e `SetMediaParm(MM_PARM_MEDIA_DATA, pmd, 0)` |

O SDK corrige de passagem uma suspeita antiga: a Z-Wheel pedir a classe
`0x01000000` em `Tectoy_FixupTime` e `ShopAction_Init` **nao** e valor truncado
nem dado corrompido -- e exatamente `AEECLSID_DOWNLOAD`. Nos recusamos essa
classe hoje, e as duas mensagens "Unable to create instance of IDOWNLOAD" vem
dai.

### O caminho da string de recurso, e o que ainda falta

A cadeia do formulario do z-pad, medida instrucao por instrucao (trace de
execucao, nao disassembly linear):

```
0x16ed60  bl 0x17ec54        ; cria o formulario 0x01028e47, escreve o item 0x5002
0x17ec54  ... bl 0x17f580    ; monta o conteudo do formulario
0x17f580  0x17f6f8  ldrsh r1,[r5,#0x24]   ; id do recurso (1178 = "Z-Pad")
          0x17f700  bl 0x179518            ; (shell, id)
0x179518  0x179528  bl 0x1785a4  -> SendEvent(0x7b0a, wParam, &resposta)
          0x179540  bl 0x179594            ; le a string pelo slot 41 do objeto
          0x179554  bl 0x179290            ; alternativa, tambem falha
          0x17956c  devolve 0
0x17f580  0x17f704  cmp r0,#0 ; bne ...
          0x17f70c  mov r4,#6              ; EUNABLETOLOAD
```

Duas coisas ficaram provadas nesta sessao: (a) o evento 0x7b0a agora chega ao
applet e a resposta dele e `applet+0x2ef8` -- exatamente a constante que este
projeto tinha chumbado como palpite, agora confirmada pelo proprio guest;
(b) as strings existem e sao legiveis: `tectoyli.brf` id 1178 = "Z-Pad" e
`tectoy_pt.brf` id 1002 = "Nao foi possivel inicializar.".

O que continua sem resposta: o guest le o ponteiro de funcao que chama de
`*(*(resposta)) + 0xa4`, e para `resposta = 0x80302f1c` isso da a palavra em
`0x80001000 + 0xa4` -- dentro do NOSSO objeto de shell, onde ela e zero. Um
`bx` para zero e exatamente a excursao `pc=0x00000000` que aparece no log
(recoverable wander, ver abaixo). Ou seja: a forma do objeto que o guest espera
do evento 0x7b0a ainda nao esta entendida, e e o proximo alvo.

### Onde a Z-Wheel para agora (bloqueio medido, nao hipotese)

O app passa o gate de 30 ticks do splash (o contador em `[r4+0x30]` do callback
`0x1014ac`, rearmado por `IShell::SetTimer`, slot 11) e entao monta a proxima
tela. Essa montagem falha:

```
[guest] Tectoy.c:743   Unable to launch z-pad intructions form: 6
[guest] Tectoy.c:760   Couldn't create z-pad instruction form (6)
[guest] tectoymain.c:1547 Unable to launch z-pad intructions form: 6
```

Cadeia estatica ate a falha: `0x16ed60 -> 0x17ec54` (cria o formulario da classe
`0x01028e47`, escreve o item `0x5002`, e chama `0x17f580`) -> `0x17f580` devolve
`6` (`EUNABLETOLOAD`). Perfil de chamadas HLE (`ZEEB_HLE_PROFILE=1`) em toda a
faixa `0x17f...`: **todas as chamadas implementadas devolveram SUCCESS**. Ou
seja, o `6` nasce dentro do proprio guest, em codigo que ainda nao foi lido por
inteiro -- nao em um stub nosso. Depois disso o app fica num pulso de 1 s
lendo pontos e fila de download, sem nunca chamar `IBitmap`/`IDisplay` para
desenhar, que e exatamente o sintoma que o documento da roda descreve para o
palco recusado.

### Onde este projeto discorda do documento da roda (e por que)

- O documento cita `Couldn't create z-pad instruction form (20)` (classe
  `0x01028e36` recusada). Aqui o mesmo ponto devolve `6`, e a classe `0x01028e36`
  **esta** registrada. Ou seja: o erro deles e de classe faltando; o nosso e de
  carga de recurso. Mesmo sintoma, causa diferente.
- Os enderecos do documento (`0x22d58`, `0x88338`, `0x78acc`) nao caem no mesmo
  lugar do nosso `tectoy.mod`: `0x22d58` no nosso espaco e aritmetica de laco, e
  o modulo deles aparentemente tem outro build. Onde os dois batem
  (`0x101828` handler de boot, `0x178338` carregador chave:valor, `0x1783b8`
  desreferencia nula do `fontsize.map`), as conclusoes concordam.

---

## Lições Aprendidas na Auditoria de Código

1. **Auto-Citação e Falsa Certeza**: Vários trechos de código continham comentários como "confirmado por disassembly" que,
   ao serem cruzados com o SDK oficial (`AEEStdLib.h`, `AEEIBitmap.h`), revelaram-se suposições incorretas (ex.: `0xdc` ser
   gzip em vez de `memcmp`, ou `ECLASSNOTSUPPORT` ser 20 em vez de 3).
2. **Reentrância Assíncrona no BREW**: O modelo de componentes da Qualcomm proíbe estritamente que notificações de eventos
   (áudio, streams, sinais de controle) reentrem no código do guest durante a execução de um trap HLE. O adiamento para o
   próximo ciclo de `Tick` eliminou congelamentos instantâneos de vídeo após cliques de botão.
3. **Isolamento de Estado de Persistência**: Salvar `.userdata` e abrir bancos SQLite dentro do diretório `/media/.../ROMs/`
   contaminava dumps históricos e falhava em mídias somente-leitura. O redirecionamento para caminhos padrão XDG resolve
   a portabilidade.


## Ferramentas de Evidência e Limites da Análise

### `tools/binary_survey.py`

Extrai strings ASCII e UTF-16LE com offsets e, opcionalmente, executa `binwalk`.
É o primeiro passo para qualquer título: revela arquivos, mensagens de erro,
CLSID/IID e nomes de engine sem supor que bytes arbitrários sejam código.

### `tools/opcode_stats.py`

Analisa **somente** o trace produzido por `ZEEB_TRACE` (`DebugHooks::OnExec`).
Não deve ser substituído por `objdump -D` ou Capstone aplicado ao `.mod` inteiro:
um módulo mistura código, literais, strings, tabelas e dados comprimidos. Uma
varredura linear de bytes encontrou falsos `CP15`, `SVC` e DSP que desapareceram
quando a amostra foi limitada a instruções executadas.

Validação inicial da ferramenta, Z-Wheel, trace limitado a 5.000 entradas:

```text
3.790 instruções executadas; 1.405 PCs distintos; 0 linhas malformadas
faixa de PC: somente 0x001xxxxx (módulo guest esperado)
loop mais quente: 0x001008d8..0x001008ec, cópia em blocos, 1.536 execuções
```

O relatório fornece histograma de nibble alto do PC, entropia de PCs, categorias
de instrução e PCs quentes. Toda hipótese criada a partir dele deve ter controle
negativo, conforme `zeebo-lle/notes/STATS_TECHNIQUES.md`.

### Higiene de corpus

O corpus não é automaticamente imutável: versões anteriores do probe gravaram
`.userdata`, `.savestate`, `.playlog` e bancos SQLite ao lado da ROM. Esses
artefatos contaminam contagens de arquivos e extensões; 29 registros estáticos
os continham. A ferramenta agora prefere `~/.local/share/zeebulator/` (ou
`ZEEB_DATA_DIR`), com leitura compatível do caminho legado. Relatórios futuros
devem excluir artefatos gerados e registrar mtimes anômalos separadamente.

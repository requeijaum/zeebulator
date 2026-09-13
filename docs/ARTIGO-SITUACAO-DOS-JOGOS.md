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

- **RESSALVA IMPORTANTE (medida em 2026-09-13): o censo deste documento foi
  medido no INTERPRETADOR.** O `ZEEB_CPU=jit` era opt-in e o harness nao o pedia.
  Medido no zenonia: com JIT sao 486-725 ticks em 34-50 s e zero wander; sem JIT,
  58 ticks e o jogo travado. Medido tambem em amostra de 10 titulos: `a3d` tem
  **1.084 wanders no interpretador e zero no JIT**; `quake` e `chessbots` perdem o
  wander; `cnk2` faz 1,57x mais ticks. Ou seja, **sem JIT nao e a mesma
  configuracao para estes convidados** -- o JIT muda o entrelacamento dos timers
  do guest, nao so a velocidade. Os numeros de cores deste documento devem ser
  lidos com essa ressalva ate o censo ser refeito com JIT.
- Video e audio reais, sem driver offscreen. **Ciclo atual: Wayland**
  (`SDL_VIDEODRIVER=wayland`, `WAYLAND_DISPLAY=wayland-0`); os ciclos
  anteriores desta tabela foram medidos em X11 (`DISPLAY=:0`). Onde a
  diferenca importa, o texto diz qual foi usado.
- Captura pelo canal de controle do proprio probe (comando `screenshot`),
  contando pixels e cores distintas. Cerca de 500 px e 2 cores e apenas o
  overlay de FPS, ou seja, tela vazia.
- Contagem por criterio de cor, nao por impressao visual: verde = `G>R+25 e
  G>B+25`; amarelo = `R>150, G>150, B<110`; azul = `B>R+25 e B>G+25`. O
  criterio esta escrito porque "parece azul" nao e medicao.
- Entrada por canal de controle do proprio probe, que injeta o mesmo evento
  HID que o teclado gera.
- Analise estatica de containers e strings antes de qualquer desmontagem.
- Comparacao com quatro fontes independentes: headers do SDK BREW MP,
  Zeebx (Rust), Zeemu (C++) e Zeebo-LLE.


## Sessão de equiparação com o zeebx (2026-09-13)

O objetivo desta rodada foi equiparar o Zeebulator ao `zeebx` (branch `pr-2`,
topo `4f1333b`) **por medição**. Regra que valeu para tudo aqui: nenhuma
afirmação lida no `zeebx` entrou sem ser re-verificada neste corpus, com a nossa
instrumentação. O resultado não foi o que a regra sugere — **três das
transferências não se confirmaram**, cada uma por um motivo diferente, e só
apareceram porque foram medidas em vez de copiadas.

### Eixos do controle: o valor que o aparelho usa está escrito no jogo

O `zeebx` corrigiu o eixo analógico para um byte `0..255` com repouso em 128.
Medi no nosso corpus antes de aplicar: `funsoccer.mod` (Super League), em
`0x001ed504`, faz

```text
mvn   r0, #0x7f        ; r0 = -128
sxtah r4, r0, r4       ; valor = (int16)eixo - 128
strh  r4, [r2]         ; quatro eixos, gravados em halfword
```

Ou seja, o jogo subtrai 128 para achar o centro. Havia **três defeitos nossos**
na mesma fronteira: repouso em zero (que o jogo lê como `-128`, o manche
encostado no batente), faixa declarada como 16 bits com sinal, e o UID do eixo X
valendo `0x0106C40C` — que é UID de **botão**, o mesmo que o nosso próprio
código já usava. Eixo e botão com o mesmo UID não coexistem: o jogo varre a
tabela do `GetAxesInfo` procurando UID de eixo, não acha nenhum para o X, e
nunca guarda o campo dele.

Confirmação posterior, por fonte primária: o arquivo do próprio console,
`research/sources/zeemu/rootfs/sys/hid_devices.cfg`, traz quatro entradas de
controle de dois fabricantes diferentes, e **todas as quatro** declaram
`AXIS:X:0x0106c4d0`.

### UIDs de botão: o sul está confirmado, o resto não está medido

O `zeebx` corrigiu o pareamento dos botões de face. O valor que ele **mediu**
para o sul (`0x0106C40B`) é exatamente o que este projeto já usava. Oeste,
leste e norte seguem sem medição de nenhum dos dois lados — o próprio autor
escreve isso — e a entrada do controle do Zeebo (`VID:0x1EAA:PID:0x0135`), onde
ele encontra a troca espelhada, **não existe na nossa cópia do arquivo**. Trocar
os rótulos seria substituir um palpite por outro. Está anotado no código, onde
alguém iria mexer.

### Objetos de buffer do OpenGL: a hipótese não transferiu

O `zeebx` implementou objetos de buffer e o Prey Evil passou a rodar lá.
Aqui o `prey3d.mod` carrega **105 nomes `gl*`**, incluindo três variantes de
cada função de buffer (`glBindBufferARB/OES/QUALCOMM`, `glBufferData*`,
`glGenBuffers*`), e o nosso core não tem uma linha sobre o assunto. Parecia caso
fechado.

Instrumentei o `eglGetProcAddress` antes de implementar nada. **O Prey Evil
nunca o chama.** Zero pedidos. Os 105 nomes são uma tabela de extensões que o
engine carrega e não usa. Implementar buffer objects não mudaria nada aqui.

Onde ele realmente para, medido com `ZEEB_TRACE_HE=1`: `HandleEvent(EVT_APP_START)`
devolve 0 — no BREW, falso no start significa que o applet **recusou iniciar**. A
última chamada antes da cadeia de falha é o **slot 9 do objeto QEGL**, e o
chamador lê o resultado de um buffer na pilha (`sp+0x10`) que a nossa
implementação nunca escreve. Não corrigi: o slot veio de outro título, com forma
de chamada diferente, e escrever um valor adivinhado ali faz o convidado
desreferenciar um ponteiro inventado.

### Modo do processador: correto por acidente, agora correto de propósito

O `zeebx` também ajustou o processador para modo usuário. Varri os 62 títulos:
**um** lê o CPSR e testa os bits de modo — `chessbots`, o título do container
SWVARC, motor da SuperScape. Em `chessbots.mod 0x0019a870`:

```text
mrs  r0, cpsr
tst  r0, #0xf                  ; bits baixos do modo
bxeq lr                        ; modo usuário -> sai em segurança
mrc  p15, #0, r1, c2, c0, #0   ; senão: lê o TTBR0 e caminha na MMU
```

O módulo só caminha na tabela de páginas se achar que está em modo privilegiado.
Nosso `Reset()` fazia `cpsr_ = 0`, e **zero não é um modo ARM** (User `0x10`,
FIQ `0x11`, IRQ `0x12`, Supervisor `0x13`, Abort `0x17`, Undefined `0x1B`,
System `0x1F`).

**Isto não conserta sintoma nenhum**, e está escrito no commit: `0x10 & 0xf == 0`,
igual a zero, então o `chessbots` já tomava o caminho seguro. Medido antes e
depois: 1015 cores nos dois casos. A mudança existe para o valor deixar de ser
impossível — um convidado que teste com `and #0x1f; cmp #0x10` veria a diferença,
e nenhum dos 62 faz isso hoje.

### `GetDestination` sem `AddRef`: defeito real, consequência nenhuma

O código do próprio SDK da Qualcomm (`utgifviewer.c`, `UTest_Enter`) prova a
convenção: `pib = IDISPLAY_GetDestination(...)` seguido de `IBITMAP_Release(pib)`,
sem `AddRef` no meio — **as duas** funções entregam uma referência que o chamador
possui. Nós devolvíamos o ponteiro puro.

Medido com um contador real instalado no bitmap do dispositivo (`ZEEB_LOG_BMPREF`):
**82 movimentos, todos `Release`, zero `AddRef`, contador terminando em −81.**
Depois da correção: 84/84, zero negativos.

**Mas o critério de aceitação não foi atingido**, e isso fica escrito: A/B com o
mesmo binário (`ZEEB_NO_BITMAP_ADDREF=1`) produz capturas **byte a byte
idênticas** — mesmo md5. A razão é arquitetural: o modo de falha descrito no
`zeebx` precisa de um contador que destrua o objeto em zero e de uma lista de
livres que recicle o endereço. O nosso bitmap de dispositivo é imortal, em
endereço fixo. O excesso de `Release` era real e não tinha consequência.

### Módulos de extensão: o `a3d` passou a rodar

O `zeebx` carrega módulos de extensão do pacote. Medido aqui: o `a3d`
(Action Hero 3D, `mod/274259`) pede a classe `0x010292c3` e recebe
`ECLASSNOTSUPPORT`. O manifesto `mif/12875.mif` do módulo `imicro3d.mod`
**declara exatamente essa classe**. Dois fatos independentes se encontrando.

O mecanismo do BREW: um `.mif` **sem registro de applet** é de extensão, e o
registro de 8 bytes dele diz que classe fornece. Quando o `ISHELL_CreateInstance`
não conhece a classe, o carregador sobe o `.mod` da extensão, chama o
`AEEMod_Load` dela e depois o `IModule::CreateInstance` — o objeto que volta é
código do jogo rodando de verdade, não uma interface nossa.

Resultado, com A/B pelo mesmo binário (`ZEEB_NO_EXTENSION=1`):

| | sem extensão | com extensão |
|---|---|---|
| janela | 2 cores | 6 cores |
| FBO | nunca chegou ao tick 30 | 3 cores |
| subsistemas | nenhum | `FILEMGR` ×93, `MEDIAADPCM` ×22, `THREAD`, `HEAP`, `GRAPHICS`, `MEMASTREAM` |
| tela | branca | polígono desenhado |
| som | mudo | música tocando |

O áudio é a confirmação mais forte e veio de fora do emulador: o dono do projeto
reconheceu a música do jogo de outro ambiente. Nenhuma contagem de pixel prova
que um codec funciona; ouvir prova.

**Os dois métodos de captura discordam na magnitude** (6 cores na janela contra 3
no FBO), então isto entra como "progrediu e está desenhando", **não** como "a tela
do jogo funciona".

### Kingdom Hearts: reproduzível agora, e progrediu sem desenhar

O `swv21brew.mod` (extensão da SuperScape) **não existia na nossa cópia do NAND**.
Com o pacote completo que o dono do projeto forneceu — uma versão de BREW de
telefone injetada, não uma build de Zeebo — ele passou a ser reproduzível:

```text
cls=0x0102bbfc -> EXTENSAO OK (obj=0x803009e0)
```

Ele recebe o objeto e passa a executar 10 ticks com código da extensão; o
controle sem extensão não executa nenhum. **A tela continua branca** nos dois
casos. Falta a segunda classe que ele pede, `0x0100a004`, que nenhum `.mif` do
corpus fornece — provavelmente classe de firmware, não de extensão. Não foi
inventado *stub* para ela.

Isto também corrige uma afirmação minha anterior: eu havia escrito que o
`swv21brew` "não existe". Ele existe; **não estava na nossa mídia**. A diferença
importa.

### Configuração por jogo na GUI: o `cnk2` subia morto

Medido: subindo o `cnk2` exatamente como a GUI subia, sem orçamento de passos,

```text
warning: exceeded 64000000 steps without returning -- aborting this call
CreateInstance did not produce a trustworthy applet pointer -- stopping.
```

Com o orçamento que ele precisa (186486543, medido com `ZEEB_LOG_STEPS`) ele
roda. O emulador já honrava `ZEEB_MAX_STEPS`; faltava a GUI ter de onde tirar o
número. O manifesto agora aceita configuração por jogo, e a forma antiga
continua valendo:

```json
"274214": 17308036
"274259": { "clsid": 17308016, "max_steps": 186486543,
            "env": { "ZEEB_GL_SOFT": "1" } }
```

Verificado de ponta a ponta dirigindo a GUI (Xvfb + `xdotool`):

```text
[gui] lancando Crash Bandicoot Nitro Kart 3D (pasta 274214, clsid 0x01081984)
[gui]   env: ZEEB_MAX_STEPS=186486543
[title] Zeebulator - cnk2
"exceeded 64000000": 0 ocorrências
337 ticks
```

Em título sem medida (`activitycenter`) o log mostra `env: (vazio)`: a GUI não
inventa orçamento. Só `cnk2` e `fifa09` têm medida, e só eles entraram.

**O que não foi resolvido**: a emulação continua rodando **fora** do processo da
GUI, então abre uma segunda janela em vez de o jogo aparecer dentro dela. Isso é
a Fase 2 da GUI e não foi tocado.

### Um erro de medição meu, que vale registrar

Ao verificar o `cnk2` pela GUI, vi `exceeded 64000000` logo depois da linha de
lançamento e concluí que a flag não chegava. Estava errado em dois níveis: a
linha `[gui] lancando` é impressa **antes** do `Start`, então aparece mesmo
quando o lançamento é **recusado**; e a recusa só ia para a barra de status, nunca
para o stderr. O aviso que eu li era do processo **anterior**. Duas correções
saíram daí: o log de argv e ambiente, e `[gui] RECUSADO: ...` no stderr.

É a quarta vez nesta base em que o defeito estava no instrumento, não no
emulador. Registro porque o padrão se repete: quando uma medição contradiz a
expectativa, o instrumento é o primeiro suspeito.

### Desempenho: a conversão do readback era o gargalo, e não a leitura do quadro

A pendência no roadmap dizia "suspeitos: `glReadPixels` por quadro (640×330 RGBA
= 845 KB) e 211.200 `Memory::Write16`". Instrumentei os dois **separadamente**
antes de mexer em qualquer um (`ZEEB_PROF_READBACK=1`):

```text
[prof] 90 leituras: glReadPixels=31286 us  conversao=187227 us  (86% na conversao)
```

O `glReadPixels` ficava com 14%. O custo não era ler o quadro do host: era gravar
211.200 halfwords na memória do convidado, e não pelo cálculo de cor — que é
trivial — mas por um `unordered_map` e **duas chamadas de hook por pixel**.

`Memory::WriteBlock16` faz **um** lookup por trecho contíguo, gravando direto no
buffer da página. A memória do convidado é esparsa por página de 4 KiB, então um
bloco atravessa quantas páginas quiser. No laço quente, a conversão passa a ser
feita por linha e a linha inteira vai numa gravação: **330 gravações por quadro
em vez de 211.200**. A fórmula de conversão não foi tocada, de propósito: não era
o gargalo, e mexer nela mudaria cor sem ganho.

| | antes | depois |
|---|---|---|
| FPS médio | 16,31 | **33,38** |
| FPS mínimo | 4,7 | 31,0 |
| custo da conversão | 86% | 17–21% |
| total por leitura | ~2123 µs | ~1331 µs |

**Correção, não "parece igual"**: a captura de tela é idêntica antes e depois —
275 cores, com o top-3 nas mesmas contagens (105840, 48196, 45241 contra 45227; a
diferença de 14 px é o contador de FPS desenhando outro número).

Os hooks continuam sendo chamados, uma vez por trecho e com o comprimento: a
checagem de watchpoint testa **sobreposição de faixa**, então uma chamada com o
trecho inteiro acerta os mesmos watchpoints e reduz o spam do log.

### O que o `zeebx` deu de útil mesmo sem transferir

Em nenhum dos casos o trabalho foi perdido, porque o valor dele está em apontar
onde olhar. As três lacunas acima viraram, cada uma, um diagnóstico medido
nosso — e duas viraram correção (eixos e CPSR).

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

> **Validade das linhas.** Esta tabela é qualitativa e vem de ciclos
> anteriores. A medição numérica atual dos 62 títulos está na seção
> "Censo de imagem do corpus — 62 títulos, medição de 2026-09-12", com captura dupla
> (janela e FBO) e veredito explícito por título. Onde as duas divergirem, vale
> o censo — ele é reprodutível por `testkit/smoke_now.py`.

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
| `274755` | *Z-Wheel (Menu)* | Rocket Mobile / Tectoy | **Renderiza**: telas 2D de abertura (verde 5.130 / amarelo 1.146 / azul 5.203 px nos 8 s iniciais) e palco 3D (149.824 px pretos a partir dos 9 s). O bloqueio antigo do z-pad (`EUNABLETOLOAD` 6) **não ocorre mais**; restam 3 erros do guest: IDOWNLOAD (2 linhas encadeadas) e `AEECLSID_LCT_SIMCARDCTL`. Defeito conhecido: cores do palco com **R e B trocados** no decode ATITC. | Ver "Z-Wheel: estado atual medido" abaixo |
| `277495` | *Opera Mini (reksio)*| Opera Software | Loop de eventos ativo (Requer stack de sockets/rede) | Fornecer bridge HLE para sockets TCP/IP |

---

## Censo de imagem do corpus — 62 títulos, medição de 2026-09-12

**Método.** Xvfb `:90` isolado, janela X real, 22 s por título, sem entrada do
jogador. Cada título é capturado por **dois** caminhos independentes: a janela
raiz (`import -window root`) e o FBO do host (`ZEEB_SHOT_EXIT`, que passa por
`Sdl2UnifiedBackend::CaptureScreenshot`). Harness: `testkit/smoke_now.py`;
resultados em `testkit/census_now.jsonl`.

**Por que dois caminhos.** Medindo só a janela, três títulos apareciam como tela
vazia enquanto tinham conteúdo real no FBO — o `abd` mede **2 cores na janela e
1.957 no FBO**. Um veredito de "não renderiza" tirado só da janela mandaria
alguém caçar bug de renderização onde ela funciona. A divergência **não é
universal**: em Tennis, Peteca, Zeeboids e Volley os dois métodos batem
exatamente. Por isso o veredito só é afirmativo quando os dois concordam.

**O que esta tabela NÃO mede.** Jogabilidade. Nenhum destes títulos recebeu
entrada do jogador nesta bateria. Contagem de cores mede imagem; um título com
77 mil cores pode não responder a comando nenhum.

| veredito | títulos |
|---|---|
| renderiza e apresenta | 20 |
| renderiza, não apresenta | 3 |
| vazio (os dois concordam) | 33 |
| morto | 4 |
| indefinido / sem captura | 2 |

Contra o censo anterior (`testkit/census62.jsonl`): **33 melhoraram, 2 pioraram,
27 iguais**. Mortos caíram de 30 para 4.

### Duas regressões, com causas diferentes

- **`abd`**: registrado como "7.910 → 1.957". **NÃO É REGRESSÃO — é defeito de
  amostragem desta tabela.** Medido em 2026-09-13, com série temporal e com
  bisseção nos dois binários (ontem `f87e8bb` e hoje `HEAD`): o MESMO execução
  passa por 37 → **7.910** → **2** → **1.957** cores, e os dois binários dão a
  MESMA sequência, nos mesmos instantes, com os mesmos `exceeded=2` e ~1.530
  ticks. O censo antigo amostrou a janela no pico da animação (7.910); o censo
  novo amostrou depois (2 na janela, 1.957 no FBO). **Comparar instantâneos de
  uma sequência como se fossem o mesmo instante foi o erro**, e é o mesmo tipo de
  defeito de instrumento que já apareceu três vezes nesta base.
  O que continua verdadeiro e é o problema real: a animação trava. Dois callbacks
  de timer estouram o orçamento (`exceeded 64000000 steps without returning`) e o
  jogo congela num quadro estático de 1.957 cores, depois de ficar em 2.
  Aumentar o orçamento para 1G **não** resolve — zera as mensagens de estouro e
  deixa a tela em 2 cores de vez. Ou seja: não é "só aumentar o budget".
  E a fonte, medido letra por letra no primeiro quadro (`LOADING...`): as 7 letras
  L-O-A-D-I-N-G estão corretas, sem glifo substituto. A suspeita de "fonte virou
  bolinha" NÃO se confirma nesse instante.
- **`torkandkral`**: 452 → 2 nos dois métodos. Não renderiza. Regressão mais
  profunda, ainda sem causa isolada.

### `abd`: o travamento da animação, com o laço localizado

Fecha o diagnóstico deixado em aberto acima ("duas regressões"): a de `abd` **não
era regressão**, e o que existe de real é um travamento com mecanismo medido.

**Onde param.** A animação da intro vai de 37 → **7.910** cores, cai para **2** e
congela em **1.957**. No log, dois eventos:

```text
warning: exceeded 64000000 steps without returning -- aborting this call
timer callback did not complete trustworthily (wandered=0 exceeded=1)
```

Logo antes deles, o próprio convidado imprime `build menu 7-2`,
`Sources\VM_menugame.c:772` e `build menu end` — ou seja, **o jogo termina de
construir o menu e só então estoura**.

**Onde ele gira** (`ZEEB_SPIN_PROFILE=1`):

```text
pc=0x00105dc8 off=0x00005dc8 hits=1114
pc=0x0011535c..0x00115384      hits=1110   (sequência de PCs)
pc=0x00115aa0                  hits=1110
pc=0x001156d4                  hits=1110   (último BL antes do giro)
loop PC span: 0x00105510-0xf0001bf0, distinct=226
```

O topo da faixa é um **endereço de trap** (`0xf0001bf0`), então o laço atravessa o
HLE. Nomeando os traps (`ZEEB_LOG_BREW=1`) e contando por 34 s:

| índice | slot | rótulo | chamadas |
|---|---|---|---|
| 1788 | 107 | `DrawGeometry` | **367.133** |
| 1735 | 54 | `wall-cycle-slot54` | 367.132 |
| 1714 | 33 | `SelectTexture/consume-cmdlist` | 366.848 |
| 1781 | 100 | `wall-cycle-slot100` | 366.834 |

Quatro slots em lockstep, ~367 mil vezes cada em 34 s — cerca de **10.800
chamadas por segundo**. É este laço que estoura o orçamento.

**O laço é uma lista circular.** O slot 33 recebe um cursor em `r2`. Sequência
medida (`ZEEB_WALL_DUMP=1`):

```text
call#5:  0x80323f84
call#6:  0x80364430
call#7:  0x80310a7c
call#17: 0x80323f84   <- repete
call#18: 0x80364430   <- repete
call#19: 0x80310a7c   <- repete
```

**Três nós, repetindo para sempre.** A varredura do descritor (`cursor - 0x30`)
acha o link de sucessor em `desc+0x00` e `desc+0x2c`, ambos apontando para
`0x80310a7c`.

**Isto não é descoberta nova, e o trabalho anterior precisa do crédito.**
`research/sources/2026-09-01_abd-wall-rootcause.md` já investigou este mesmo muro e
nomeou a faixa do laço: `abd.mod +0x5ba0..+0x5e88`. Os três PCs quentes medidos hoje
(`0x00105dc8`, `0x105dcc`, `0x105dd0`) caem **dentro dessa faixa** — confirmação
independente, cinco semanas depois, com outro instrumento.

**Onde as duas medições discordam.** Aquele documento afirma que a callback presa
roda 3.000.000 de passos "sem disparar um único trap HLE", e conclui que o defeito
não é slot faltante. Medido hoje com `ZEEB_LOG_BREW=1`, nas linhas imediatamente
anteriores ao estouro do orçamento:

```text
1735, 1714, 1788, 1781, 1735, 1714, 1788, 1781, 1735, 1714, 1788, 1781, ...
```

Os quatro traps em lockstep, sem variação. **O laço atravessa o HLE.** A conclusão
de fundo dele — o grafo de cena do convidado está mal-formado — continua plausível e
não foi refutada; o que não se sustenta é a evidência específica de "laço sem trap".
Diferença provável: o repro daquele documento usa `ZEEB_MAX_STEPS=3000000`, e um
orçamento apertado aborta numa fase diferente da callback.

**O scaffold não é a causa — e isso foi testado, não suposto.** Os slots quentes
pertencem ao objeto devolvido pelo `QueryInterface` do QEGL para os IIDs
`0x0103d8dd` e `0x0103d8ea`. Os dois emuladores de referência os identificam:

```text
zeebx/src/machine.rs:603   AEEIID_GLES10 = 0x0103_d8dd
zeebx/src/machine.rs:604   AEEIID_GLES11 = 0x0103_d8ea
zeemu/brew/BrewEGL.cpp:542 devolve gles_object_ptr_ para os dois
```

São **IIDs de interface** (GLES10/GLES11), não classes. E o nosso código **já
devolve o objeto GLES11 real** para eles — só que para todo título **exceto o
abd** (`if (!is_abd_title && ...)`), e o comentário de lá os tratava como "duas
ClsIds não identificadas".

A guarda não veio de uma regressão: o commit `b863d4f` a introduziu para dar GLES
real ao Pac-Mania/Peggle e registrou o abd como *mantido* em 36 cores — a hipótese
de dar GLES real ao abd nunca foi medida. Medido agora, com a chave
`ZEEB_ABD_GLES_QI=1`:

| tempo | scaffold (atual) | abd com GLES real |
|---|---|---|
| 6 s | 37 | **2** |
| 20 s | **7.910** | 2 |
| 28 s | 2 | 2 |
| 40 s | **1.957** | 2 |

Com o GLES real o abd fica em **2 cores do início ao fim**: não desenha nada. A
guarda está **certa**, e o scaffold **não** é o que causa o laço — se fosse,
trocá-lo pelo objeto real teria melhorado.

**O que isso significa.** O scaffold responde 0 em tudo e **nunca consome nem
avança o cursor** — o slot se chama `consume-cmdlist` e não consome nada. A
pergunta em aberto é se o aparelho real marca o fim da lista de um jeito que não
estamos honrando, ou se o consumo é que faz o jogo sair. **Não foi mexido**: mudar
o retorno deste slot no escuro é exatamente o tipo de palpite que esta base
proíbe, e o laço é circular, então devolver o sucessor não o encerraria sozinho.

**Aumentar o orçamento não resolve.** Medido com `ZEEB_MAX_STEPS=1000000000`:
zera as mensagens de estouro e deixa a tela em **2 cores de vez** (de t=28 até
t=52). Ou seja, o orçamento muda o sintoma sem consertar a causa.

**A fonte está correta.** No primeiro quadro (`LOADING...`) as 7 letras L-O-A-D-I-N-G
foram lidas glifo a glifo, sem substituto:

| letra | forma medida (arte ASCII dos pixels) |
|---|---|
| `L` | barra vertical à esquerda + base horizontal |
| `O` | laço fechado |
| `A` | laço com barra horizontal no meio |
| `D` | barra vertical à esquerda + laço à direita |
| `I` | barra vertical + barras em cima e embaixo |
| `N` | barra vertical + diagonal |
| `G` | laço em cima + curva embaixo à esquerda |

### A causa dominante dos mortos é nossa, não dos jogos

**Dos 15 mortos acumulados nas passagens, 11 eram ClsId errado** — o próprio despacho
do jogo recusava a classe (`*ppObj=0`, sem estouro nem desvio). Onze ClsIds foram
corrigidos ao todo (4 + 4 + 3). Dos **4 que restam** (`recklessracing`, `cnk2`,
`fifa09`, `pbc`), as causas são distintas e estão listadas na tabela.
Isso é um limite do emulador, não defeito do título. O `recklessracing` prova:
estoura o orçamento e ainda assim tem **77.021 cores no FBO** — o maior conteúdo
do corpus inteiro. Está renderizando e sendo abortado.

### Defeitos de dados corrigidos no `corpus62.json`

Onze títulos eram medidos com ClsId errado, o que os registrava como mortos.
Cada ClsId novo foi validado por sonda, exigindo `CreateInstance OK` e laço de
eventos ativo. Última rodada:

| título | antes | depois | efeito |
|---|---|---|---|
| `tectoy` (Z-Wheel) | `0x1030c00` | `0x1070798` | morto → 275 cores |
| `nfs` | `0x1020000` | `0x108c0bc` | morto → vivo |
| `zeebopeteca` | `0x1060000` | `0x108ff18` | morto → 729 cores |
| `footparty` | `0x1060000` | `0x108ff19` | morto → vivo |

| `a3d` | `0x10900b9` | `0x1081970` | morto → vivo |
| `heavyweaponbrew` | `0x103081d` | `0x10978a2` | morto → vivo |
| `zenonia` | `0x43bcf` | `0xbf2e2021` | morto → vivo, 56 cores |

O ClsId da `zenonia` (`0xbf2e2021`) não está na faixa BREW comum — foi lido no
próprio despacho do jogo (`ldr r0, [pc]` em `0x1793b4`, retornado em r0 e
comparado em `0x10217c`), depois de três tentativas erradas com decimais
mal convertidos. Fica o registro do método, não só do valor.

### Caso indefinido, deliberadamente não classificado

`chessbots` mede 1.015 cores na janela e 1 no FBO — direção inversa da
divergência esperada. A hipótese mais provável é que o `ZEEB_SHOT_EXIT` capture
depois do contexto GL ser destruído, ou seja, defeito da instrumentação e não do
emulador. Fica sem veredito até ser medido.

### Tabela por título

Ordenada por veredito e depois por conteúdo. "FBO" e "janela" são contagens de
cores distintas; "antes" é o censo anterior.

| título | veredito | FBO | janela | antes | delta | observação |
|---|---|---|---|---|---|---|
| `zeeboids` | renderiza e apresenta | 14763 | 14763 | 2 | +14761 |  |
| `ridgeracer` | renderiza e apresenta | 9476 | 9477 | 1 | +9476 |  |
| `AirRacez` | renderiza e apresenta | 5505 | 5505 | 2 | +5503 |  |
| `Bajaz` | renderiza e apresenta | 5505 | 5505 | 2 | +5503 |  |
| `JetBoardz` | renderiza e apresenta | 5505 | 5505 | 2 | +5503 |  |
| `Rolimaz` | renderiza e apresenta | 5505 | 5505 | 2 | +5503 |  |
| `alpineracerex` | renderiza e apresenta | 2894 | 2920 | 1 | +2919 |  |
| `ironsight` | renderiza e apresenta | 766 | 767 | 728 | +39 |  |
| `zeebopeteca` | renderiza e apresenta | 729 | 729 | 1 | +728 |  |
| `allstarcards` | renderiza e apresenta | 634 | 634 | 2 | +632 |  |
| `zeebotennis` | renderiza e apresenta | 611 | 611 | 1 | +610 |  |
| `toyraidzeebo` | renderiza e apresenta | 350 | 355 | 1 | +354 |  |
| `tectoy` | renderiza e apresenta | 275 | 275 | 1 | +274 |  |
| `ddragonz` | renderiza e apresenta | 262 | 262 | 262 | +0 |  |
| `game` | renderiza e apresenta | 79 | 79 | 1 | +78 |  |
| `zenonia` | renderiza e apresenta | 56 | 56 | 1 | +55 |  |
| `quake` | renderiza e apresenta | 4 | 4 | 1 | +3 |  |
| `bio4_brew` | renderiza e apresenta | 3 | 3 | 1 | +2 |  |
| `heavyweaponbrew` | renderiza e apresenta | 3 | 3 | 1 | +2 |  |
| `pacmania` | renderiza e apresenta | 3 | 3 | 3 | +0 |  |
| `abd` | renderiza, não apresenta | 1957 | 2 | 7910 | -5953 |  |
| `gof` | renderiza, não apresenta | 4 | 1 | 1 | +3 |  |
| `nfs` | renderiza, não apresenta | 4 | 2 | 1 | +3 |  |
| `chessbots` | indefinido (captura) | 1 | 1015 | 1 | +1014 |  |
| `activitycenter` | sem captura de FBO | — | 2 | 2 | +0 |  |
| `a3d` | vazio | 2 | 2 | 1 | +1 |  |
| `alice` | vazio | 2 | 2 | 2 | +0 |  |
| `asq` | vazio | 2 | 2 | 1 | +1 |  |
| `baddudes` | vazio | 2 | 2 | 2 | +0 |  |
| `bjt` | vazio | 2 | 2 | 1 | +1 |  |
| `Boiaz` | vazio | 2 | 2 | 2 | +0 |  |
| `brainchallenge` | vazio | 2 | 2 | 1 | +1 |  |
| `cninja` | vazio | 2 | 2 | 2 | +0 |  |
| `darkseal` | vazio | 2 | 2 | 2 | +0 |  |
| `dodgeball` | vazio | 2 | 2 | 1 | +1 |  |
| `footparty` | vazio | 2 | 2 | 1 | +1 |  |
| `funsoccer` | vazio | 2 | 2 | 1 | +1 |  |
| `game` | vazio | 2 | 2 | 1 | +1 |  |
| `hbarrel` | vazio | 2 | 2 | 2 | +0 |  |
| `imicro3d` | vazio | 2 | 2 | 1 | +1 |  |
| `karnovr` | vazio | 2 | 2 | 2 | +0 |  |
| `magdrop3` | vazio | 2 | 2 | 2 | +0 |  |
| `peggle` | vazio | 2 | 2 | 1 | +1 |  |
| `prey3d` | vazio | 2 | 2 | 1 | +1 |  |
| `quake2brew` | vazio | 2 | 2 | 1 | +1 |  |
| `reksio` | vazio | 2 | 2 | 1 | +1 |  |
| `rmp` | vazio | 2 | 1 | 1 | +1 |  |
| `rocketweb` | vazio | 2 | 2 | 1 | +1 |  |
| `rt2` | vazio | 2 | 2 | 1 | +1 |  |
| `spinmast` | vazio | 2 | 2 | 2 | +0 |  |
| `strhoop` | vazio | 2 | 2 | 2 | +0 |  |
| `supbtime` | vazio | 2 | 2 | 2 | +0 |  |
| `tekken2` | vazio | 2 | 2 | 1 | +1 |  |
| `torkandkral` | vazio | 2 | 2 | 452 | -450 |  |
| `wizdfire` | vazio | 2 | 2 | 2 | +0 |  |
| `zeebo_app` | vazio | 2 | 2 | 1 | +1 |  |
| `zeebovolley` | vazio | 2 | 2 | 2 | +0 |  |
| `zumar` | vazio | 2 | 2 | 1 | +1 |  |
| `recklessracing` | morto | 77021 | 1 | 1 | +77020 | estouro de 64M passos |
| `cnk2` | morto | — | 1 | 1 | +0 | estouro de 64M passos |
| `fifa09` | morto | — | 1 | 1 | +0 | estouro de 64M passos |
| `pbc` | morto | — | 1 | 1 | +0 | estouro de 64M passos |
---

## Z-Wheel: estado atual medido

Título: `mod/274755/tectoy.mod`, ClsId `17237912`, applet `0x01070798`.
Ambiente: Wayland (`SDL_VIDEODRIVER=wayland`, `WAYLAND_DISPLAY=wayland-0`),
vídeo e áudio SDL reais. Medições de 27 s, captura pelo canal de controle.

### Linha do tempo da tela (contagem de pixels, não impressão visual)

| t | cores | verde | amarelo | azul | branco | preto |
|---|---|---|---|---|---|---|
| 3–7 s | 256 | **5.130** | **1.146** | **5.203** | 266.177 | 0 |
| 9–19 s | 274 | 1 | 440 | 9.322 | 107.752 | **149.824** |

Verde, amarelo e azul simultâneos nos primeiros segundos são a assinatura da
bandeira na abertura. Antes deste ciclo, esses mesmos instantes mediam
`cores=1` com 307.200/307.200 pixels brancos.

### O que passou a funcionar, e por quê

1. **Recursos de imagem existem de verdade.** `ISHELL_LoadResObject` devolvia um
   objeto falso único para todo recurso, com `GetInfo` mentindo 640×480 e `Draw`
   que não desenhava nada. Agora cada recurso decodificado tem objeto e buffer
   próprios. Decodificados na Z-Wheel: `opening_low.gif` 640×480, PNG 576×313
   (id 5008) e BMP 214×34 (id 5007).
2. **Quem desenha o conteúdo do widget somos nós.** O jogo entrega a imagem por
   `IInterfaceModel::SetIPtr` e **nunca** chama `IImage::Draw` — num BREW real
   quem pinta o conteúdo é a biblioteca de widgets do aparelho. Medido: slot 12 =
   `GetModel(AEEIID_IInterfaceModel 0x0101593c)` e, no objeto devolvido, slot 5 =
   `SetIPtr(pIImage, AEEIID_IImage 0x01013110)`. Nosso slot 5 tratava isso como
   "adicionar filho", então a imagem ficava guardada e ninguém pintava.
3. **Duas colisões de endereço de objeto HLE**, ambas encontradas por medição e
   ambas capazes de parecer defeito de CPU: os objetos `IImage` nasciam em
   `0x8006C000`, que **é** `kWidgetVtable`; e o objeto de fallback nascia no
   mesmo endereço do primeiro recurso, fazendo todo recurso não decodificável
   devolver o GIF de abertura (pintado 69× sobre a tela inteira a partir de
   `DrawRollerExt`, `lr=0x0011ff28`).

### Defeito conhecido e não corrigido: canais trocados no ATITC

As três texturas ATITC `512×256` do palco decodificam com **124.905 pixels
alaranjados** cada (critério `R>B+25`), dominantes `(239,138,41)` e
`(231,134,41)`. A captura de referência antiga `zw_shot_exit.ppm` tinha
**211.136 pixels azulados** e **zero** alaranjados, dominante `(41,142,206)`.
É o mesmo pixel com **R e B trocados**.

A cadeia inteira foi descartada por medição, uma etapa de cada vez:

| etapa | veredito |
|---|---|
| texturas não comprimidas (`glTexImage2D`, `GL_RGB`) | corretas, zero pixel alaranjado |
| decode ATITC (`glCompressedTexImage2D`) | **é aqui** |
| readback do host (`ZEEB_EGL_DUMP`) | já chega alaranjado — defeito é anterior |
| conversão RGBA→RGB565 (`SyncSurfaceColorBuffer`) | correta (`R` vem de `rgba[+0]`) |
| imagens novas (GIF/PNG/BMP) | corretas — PNG casa em RGB com erro **2,28/255** |

**Por que não foi corrigido:** `core/loader/atitc.cpp` tem testes que fixam a
ordem atual e o formato é usado por outros títulos do corpus. Inverter os canais
sem antes provar qual ordem é a verdadeira — com decodificador independente ou
com a arte equivalente não comprimida — só mudaria o defeito de lugar. É
exatamente a armadilha de fixture que este documento cobra dos outros casos.

### Outras pendências medidas

- **Posição das imagens 2D**: tudo é desenhado em `(0,0)` porque
  `widget_geometry` não tem entrada para esses objetos. Cor e orientação estão
  certas; o lugar não. As posições chegam por `EVT_WDG_SETPROPERTY` (0x801) com
  wParam `0x152`, `0x153`, `0x130`, `0x140`.
- **`class_id = -1` é do próprio jogo**, não nosso: `mvn r2, #0` hardcoded no
  chamador `0x127c1c`. A consulta principal da roda devolve 0 linhas também
  contra o banco real, e por isso os nomes dos itens ficam vazios em
  `item + 0x114`.
- **`AEECLSID_LCT_SIMCARDCTL` (0x01006c01)** não é implementada; o jogo trata a
  falha (retorna `0x27`) e segue para o menu.
- **Entrada por HID**: as teclas chegam ao `HandleEvent` e voltam 0. O jogo usa o
  caminho do joystick (`Joystick.c:157 1 Joysticks connected`,
  `Joystick.c:183 No keyboard reported`).
- **Desempenho**: 12 FPS medidos, esperado 30/60.

### Erros do guest que restam (3, contra o bloqueio total anterior)

```
Unable to create instance of IDOWNLOAD in ShopAction_Init
Unable to init ShopAction in Gamelib_CheckPreLoaded
ERROR: Unable to create instance of AEECLSID_LCT_SIMCARDCTL, cannot do SIM check
```

---

## Z-Wheel: histórico de desbloqueio (ciclo anterior — bloqueio já superado)

> **Aviso de validade.** Esta seção descreve o ciclo em que a Z-Wheel ainda
> parava no formulário do z-pad com `EUNABLETOLOAD` (6). Esse bloqueio **não
> existe mais** — verificado nos logs do ciclo atual, onde a mensagem
> `Couldn't create z-pad instruction form` não aparece nenhuma vez. A análise
> foi mantida porque documenta o método que levou à correção, não o estado
> presente. Para o estado presente, ver "Z-Wheel: estado atual medido".

Titulo: `mod/274755/tectoy.mod`, ClsId `17237912` (`0x0107...`), applet
`0x01070798`. Execucoes daquele ciclo: passivas (`ZEEB_DISABLE_INPUT=1`), sem
JIT, com video SDL real e `DISPLAY=:0`. (O ciclo atual roda em Wayland:
`SDL_VIDEODRIVER=wayland`, `WAYLAND_DISPLAY=wayland-0`.)

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

### Checklist do documento da roda contra o que existe aqui

Documento: `18-a-roda-da-z-wheel.md` (sha `57a6db1375e8`, lido integralmente).
Cada linha foi conferida no codigo e, quando possivel, medida em execucao.

| Item do documento | Estado no Zeebulator |
|---|---|
| §2.1 evento para a propria classe **dentro do CreateInstance** | feito nesta sessao: entrega real ao `HandleEvent`, applet resolvido do `ppObj` |
| §2.2 SQLite de verdade | ja existia (`SqlHle`, SQLite real ligado, dialeto do jogo) |
| §2.3 catalogo gravavel (copia de perfil) | ja existia (`.sqldb/` copiada, nunca escreve na midia da ROM) |
| §2.4 esquema do `tt_dlqueue.db` | ja existia (`DBINFO` + `DLITEMINFO` ao abrir) |
| §2.5 `preloaded.cfg` existe e vazio | feito nesta sessao; `fontsize.map` ausente confirmado por varredura da NAND |
| §3 retorno invertido do acessador (`!= 0` = sucesso) | ja existia |
| §3.1 itens tipados (`>= 0x5000` objeto, `< 0x5000` numero) | ja existia; nesta sessao passou a ser por `(this, id)` |
| §3.2 familia de classes de widget | ja registrada (`e05 e14 e19 e26 e2a e36 e3f e47` + `e51`) |
| §4 recusar `0x01006c01` de proposito | fazemos isso; o log mostra a recusa e o fluxo segue |
| §4 pbuffer com o tamanho dos atributos | implementado (640x330 medido; limites 640x480) |
| §4 `eglGetColorBufferQUALCOMM` devolve pixel cru | parcial: ponteiro RGB565 real; falta readback do GL host |
| §5.1 instrumentar o desfecho de cada callback | feito nesta sessao (`ZEEB_LOG_WIDGET_ALL`) |
| §5.2 slot 13 = `CreateCompatibleBitmap(&bmp,w,h)` | corrigido: objeto/pixels/geometria por superficie, arenas limitadas |
| §5.3 acessador chamado com endereco de filho no lugar do seletor | ja existia |
| §5.4 slot 17 aceito (e segurar a fonte) | aceito; **nao** seguramos referencia da fonte |
| §5.5 slots 4 e 16 devolvem o registro anterior | feito nesta sessao |
| §5.6 bitmap do display nao volta para a lista de livres | **aberto / nao verificado** |
| §5.7 eventos do root devolvem "nao tratei" (0x101, 0x7b0a, 0x7b0e, 0x7b0f) | feito nesta sessao (incluindo o `0x7b0e`) |
| §6.1 desenho registrado e explicito; tela atual = mais novo com filhos | **aberto** (nao ha desenhador de widget aqui) |
| §6.2 bloco de posicao do slot 5 | feito nesta sessao, e os valores batem com o documento |
| §6.3 passo de lista nunca zero | ja existia (18) |
| §6.3 `GetExtent` do texto | parcial (a fonte devolve 640x50 fixo), **aberto** |
| §7 teclas `0xe033`/`0xe034` chegando como `EVT_KEY` ao applet | ja existia e confere (`AVK_LEFT`/`AVK_RIGHT` = `0xE033`/`0xE034`) |

### O que o documento nao explica do nosso caso

O erro que trava a Z-Wheel aqui e `EUNABLETOLOAD` (6) na construcao do
formulario do z-pad. O documento registra esse mesmo ponto com erro **20**
(`EUNSUPPORTED`, a classe `0x01028e36` recusada). Aqui a classe esta registrada,
entao o nosso 6 tem outra causa -- a cadeia medida acima. Mesmo sintoma, causa
diferente, e a leitura do codigo do guest (nao do documento) e que resolve.

---

## 3-Legged 5-Why + espinha de peixe: por que o formulario do z-pad nao monta

**Fato observado (medido, nao inferido).** Execucao passiva da Z-Wheel
(`tectoy.mod`, ClsId 17237912), 50 s, sem entrada:

```
[sendevent] clsApp=0x01070798 evt=0x7b0a wParam=10 dwParam=0x0038ffe0
            -> applet HandleEvent=0x00100e1c devolveu 1; resposta=0x00000000
[guest] Tectoy.c:743  Unable to launch z-pad intructions form: 6
[guest] Tectoy.c:760  Couldn't create z-pad instruction form (6)
```

Cadeia estatica correspondente (trace de execucao em 0x17f5e0-0x17f840):

```
0x17f6f8  ldrsh r1,[r5,#0x24]        ; id do recurso = 1178 (= "Z-Pad" em tectoyli.brf)
0x17f700  bl 0x179518                ; (shell, 1178)
0x179528    bl 0x1785a4              ; -> SendEvent(0x7b0a, wParam=10, &resposta)
0x17952c    movs r4,r0 ; beq 0x17956c ; resposta == 0 -> devolve 0
0x17f70c  mov r4,#6                  ; EUNABLETOLOAD (AEEError.h)
```

### As tres pernas, e o que cada uma responde

- **Perna M (mecanismo)**: o que o codigo faz, instrucao por instrucao.
- **Perna C (contrato)**: o que o SDK/BREW define que deveria acontecer.
- **Perna P (processo/metodo)**: por que este projeto nao viu o problema antes.

### Round 1 -- o sintoma

| Perna | Resposta |
|---|---|
| M | O wrapper `0x179518` devolve 0 e o chamador traduz esse 0 em `EUNABLETOLOAD` (6) |
| C | O wrapper so devolve 0 quando a resposta do evento 0x7b0a e nula |
| P | Nos tratavamos esse mesmo ponto como "resolvido" porque o boot seguia |

### Round 2 -- por que o wrapper devolveu 0

| Perna | Resposta |
|---|---|
| M | `SendEvent(0x7b0a, wParam=10)` voltou com `resposta=0x00000000`, e o applet devolveu **1** ("tratei") ao mesmo tempo |
| C | Quem responde 0x7b0a e o proprio applet, escrevendo o ponteiro pedido no `dwParam`; resposta nula = "nao tenho esse dado" |
| P | Nos substituiamos a resposta do applet por uma constante chumbada (`w==1||w==0xa` escrevia o idioma "pt  "); enquanto isso o boot andava por ficcao, nao por emulacao |

### Round 3 -- por que o applet nao tinha o dado

| Perna | Resposta |
|---|---|
| M | Imediatamente antes da resposta nula o applet abre `tt_prefs.db`, percorre chaves e escreve 0 |
| C | O caminho observado e interno ao applet/SQL; `ISHELL_GetPrefs`/`SetPrefs` seriam outro caminho possivel, mas precisam aparecer no trace para poderem ser culpados |
| P | A primeira analise promoveu proximidade temporal (`OpenDatabase`) e dois stubs existentes (`GetPrefs`/`SetPrefs`) a causalidade sem provar a chamada |

### Round 4 -- a hipotese de GetPrefs foi falsificada

| Perna | Resposta |
|---|---|
| M | `ZEEB_STUB_TRACE=1` registrou 91 amostras de stubs na mesma execucao e **nenhuma** chamada a `IShell::GetPrefs` ou `SetPrefs` |
| C | Ausencia no trace completo da vtable exercitada falsifica a alegacao de que esses slots produziram diretamente o zero desta cadeia |
| P | O artigo anterior dizia "causa-raiz" e so depois admitia que o passo principal era hipotese. Isso estava metodologicamente invertido |

### Round 5 -- o stub realmente exercitado

| Perna | Resposta |
|---|---|
| M | O helper `AEEHelperFuncs::aee_stribegins`, offset `0x1a4`, foi chamado dezenas de vezes enquanto o guest varria chaves no BSS; o stub devolvia 0 em todas |
| C | O SDK e explicito: `AEEStdLib.h:217` declara `boolean (*aee_stribegins)(const char *cpszPrefix, const char *psz)` e `AEEStdLib_static.h:103` repete o prototipo; 1 significa que `psz` comeca com o prefixo. A comparacao sem diferenciar caixa e inferida do par separado `strbegins`/`stribegins`, nao de uma frase explicita da documentacao |
| P | A tabela tinha o nome correto, mas nomear slot nao e implementar slot. `LoggedStub` continuava sendo comportamento falso, apenas mais silencioso |

### Convergencia corrigida

A convergencia publicada antes estava **errada**: `GetPrefs`/`SetPrefs` nao sao a
causa direta porque nao foram chamados. O que agora se pode afirmar e:

> **O bloqueio medido era a resposta nula ao evento `0x7b0a/wParam=10`, dentro
> de um caminho de lookup de dados do applet. Nesse caminho, o unico stub
> fortemente exercitado e semanticamente relevante encontrado foi
> `aee_stribegins`: ele respondia falso para toda chave e podia tornar qualquer
> varredura incapaz de achar `game_id`, `BrowserClassID` e chaves vizinhas.**

Teste falsificavel depois da implementacao correta do helper: uma execucao passiva
unica de 25 s nao repetiu `wParam=10 -> resposta=0`, nao imprimiu as mensagens
`Unable to launch z-pad... (6)`/`Couldn't create... (6)` e `EVT_APP_START`
retornou 1. Isto prova que o fluxo mudou e que o erro imediato desapareceu nessa
execucao. **Nao prova** que o menu ou a roda montaram: nao houve callback slot 16
nem frame diferente de branco comprovado.

### Espinha de peixe corrigida

```
              FORMULARIO DO Z-PAD NAO MONTAVA -> MENU AUSENTE -> QUADRO BRANCO
                                   |
  METODO --------------------------+
    constante chumbada substituindo resposta do guest                 <- mascarou
    GetPrefs promovido a causa sem chamada medida                      <- erro nosso
  LOOKUP/DADOS ---------------------+
    wParam=10 devolvia ponteiro nulo                                   <- medido
    tt_prefs.db era aberto imediatamente antes                         <- correlacao
    aee_stribegins devolvia FALSE para todas as chaves                 <- medido
  IMPLEMENTACAO --------------------+
    helper 0x1a4 tinha nome mas apontava para stub                     <- corrigido
    widgets ainda compartilham um unico objeto                         <- aberto/P0
  INSTRUMENTACAO -------------------+
    stub neutro deixava o guest seguir sem erro de host
    nova execucao removeu erro 6, mas ainda nao provou pixels/slot 16
  CONTRATO -------------------------+
    AEEStdLib.h: prefixo em r0, string em r1, retorno boolean
    GetPrefs/SetPrefs ausentes do trace: hipotese falsificada
```

### O que ainda permanece aberto

A ligacao `aee_stribegins falso -> lookup wParam=10 nulo` tem forte evidencia
mecanica e o erro sumiu depois da correcao, mas uma unica execucao nao fecha toda
a cadeia SQL. O proximo oraculo e registrar as consultas e linhas devolvidas pelo
handler de `wParam=10`. Separadamente, a tela branca continua ligada a infraestrutura de widget. O
singleton entre nove classes e o bitmap compartilhado foram corrigidos; ainda
faltam ownership completo de fonte/modelo, selecao da arvore/tela atual e
readback do pbuffer GL. O callback slot 16 continuou ausente na execucao medida.

> **Atualizacao (ciclo do pipeline de imagem).** Esta conclusao ja nao vale: a
> tela branca acabou. O readback do pbuffer GL foi implementado (FBO proprio,
> `readback=ok` 226/0 contra 0/226 antes), o callback do slot 16 passou a ser
> exercitado e as telas 2D de abertura renderizam. O que restou do diagnostico
> acima e o ownership de fonte/modelo. Ver "Z-Wheel: estado atual medido".


Execucao passiva unica depois das factories (25 s, sem entrada) mostrou
`CreateInstance` distintos para `0x01028e47`, `0x01028e19` e `0x01028e3f`, e o
primeiro `SetHandler` ocorreu em `this=0x86000300`, nao mais no singleton
`0x8006d000`. Ainda houve zero `SetDrawHandler`: identidade foi consertada, mas o
menu completo/OwnerDraw nao esta provado.

### Efeito colateral que vale registrar

O mesmo padrao explica por que este projeto teve, por sessoes seguidas, numeros
de compatibilidade que pareciam bons e jogos que nao apareciam: **substituir a
resposta do guest por uma constante correta faz o boot andar, e o boot andar nao
e prova de que o caminho e o certo.** Aqui a troca da constante pela entrega real
transformou um "boot parcial" silencioso num erro honesto com causa localizada.

---

## Lições Aprendidas na Auditoria de Código

1. **Auto-Citação e Falsa Certeza**: Vários trechos de código continham comentários como "confirmado por disassembly" que,
   ao serem cruzados com o SDK oficial (`AEEStdLib.h`, `AEEIBitmap.h`), revelaram-se suposições incorretas (ex.: `0xdc` ser
   gzip em vez de `memcmp`, ou `ECLASSNOTSUPPORT` ser 20 em vez de 3).
2. **Reentrância Assíncrona no BREW**: callbacks guest disparados de dentro de um trap HLE precisam usar
   `CallArmFunctionPreservingContext`; notificações assíncronas devem ser adiadas. Chamar `CallArmFunction` cru sobrescreve
   PC/LR/registradores da chamada externa. O mesmo vale para comparadores de sort e fontes de unzip.
3. **Isolamento de Estado de Persistência**: Salvar `.userdata` e abrir bancos SQLite dentro do diretório `/media/.../ROMs/`
   contaminava dumps históricos e falhava em mídias somente-leitura. O redirecionamento para caminhos padrão XDG resolve
   a portabilidade. O caminho legado agora e somente origem de importacao; toda escrita futura permanece no XDG.

### Resultado desta rodada de auditoria

Auditoria paralela cobriu ABI SDK, memoria/lifetime, parsers, filesystem, SQL,
midia, EGL/GL e widgets. Achados corrigidos e validados por testes unitarios:

| Severidade | Falha | Estado |
|---|---|---|
| P0 | `LoadResDataEx` tratava retorno como `AEEResult` e ignorava capacidade do buffer | corrigido contra `AEEIShell.h` |
| P0 | vtable `IBitmap` de 64 bytes era reservada com 24 e sobrescrevia o proprio IDIB | corrigido |
| P0 | `IFile::Write` fazia wrap de `position+nWant` e escrevia fora do `std::vector` host | corrigido com aritmetica 64-bit/quota |
| P0 | BAR aceitava wrap da subtabela e `Extract` aceitava `BarEntry` publico fora do arquivo | corrigido |
| P0 | metadata GL invalida produzia vetores menores que o backend lia | validacao de componentes/tipos/stride/count adicionada |
| P0 | uploads GL, midia e gzip aceitavam tamanhos de varios GiB | quotas, produtos checados e falhas de alocacao contidas |
| P0 | chamada guest aninhada no unzip/sort destruia contexto da chamada externa | usa preservacao integral |
| P1 | `IThread::Release` podia liberar duas vezes e ignorava `AddRef` | refcount/lifetime reais |
| P1 | callbacks de stream sobreviviam a `Cancel`/`Release` | ownership e cancelamento implementados |
| P1 | notificacao de midia podia atingir objeto liberado/reutilizado | objeto+geracao validados antes do callback |
| P1 | `.userdata`, savestate e SQLite podiam continuar gravando ao lado da ROM | legado virou import-only; destino sempre XDG |
| P1 | `eglCreatePbufferSurface` e `eglGetColorBufferQUALCOMM` eram stubs | atributos/tamanho/RGB565 implementados |
| P1 | nove classes widget recebiam o mesmo objeto singleton | factories entregam identidade por instancia |
| P1 | slot 13 devolvia o mesmo bitmap vazio | superficies RGB565 independentes e arenas limitadas |
| P1 | OwnerDraw dependia de timer e engolia toda excecao | passe independente, limitado a 60 Hz e log confiavel |

Validacao apos as correcoes de core: **623 testes**, **621 passaram**, **2 foram
pulados** por dependerem do corpus externo (`FufsCorpus` e `SarCorpus`). Nenhuma
bateria de jogos foi usada.

### Pendencias honestas encontradas pela mesma auditoria

- `IDisplay::SetDestination` ainda guarda o bitmap, mas DrawText/DrawRect/BitBlt
  ainda escrevem principalmente no framebuffer de tela; composicao offscreen nao
  esta fechada.
- Widgets ainda precisam ownership completo de fonte/modelo, destruicao recursiva,
  tela raiz atual e pintura tipada de texto/imagem.
- A ABI de `IDisplay::DrawText` diverge de `AECHAR=uint16` do SDK por causa de uma
  observacao narrow em um titulo; precisa virar quirk reproduzivel, nao regra global.
- Mirror/control server ainda le `Memory` de outra thread sem snapshot/lock e pode
  bloquear shutdown em cliente ocioso.
- Desserializadores de Mixer e log de texturas ainda precisam das mesmas quotas ja
  aplicadas a Memory/File/Media.
- `IFileMgr` agora possui todos os 21 slots e falha explicitamente nos oito ainda
  nao implementados; ResolvePath/GetFreeSpaceEx continuam funcionais pendentes.
- EGL pbuffer expoe memoria RGB565 correta, mas backend GL host ainda nao faz
  readback automatico para esse buffer depois de cada draw.

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

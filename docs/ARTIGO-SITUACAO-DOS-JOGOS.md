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
| renderiza e apresenta | 17 |
| renderiza, não apresenta | 3 |
| vazio (os dois concordam) | 29 |
| morto | 11 |
| indefinido / sem captura | 2 |

Contra o censo anterior (`testkit/census62.jsonl`): **33 melhoraram, 2 pioraram,
27 iguais**. Mortos caíram de 30 para 11.

### Duas regressões, com causas diferentes

- **`abd`**: 7.910 → 1.957 no FBO e 2 na janela. Renderiza, mas o conteúdo não
  chega à janela, e também perdeu conteúdo no próprio FBO. Bissectado com
  `ZEEB_NO_RES_IMAGE=1`: o resultado não muda, então **não é** o pipeline de
  imagem deste ciclo.
- **`torkandkral`**: 452 → 2 nos dois métodos. Não renderiza. Regressão mais
  profunda, ainda sem causa isolada.

### A causa dominante dos mortos é nossa, não dos jogos

**Os 11 mortos morrem pelo mesmo motivo: estouro do orçamento de 64 M passos.**
Isso é um limite do emulador, não defeito do título. O `recklessracing` prova:
estoura o orçamento e ainda assim tem **77.021 cores no FBO** — o maior conteúdo
do corpus inteiro. Está renderizando e sendo abortado.

### Defeitos de dados corrigidos no `corpus62.json`

Quatro títulos eram medidos com ClsId errado, o que os registrava como mortos.
Cada ClsId novo foi validado por sonda, exigindo `CreateInstance OK` e laço de
eventos ativo:

| título | antes | depois | efeito |
|---|---|---|---|
| `tectoy` (Z-Wheel) | `0x1030c00` | `0x1070798` | morto → 275 cores |
| `nfs` | `0x1020000` | `0x108c0bc` | morto → vivo |
| `zeebopeteca` | `0x1060000` | `0x108ff18` | morto → 729 cores |
| `footparty` | `0x1060000` | `0x108ff19` | morto → vivo |

`zenonia` continua com ClsId desconhecido: os candidatos testados falharam, e
registrar "desconhecido" é mais honesto que inventar um valor.

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
| `tectoy` | renderiza e apresenta | 275 | 275 | 1 | +274 |  |
| `ddragonz` | renderiza e apresenta | 262 | 262 | 262 | +0 |  |
| `game` | renderiza e apresenta | 79 | 79 | 1 | +78 |  |
| `quake` | renderiza e apresenta | 4 | 4 | 1 | +3 |  |
| `bio4_brew` | renderiza e apresenta | 3 | 3 | 1 | +2 |  |
| `pacmania` | renderiza e apresenta | 3 | 3 | 3 | +0 |  |
| `abd` | renderiza, não apresenta | 1957 | 2 | 7910 | -5953 |  |
| `gof` | renderiza, não apresenta | 4 | 1 | 1 | +3 |  |
| `nfs` | renderiza, não apresenta | 4 | 2 | 1 | +3 |  |
| `chessbots` | indefinido (captura) | 1 | 1015 | 1 | +1014 |  |
| `activitycenter` | sem captura de FBO | — | 2 | 2 | +0 |  |
| `alice` | vazio | 2 | 2 | 2 | +0 |  |
| `baddudes` | vazio | 2 | 2 | 2 | +0 |  |
| `bjt` | vazio | 2 | 2 | 1 | +1 |  |
| `Boiaz` | vazio | 2 | 2 | 2 | +0 |  |
| `brainchallenge` | vazio | 2 | 2 | 1 | +1 |  |
| `cninja` | vazio | 2 | 2 | 2 | +0 |  |
| `darkseal` | vazio | 2 | 2 | 2 | +0 |  |
| `footparty` | vazio | 2 | 2 | 1 | +1 |  |
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
| `a3d` | morto | — | 1 | 1 | +0 | estouro de 64M passos |
| `asq` | morto | — | 1 | 1 | +0 | estouro de 64M passos |
| `cnk2` | morto | — | 1 | 1 | +0 | estouro de 64M passos |
| `dodgeball` | morto | — | 1 | 1 | +0 | estouro de 64M passos |
| `fifa09` | morto | — | 1 | 1 | +0 | estouro de 64M passos |
| `funsoccer` | morto | — | 1 | 1 | +0 | estouro de 64M passos |
| `heavyweaponbrew` | morto | — | 1 | 1 | +0 | estouro de 64M passos |
| `pbc` | morto | — | 1 | 1 | +0 | estouro de 64M passos |
| `toyraidzeebo` | morto | — | 1 | 1 | +0 | estouro de 64M passos |
| `zenonia` | morto | — | 1 | 1 | +0 | estouro de 64M passos |

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

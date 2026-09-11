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
| `274755` | *Z-Wheel (Menu)* | Rocket Mobile / Tectoy | **Boot parcial**: applet, SQLite e widgets iniciais chegam a `EVT_APP_START`; animação, chime, instruções Z-Pad e carrossel não foram comprovados no Zeebulator. | Implementar ISourceUtil/IGetLine e callbacks dos widgets; validar a cadeia de boot |
| `277495` | *Opera Mini (reksio)*| Opera Software | Loop de eventos ativo (Requer stack de sockets/rede) | Fornecer bridge HLE para sockets TCP/IP |

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

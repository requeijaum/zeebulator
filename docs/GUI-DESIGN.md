# Design — GUI do Zeebulator (`new_ez_ui`)

Requisitos em `GUI-REQUISITOS.md`. Este documento explica **como** e **por quê**.

---

## 1. Por que Dear ImGui

Três candidatos foram considerados:

| opção | a favor | contra |
|---|---|---|
| **Dear ImGui** | modo imediato, integra no contexto OpenGL que o emulador já tem, sem árvore de widgets externa, ~20 arquivos vendorizáveis | não é toolkit de aplicação — menus, temas e layout são trabalho nosso |
| **Qt** | toolkit completo, diálogos nativos prontos | dependência grande, licença a considerar, e um segundo modelo de janela sobre o SDL que já usamos |
| **UI própria em SDL** | zero dependência nova | reinventa lista, rolagem, foco, campo de texto — semanas para chegar a algo pior |

A escolha é ImGui porque o emulador já tem um contexto OpenGL vivo
(`Sdl2UnifiedBackend`), então o custo de integração é um *renderer backend*, não
uma aplicação nova. E porque o alvo é uma UI de emulador — poucas telas, densas
em lista e opção — que é exatamente onde o modo imediato brilha.

**Risco assumido e como mitigar:** ImGui não traz o "jeito emulador" de graça.
A seção 3 define o desenho de tela explicitamente, para não virar um amontoado
de janelas flutuantes.

---

## 2. Referências: o que copiar de cada emulador

Padrões observados em emuladores que as pessoas usam de verdade. Copiamos o
padrão, não o código.

| emulador | o que copiamos |
|---|---|
| **Dolphin** | lista com colunas (título, ID, tamanho, país) e **busca que filtra enquanto digita**; duplo-clique inicia; botão de pasta para trocar a raiz |
| **PCSX2 (Qt)** | separação clara **Biblioteca / Configurações**; a configuração não fica escondida em arquivo |
| **DuckStation** | barra de estado do jogo com ações óbvias (pausar, parar, salvar estado) sempre no mesmo lugar |
| **RetroArch** | **playlist**: a lista é um artefato salvo, não algo que se re-descobre a cada abertura |
| **mGBA** | simplicidade: uma janela, menu curto, sem assistente de primeira execução |
| **RPCS3** | quando um título falha, **diz por quê** na própria linha, em vez de só não abrir |

Três lições que viram decisão de design:

1. **A lista é a tela.** Todo o resto é secundário. (Dolphin, mGBA)
2. **Trocar a raiz de jogos é operação de primeira classe**, não configuração
   escondida. Aqui isso pesa mais que nos outros: o NAND é um diretório montado
   que muda de lugar conforme o cartão. (Dolphin, RetroArch)
3. **Falha tem nome.** Nosso caso é pior que o dos outros: um título pode estar
   morto por ClsId errado, e isso não é culpa do jogo. A UI precisa dizer isso.
   (RPCS3, e a lição desta sessão)

---

## 3. Desenho de telas

### 3.1 Biblioteca (tela inicial)

```
+--------------------------------------------------------------------------+
| Zeebulator            [ Biblioteca ]  [ Configuracoes ]        _ □ X     |
+--------------------------------------------------------------------------+
| NAND: /media/rafaelfrequiao/8C5F-19E51/zeebo/ROMs/debug_nand   [Trocar]  |
| [ Buscar: ______________________ ]    63 titulos    [ Re-varrer ]        |
+--------------------------------------------------------------------------+
| TITULO                 PASTA     ASSETS     CLSID        ESTADO          |
| ------------------------------------------------------------------------ |
| Crash Nitro Kart 2     274214    -          0x1081984   OK              |
| Zenonia                277455    bar        0xBF2E2021  OK              |
| Ridge Racer            274802    -          0x1087B73   OK              |
| ...                                                                      |
| Titulo sem mif         270001    -          desconhecido NAO INICIAVEL   |
+--------------------------------------------------------------------------+
| Enter inicia  .  F5 re-varre  .  Setas navegam                           |
+--------------------------------------------------------------------------+
```

Decisões:

- **Uma janela só.** Sem janelas flutuantes acidentais; abas em vez disso.
- **Coluna ESTADO sempre presente**, com os valores: `OK`, `NAO INICIAVEL`
  (com motivo), `SEM CORPUS`. Nunca vazio e nunca só um ícone.
- **Busca filtra a cada tecla**, sem botão de aplicar.
- **A raiz do NAND fica visível na barra**, com o botão ao lado. Caminho longo é
  cortado no meio, não no fim — o fim (`debug_nand`) é o que identifica.

### 3.2 Emulação

A janela do jogo ocupa a área; a barra de estado fica **fora** da imagem, para
não cobrir pixel de jogo:

```
+--------------------------------------------------------------------------+
| Crash Nitro Kart 2    [Rodando]  00:01:23          [Pausar] [Parar]      |
+--------------------------------------------------------------------------+
|                                                                          |
|                        (quadro emulado 640x480)                          |
|                                                                          |
+--------------------------------------------------------------------------+
| 60.0 FPS   |  22050 Hz  |  ClsId 0x1081984  |  Esc pausa, F9 para       |
+--------------------------------------------------------------------------+
```

- **Pausar e Parar sempre no mesmo canto**, não em menu.
- **Tempo decorrido e FPS visíveis** — o usuário precisa saber se travou ou só
  está lento. Foi a diferença central desta sessão: 12 FPS medidos contra 30
  esperados.
- A imagem **não** ganha overlay por padrão.

### 3.3 Configurações

Uma aba, campos agrupados:

1. **NAND** — diretório, botão de escolher, botão de re-varrer, contagem.
2. **Vídeo** — escala inteira (1x..4x), filtro (nearest/linear).
3. **Áudio** — ligado/desligado, volume.
4. **Entrada** — mapeamento de teclado, e o estado do controle detectado.
5. **Avançado** — orçamento de passos por chamada, com o padrão visível e o
   motivo escrito na própria tela.

O item 5 é deliberado: nesta sessão um título foi declarado morto porque o
orçamento padrão de 64 M passos abortou um trabalho legítimo de 124 M. Quem abre
essa tela deve ver o número e o que ele significa.

---

## 4. Arquitetura

```
                 +-------------------------------------+
                 |  frontends/gui  (ImGui + SDL2/GL)    |
                 |  telas, estado de UI, comandos       |
                 +------------------+------------------+
                                    |
        +---------------------------+---------------------------+
        |                           |                           |
+-------v--------+      +-----------v----------+     +----------v---------+
| game_library   |      |  ui_config           |     | emulator_session   |
| descoberta,    |      |  preferencias,       |     | ciclo de vida da   |
| resolucao de   |      |  persistencia em     |     | emulacao: iniciar, |
| ClsId          |      |  ~/.local/share      |     | pausar, parar      |
+----------------+      +----------------------+     +----------+---------+
                                                                 |
                                                    +------------v-----------+
                                                    | carregador existente    |
                                                    | (game_probe / core)     |
                                                    +-------------------------+
```

**A camada `game_library` não conhece UI nem SDL.** Ela recebe um diretório e
devolve uma lista. É o que permite testar a descoberta sem abrir janela
(RNF-4) — e é o que evita que a lógica de identificação de título fique presa
dentro de um `if` de renderização.

### 4.1 Modelo de dados

```cpp
struct GameEntry {
  std::string folder;        // "274214"
  std::string mod_path;      // caminho absoluto do .mod
  std::string name;          // do .mif, ou vazio se desconhecido
  std::string name_source;   // "mif" | "pasta"
  std::string data_ggz;      // ou vazio
  std::string sound_ggz;     // ou vazio
  std::string bar;           // ou vazio
  uint32_t clsid = 0;        // 0 = desconhecido
  std::string clsid_source;  // "manifest" | "mif" | "mod" | "desconhecido"
  std::string status;        // "ok" | "nao_iniciavel"
  std::string status_reason; // texto para a UI
};
```

O campo `clsid_source` existe para a UI poder ser honesta: mostrar *de onde* veio
o ClsId é o que separa "resolvi" de "chutei".

### 4.2 Resolução de ClsId — ordem e justificativa

`gui/game_library.cpp`, na ordem do RF-3:

1. **Manifesto** `~/.local/share/zeebulator/games.json`, quando o usuário (ou uma
   ferramenta) já resolveu o valor. É o mais forte porque é explícito.
2. **`.mif`**: varredura por valor na faixa de applet, filtrando os códigos de
   sistema. Funcionou para parte dos títulos e **falhou** para `zenonia`,
   `a3d` e `heavyweaponbrew` — que só apareceram lendo o despacho do `.mod`.
3. **`.mod`**: procura o literal carregado por `ldr rX, [pc, #imm]` e comparado
   logo depois — o padrão que identifica o ClsId em `CreateInstance`. Sem exigir
   alinhamento de 4 bytes, que foi o erro da primeira tentativa.
4. **Desconhecido**: `clsid = 0`, `status = nao_iniciavel`, e a UI diz o motivo.

### 4.3 Ciclo de vida da emulação

```
 Idle ---- start(entry) ----> Iniciando ---- quadro pronto ----> Rodando
   ^                             |                                  |
   |                             | falha                            | pause
   +-------- stop() -------------+                                  v
   |                                                             Pausado
   +-------- stop() ------------------------------------------------+
```

Duas decisões:

- **Iniciar não pode bloquear a UI.** O carregamento de um título leva segundos
  (medido: até 124 M instruções numa única chamada). Se isso rodar no laço de
  renderização, a janela congela e o usuário acha que travou.
- **Parar precisa ser garantido.** Processo órfão é bug visível (RF-6).

---

## 5. Fases de entrega

Cada fase é utilizável sozinha. Não há fase que só sirva de andaime.

**Fase 1 — Biblioteca navegável (esta entrega).**
Descoberta, modelo, resolução de ClsId, persistência, janela com a lista,
busca e troca de raiz. Iniciar um jogo pode ainda delegar ao frontend existente,
desde que a UI não trave e não deixe processo órfão.

**Fase 2 — Emulação embutida.**
Extrair o carregador de `tools/game_probe.cpp` para uma biblioteca e rodar a
emulação no processo da GUI, com quadro na janela e controles de pausa.

**Fase 3 — Acabamento.**
Escala, filtros, mapeamento configurável, lista de recentes, captura de tela.

**Fase 4 — Biblioteca enriquecida.**
Capa, último acesso, tempo jogado, favoritos — desde que a fonte do dado seja
verdadeira, não um palpite.

---

## 6. Riscos

| risco | severidade | mitigação |
|---|---|---|
| Launcher por processo pode virar permanente | média | Fase 2 é explícita; a UI já nasce com a abstração `emulator_session` para a troca ser local |
| UI afirmar "jogável" sem teste de entrada | alta | RNF-6: a coluna de estado reporta imagem, e jogabilidade só com teste de entrada |
| Resolução de ClsId errar em silêncio | alta | `clsid_source` visível; sem certeza, o título é marcado não iniciável em vez de tentado |
| ImGui virar amontoado de janelas | média | Desenho de tela fixado na seção 3; abas, não janelas flutuantes |
| Dependência nova quebrar o build | média | FetchContent com versão fixada, e o alvo da GUI isolado: o resto do build não depende dela |

---

## 7. O que este design **não** promete

- Não promete jogabilidade. Promete descoberta, seleção e inicialização
  honestas. O critério de jogabilidade continua sendo o do
  `ARTIGO-SITUACAO-DOS-JOGOS.md`.
- Não promete que todo título do NAND inicia. Onze dos 62 do censo só
  funcionaram depois que o ClsId foi descoberto, e há títulos com ClsId ainda
  desconhecido.

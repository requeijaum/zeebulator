# Requisitos — GUI do Zeebulator (`new_ez_ui`)

Documento de requisitos. Numerados, verificáveis e com critério de aceite. O
design correspondente está em `GUI-DESIGN.md`.

Regra deste documento: **todo requisito tem um critério de aceite que outra
pessoa consegue executar sem perguntar nada ao autor.** Onde não há número, o
requisito está marcado como qualitativo de propósito.

---

## 1. Contexto

Hoje o emulador é operado por linha de comando:

```
zeebulator_game_probe <game.mod> <data.ggz|-> <sound.ggz|-> <cls_id_decimal>
```

Isso exige saber de antemão a pasta do título, quais assets ele usa e o ClsId do
applet — que não está em nenhum lugar óbvio (foi lido no despejo do próprio jogo
em 11 casos nesta sessão). O resultado é que a operação normal do emulador
depende de conhecimento de engenharia reversa.

Esta GUI existe para eliminar essa dependência: escolher o jogo e jogar.

---

## 2. Escopo

**Dentro do escopo**

- Biblioteca de jogos descoberta a partir de um diretório NAND configurável.
- Seleção e inicialização de um título.
- Configuração de caminhos e de opções de emulação.
- Estado de emulação visível e controlável (rodando, pausado, parado).

**Fora do escopo (nesta entrega)**

- Rede, loja, download, contas.
- Edição de memória, depurador, visualizador de VRAM.
- Recompilação ou alteração de assets do jogo.
- Suporte a plataformas além de Linux desktop (mas sem fechar a porta).

---

## 3. Requisitos funcionais

### RF-1 — Descoberta de jogos

O sistema varre o diretório NAND configurado e apresenta os títulos
encontrados, sem que o usuário informe caminho de `.mod` nem ClsId.

**Origem dos dados.** O layout real do NAND, medido nesta sessão:

```
<raiz_nand>/
  mod/<pasta>/<nome>.mod      # o binário do módulo
  mod/<pasta>/data.ggz        # opcional
  mod/<pasta>/sound.ggz       # opcional
  mod/<pasta>/<nome>.bar      # opcional (títulos BAR-only)
  mif/<pasta>.mif             # metadados do título, por pasta de applet
```

**Critério de aceite.** Com `--nand /media/.../debug_nand`, a lista mostra os
títulos cujas pastas contêm `.mod`. Contagem verificável: **63 pastas com `.mod`**
no NAND desta máquina (medido).

### RF-2 — Identificação do título

Cada linha da lista mostra um nome legível, não só o número da pasta.

**Critério de aceite.** Para todo título cuja pasta tenha `.mif`, o nome vem do
`.mif`. Quando não houver nome, a lista mostra a pasta e marca a origem do nome
como desconhecida — **nunca inventa um nome**.

### RF-3 — Resolução do ClsId

O sistema determina o ClsId necessário para inicializar o título **sem
intervenção manual**.

Ordem de resolução, da mais forte para a mais fraca:

1. **Manifesto** (`games.json` no diretório de dados do emulador), quando
   existir: valor explícito e verificável.
2. **`.mif`** da pasta, quando o valor estiver na faixa de applet.
3. **Leitura no `.mod`**: o literal comparado em `CreateInstance` (método usado
   para achar os 11 ClsIds corrigidos nesta sessão).
4. **Indeterminado** — o título aparece como não iniciável, com o motivo.

**Critério de aceite.** Para os títulos com ClsId conhecido e documentado, o
sistema apresenta o mesmo valor que o manifesto. Para os indeterminados, a UI
diz "ClsId desconhecido" e não tenta iniciar.

**Por que este requisito é explícito.** Nesta sessão, 11 títulos foram medidos
com ClsId errado e apareceram como "mortos" por isso. Se a GUI errar em
silêncio, ela reproduz o mesmo problema na cara do usuário.

### RF-4 — Inicialização

Selecionar um título e confirmar inicia a emulação.

**Critério de aceite.** Um título com ClsId resolvido (ex.: `cnk2`, pasta
`274214`, ClsId `17308036`) sai da biblioteca e passa a mostrar o quadro
emulado. Um título sem ClsId resolvido mostra erro nomeando o problema, sem
travar a UI.

### RF-5 — Estado de emulação visível

A UI mostra, no mínimo: título em execução, se está rodando ou pausado, e o
tempo decorrido de emulação.

**Critério de aceite.** Com a emulação rodando, o tempo decorrido avança; ao
pausar, ele para de avançar.

### RF-6 — Parar a emulação

O usuário encerra a sessão atual e volta à biblioteca sem fechar a aplicação.

**Critério de aceite.** Depois de parar, a biblioteca está utilizável de novo e
o processo do núcleo não fica órfão (verificável por listagem de processos).

### RF-7 — Diretório NAND configurável

O usuário troca o diretório NAND pela própria interface.

**Critério de aceite.** Trocar o diretório e re-varrer atualiza a lista. O valor
persiste entre execuções.

### RF-8 — Persistência de configuração

As preferências ficam em arquivo de dados do usuário.

**Critério de aceite.** As preferências vão para
`$XDG_DATA_HOME/zeebulator/ui.json` (ou `~/.local/share/zeebulator/ui.json`).
**O arquivo não é escrito no diretório NAND nem em mídia de ROM** — requisito de
integridade, não de estilo.

### RF-9 — Entrada por teclado e controle

As teclas do teclado e os botões do controle são mapeados para os códigos AVK do
BREW que o jogo espera.

**Critério de aceite.** Com o Z-Pad em tela, as setas alcançam o applet como
`EVT_KEY` com os códigos AVK correspondentes, e o estado do analógico é
normalizado pela lógica já existente em `frontends/standalone/zpad_edges.cpp`.

### RF-10 — Diagnóstico acessível

O erro de um título não pode virar "não funciona". A UI mostra a causa quando a
tem.

**Critério de aceite.** Para um título que estoura o orçamento de passos, a UI
nomeia isso. Para um título sem ClsId, a UI nomeia isso.

---

## 4. Requisitos não-funcionais

### RNF-1 — Não duplicar o carregador

A lógica de carregar e executar um título existe hoje em
`tools/game_probe.cpp`. A GUI **não** reimplementa isso: ou reusa, ou chama.

**Critério de aceite.** Não existe segunda implementação de descoberta de
entry point, montagem de VFS ou criação de applet.

### RNF-2 — Desempenho da interface

A lista de jogos responde em menos de **200 ms** para 63 títulos.

### RNF-3 — Tamanho e dependências

Uma dependência nova de UI, obtida pelo mesmo mecanismo que o projeto já usa
(`FetchContent`). Nada de toolkit puxando árvore inteira de sistema.

### RNF-4 — Testabilidade da lógica

Descoberta de jogos, resolução de ClsId e persistência de configuração são
testáveis **sem abrir janela**.

**Critério de aceite.** Existem testes unitários para as três, executados pelo
`ctest` existente.

### RNF-5 — Portabilidade mínima

Compila em Linux x86-64 com as dependências já usadas (SDL2, OpenGL). Wayland e
X11 funcionam.

### RNF-6 — Honestidade na interface

A UI não afirma o que não mediu: um título sem teste de entrada aparece como
"imagem", não como "jogável".

---

## 5. Critério de aceite da entrega

A entrega desta fase está completa quando, nesta ordem:

1. `ctest` continua passando, incluindo os testes novos (684 + novos).
2. A GUI abre e lista os 63 títulos do NAND.
3. Trocar o diretório NAND pela interface funciona e persiste.
4. Um título conhecido inicia e desenha quadro.
5. Parar volta para a biblioteca sem deixar processo órfão.
6. As preferências foram escritas em `~/.local/share/`, **não** no NAND.

Os itens 2 a 6 são verificáveis rodando; nenhum depende de opinião.

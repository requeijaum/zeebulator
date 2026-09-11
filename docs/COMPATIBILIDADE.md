TASKS.md

---

## Segunda medida: os titulos que iniciam realmente RODAM?

`EVT_APP_START` retornar 1 significa apenas que o applet aceitou o evento de
inicio. Nao prova que o titulo executa. Para separar as duas coisas, cada um dos
50 que iniciam foi executado com o servidor de controle e o contador de ticks
lido em dois momentos (12 s e 24 s).

| Estado | Titulos |
|---|---|
| **Roda** (contador avanca) | 14 |
| **Congela** (contador parado) | 20 |
| Inconclusivo (ver ressalva) | 15 |

Rodam: asq, bio4_brew, cnk2, ddragonz, game, gof, ironsight, pacmania, pbc, quake, rmp, rt2, torkandkral, toyraidzeebo

RESSALVA SOBRE O "INCONCLUSIVO": nao e uma falha do emulador, e um limite do
medidor. Em titulos como o karnovr o servidor de controle nao chega a abrir
dentro da janela de espera (o jogo demora a alcancar o laco de eventos), e em
outros ele abre depois de eu desistir de conectar. O log desses titulos mostra
progresso normal -- o karnovr, por exemplo, imprime "end create channel!!!!".

## "Congela" nem sempre e travamento

Dois casos medidos mostram que o rotulo esconde coisas diferentes:

- **baddudes / hbarrel** ficam num ciclo suspende/retoma que EXECUTA: com
  orcamento de 8 bilhoes de instrucoes o baddudes completa 10558 ciclos de tick.
  Ele le o proprio `.zip` byte a byte, re-varrendo o diretorio central a cada
  arquivo procurado -- cada byte e um trap de HLE. Nao esta parado, esta lento.
- **fifa09** parecia falhar em `CreateInstance` com o orcamento de 150 milhoes
  de instrucoes da varredura. Com 8 bilhoes ele passa e chega ao laco de ticks.
  O perfil mostra o motivo: 26376 chamadas de realloc e 3300 de malloc num laco
  que monta uma lista de strings, item a item.

Ou seja, parte do que a primeira medida conta como falha e orcamento de
instrucoes, nao incompatibilidade. Um numero de compatibilidade sem o orcamento
declarado ao lado nao significa nada.

## Falso alvo descartado

As 15 ocorrencias de "Miscellaneous instruction space (MRS/MSR/etc.)" nos logs
do corpus estao TODAS no mesmo endereco, `pc=0x00090024`, fora de qualquer
modulo carregado. E onde o interpretador cai depois de saltar para nulo e
executar lixo -- sintoma, nao lacuna de CPU. Implementar MRS/MSR nao destravaria
nenhum titulo.


### Atualizacao da segunda medida: titulos orientados a threads cooperativas

A medicao automatizada inicial usava `tick_count` reportado pelo servidor de controle.
No entanto, o `tick_count` era incrementado unicamente em timers `IShell` expirados.
Jogos cujo laco principal e orientado a threads cooperativas BREW (`IThread` / `0x01001017`)
executavam milhares de fatias de thread normais pelo guest sem que nenhum timer IShell
disparasse, ficando falsamente marcados como `tick=0` estatico ("CONGELAM").

Com a correcao no commit `a1edff6` (`tick_count += run_pending_threads_fn("tick")`),
a medicao real confirmou que estes 6 titulos estao em plena execucao do laco guest:
  - **AirRacez**: tick 146 -> 417 (~54 ticks/s)
  - **Bajaz**: tick 135 -> 405 (~54 ticks/s)
  - **JetBoardz**: tick 122 -> 495 (~62 ticks/s)
  - **Boiaz**: tick 73 -> 337 (~44 ticks/s)
  - **baddudes**: tick 10 -> 384 (~62 ticks/s)
  - **hbarrel**: tick 28 -> 402 (~62 ticks/s)

Total de titulos com laco de execucao ativo comprovado: **24 titulos**
  - **heavyweaponbrew**: avanca ticks ativamente (8 -> 257, ~31 ticks/s)
  - **bjt**: avanca ticks ativamente (8 -> 53)
  - **fifa09**: avanca ticks ativamente (~62 ticks/s, thread 0x80300064) (de 50 que iniciam).

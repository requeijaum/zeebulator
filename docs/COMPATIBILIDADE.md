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

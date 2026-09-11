# Compatibilidade do corpus Zeebo (medida)

Medicao automatizada sobre a NAND completa em estrutura `fs:/` do BREW
(`mif/<id>.mif` + `mod/<id>/`), 62 titulos, executados com `zeebulator_game_probe`.
Os ClassIDs sao extraidos diretamente de cada `.mif` via `ExtractMifClassIds` (60/60 pares validados).

## 1. Resumo por Estagio

| Estagio | Titulos | % |
|---|---|---|
| **`EVT_APP_START = 1`** | **54** | **87%** |
| Entra no laco de eventos da janela | 5 | 8% |
| Bloqueado no carregamento de recursos | 3 | 5% |
| **Total de titulos no corpus** | **62** | **100%** |

- **`AEEMod_Load` OK**: 62 / 62 (100%)
- **`CreateInstance` OK**: 59 / 62 (95%)
- **Títulos com laco de execucao ativo comprovado (avancando ticks de forma continua)**: **25 titulos**

---

## 2. Titulos com Laco de Jogo Ativo Comprovado (25 titulos)

Estes titulos foram medidos via canal de controle em dois momentos distintos, comprovando avanco continuo de ticks:
- **Arcade / Corrida (TecToy / Fishlabs / Data East)**: `AirRacez` (~54 t/s), `Bajaz` (~54 t/s), `JetBoardz` (~62 t/s), `Boiaz` (~44 t/s), `baddudes` (~62 t/s), `hbarrel` (~62 t/s), `torkandkral`, `toyraidzeebo`
- **3D / OpenGL ES**: `asq`, `bio4_brew`, `cnk2`, `ddragonz`, `gof`, `ironsight`, `pacmania`, `pbc`, `quake`, `recklessracing`, `rmp`, `rt2`
- **Acao / RPG / Simulação**: `fifa09` (~62 t/s), `heavyweaponbrew` (~31 t/s), `zenonia` (~15 t/s), `bjt` (~10 t/s), `game`

---

## 3. Z-Wheel (Menu Principal do Zeebo - `tectoy.mod`)

Destravada por completo ate o laco principal de eventos:
1. **`ISQLMgr` / `ISQLDatabase`** (SQLite real embutido, commit `f20fac8`): abre `tt_prefs.db`, passa no `PRAGMA integrity_check`, sem erros de preferencia.
2. **`IWidget` (`0x01028e51`)** (commit `be83768`): acessador com convencao invertida (diferente de zero = sucesso).
3. **`ICollection` (`0x0100104f`)** (commit `24feff4`): `AtEnd()` retorna 1 para colecoes vazias, eliminando laco infinito de 13,6M chamadas.
4. **Isolamento de memoria** (commit `24feff4`): movido SqlHle para `0x800D0000..0x800E0000`, eliminando colisao de vtable.
5. **Abertura do banco de cache**: abre `asset_cache` via SQLite e entra no laco principal com janela aberta (`running=true`).

---

## 4. Parsers de Containers Concluidos

Todos os containers de dados do corpus possuem suporte completo no núcleo:
- **`AEZ`** (Fishlabs, 12 arquivos, 1390 entradas): descompressao gzip e entradas brutas `0xFFFFFFFF` (commit `c4d60cd`).
- **`FUFS` (`.vfs`)** (6 arquivos, 1642 entradas): tabela de 12 bytes e funcao de hash polinomial insensivel a caixa `h = h*67 + (toupper(c) - 113)` (commit `3ceae7d`).
- **`SAR` (`SWVARC`)** (Superscape, 53 arquivos, 332 entradas): leitura completa validada contra o proprio jogo (commit `fcac5ef`).
- **`PAKZ` / `BAR` / `GGZ` / `PKG`**: 100% integrados no VFS.

---

## 5. Ultimos Titulos com Trabalho Pendente

- **`nfs`**: tenta abrir `../nfsresources/` em laco; precisa de mapeamento de diretorio de recursos pai.
- **`alpineracerex`**: gasta dezenas de bilhoes de passos processando fontes e texturas no `EVT_APP_START`; precisa de otimizacao JIT/fast-path.
- **`rocketweb`, `reksio`, `prey3d`, `imicro3d`**: chegam ao laco de eventos da janela (`running=true`), aguardando despacho de eventos de interface.

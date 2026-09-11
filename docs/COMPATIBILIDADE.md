# Compatibilidade do corpus Zeebo (medida autoritativa)

Medicao automatizada individual sobre os 63 titulos presentes na NAND oficial
em estrutura `fs:/` do BREW (`mif/<id>.mif` + `mod/<id>/`).
Executados com `zeebulator_game_probe`. ClassIDs extraidos diretamente dos MIFs
via `ExtractMifClassIds` e validados.

---

## 1. Resumo por Estagio de Execucao

| Estagio de Execucao | Titulos | Proporcao |
|---|---|---|
| **`EVT_APP_START = 1`** | **56** | **88,9%** |
| **`EVT_APP_START` nao-zero** (`imicro3d`) | **1** | **1,6%** |
| **Laco principal de eventos alcancado** (`tectoy`, `prey3d`, `reksio`, `rocketweb`) | **4** | **6,3%** |
| **Total executando no laco de eventos / apos START** | **61** | **96,8%** |
| `CreateInstance` OK (passa com orcamento expandido, ex. `fifa09`) | 1 | 1,6% |
| `AEEMod_Load` OK (em inicializacao de recursos, ex. `nfs`) | 1 | 1,6% |
| **Falha em carregar (`AEEMod_Load` falhou)** | **0** | **0,0%** |
| **Total de titulos no corpus** | **63** | **100%** |

- **Taxa de sucesso de carga (`AEEMod_Load`)**: **63 / 63 (100%)**
- **Taxa de criacao de instancia (`CreateInstance`)**: **62 / 63 (98,4%)**
- **Taxa de alcance do laco de eventos da aplicacao**: **61 / 63 (96,8%)**
- **Titulos com avanco ativo e continuo de ticks comprovado no guest**: **26 titulos**

---

## 2. Destaques e Principais Marcos Tecnicos Alcancados

1. **Z-Wheel (Menu Principal do Zeebo - `tectoy.mod`)**:
   - Totalmente funcional ate o laco de eventos.
   - Bancos SQLite `tt_prefs.db` e `asset_cache` abertos e operacionais (`ISQLMgr`/`ISQLDatabase`).
   - Acessador de interface `0x01028e51` com convencao invertida (diferente de zero = sucesso).
   - Iterador de colecao `0x0100104f` com `AtEnd()` correto.
   - Janela mantida aberta com laco de eventos ativo.

2. **Quake 2 (`quake2brew.mod`)**:
   - Resolucao de arquivos em diretorios irmaos (`./../quake2res/pak0.pakz`) implementada com seguranca de fronteira.
   - Leitura de 52,5 MB de dados do `pak0.pakz` concluida com sucesso.
   - Inicializacao do motor e despachos de threads cooperativas operacionais.

3. **Alpine Racer EX (`alpineracerex.mod`)**:
   - Slot 3 de QEGL (`eglGetError`) implementado retornando `0x3000` (`EGL_SUCCESS`).
   - Inicializacao grafica completa sem destruicao de contexto.
   - Execucao ativa a ~15 ticks/s.

4. **Heavy Weapon (`heavyweaponbrew.mod`)**:
   - Causa raiz de corrupcao de memoria isolada: pixels decodificados movidos para a regiao dedicada `0x88000000` (evitando sobrescrever a tabela estatica em `0x80280000`).
   - Execucao ativa a ~31 ticks/s.

5. **Zenonia (`zenonia.mod`)**:
   - Stub da interface `ITextCtl` (`0x01003109` / `0x01003209`) registrado.
   - Assercao interna `AF!` eliminada.
   - Execucao ativa a ~15 ticks/s.

6. **Zuma's Revenge (`zumar.mod`) e Bejeweled Twist (`bjt.mod`)**:
   - Campos publicos da estrutura `IDIB` (`0x01001045`) preenchidos conforme `AEEIDIB.h` (`cx=640`, `cy=480`, `nPitch=1280`, `nDepth=16`, `nColorScheme=16`).
   - Ambos destravados com `INITIALIZATION DONE!` e laço de ticks ativo.

7. **Crash Nitro Kart 3D (`cnk2.mod`)**:
   - Audio confirmado audivel (pico em escala cheia 32767).
   - Suporte a contêiner FUFS (`data.vfs`) com tabela de 1642 entradas e funcao de hash polinomial insensivel a caixa.

---

## 3. Contêineres de Dados 100% Suportados no Nucleo
- `AEZ` (Fishlabs, 12 arquivos, 1390 entradas)
- `FUFS` (`.vfs`, 6 arquivos, 1642 entradas)
- `SAR` (`SWVARC`, Superscape, 53 arquivos, 332 entradas)
- `PAKZ`, `BAR`, `GGZ`, `PKG` integrados no VFS.

---

## 4. Tabela Autorizada por Titulo (63 titulos)

| Modulo ID | Titulo | ClassID | Status Medido |
|---|---|---|---|
| 11839 | kh | 16933265 | START_1 |
| 12875 | imicro3d | 16945859 | START_NONZERO |
| 263019 | chessbots | 17044419 | START_1 |
| 274214 | cnk2 | 17308036 | START_1 |
| 274259 | a3d | 17308016 | START_1 |
| 274754 | ddragonz | 16971657 | START_1 |
| 274755 | tectoy | 17237912 | EVENT_LOOP |
| 274791 | zeebo_app | 17244565 | START_1 |
| 274802 | quake | 17332796 | START_1 |
| 274803 | fifa09 | 17332968 | CREATE_OK |
| 274804 | brainchallenge | 17332809 | START_1 |
| 276121 | nfs | 17350844 | LOAD_OK |
| 276151 | alpineracerex | 17250341 | START_1 |
| 276152 | ridgeracer | 17333107 | START_1 |
| 276153 | quake2brew | 17333276 | START_1 |
| 276154 | prey3d | 17355918 | EVENT_LOOP |
| 276212 | pacmania | 17333106 | START_1 |
| 276675 | bio4_brew | 17346412 | START_1 |
| 276731 | tekken2 | 17355191 | START_1 |
| 276809 | Rolimaz | 17350843 | START_1 |
| 277083 | bjt | 17351137 | START_1 |
| 277229 | game | 17368006 | START_1 |
| 277285 | AirRacez | 17366790 | START_1 |
| 277380 | gof | 17365688 | START_1 |
| 277455 | zenonia | 3207471137 | START_1 |
| 277495 | reksio | 17383435 | EVENT_LOOP |
| 277534 | zeebotennis | 17362937 | START_1 |
| 277727 | Bajaz | 17366791 | START_1 |
| 278200 | heavyweaponbrew | 17397922 | START_1 |
| 278212 | zeebovolley | 17366805 | START_1 |
| 278282 | rmp | 17365689 | START_1 |
| 278283 | JetBoardz | 17366804 | START_1 |
| 278285 | Boiaz | 17366803 | START_1 |
| 278738 | dodgeball | 17366806 | START_1 |
| 278962 | peggle | 17407190 | START_1 |
| 278965 | toyraidzeebo | 17387846 | START_1 |
| 278986 | cninja | 17376478 | START_1 |
| 278987 | spinmast | 17376481 | START_1 |
| 278988 | strhoop | 17376482 | START_1 |
| 279036 | game | 17392549 | START_1 |
| 279125 | supbtime | 17376483 | START_1 |
| 279126 | karnovr | 17376477 | START_1 |
| 279159 | zeebopeteca | 17366808 | START_1 |
| 279173 | wizdfire | 17376484 | START_1 |
| 279200 | magdrop3 | 17376480 | START_1 |
| 279233 | darkseal | 17376479 | START_1 |
| 279369 | abd | 17359702 | START_1 |
| 279380 | footparty | 17366809 | START_1 |
| 279382 | zeeboids | 17366810 | START_1 |
| 279394 | rocketweb | 17425632 | EVENT_LOOP |
| 279712 | zumar | 17411925 | START_1 |
| 279888 | baddudes | 17427483 | START_1 |
| 279889 | hbarrel | 17427484 | START_1 |
| 280173 | allstarcards | 17383642 | START_1 |
| 280214 | asq | 17422990 | START_1 |
| 280221 | ironsight | 17422991 | START_1 |
| 280238 | pbc | 17437249 | START_1 |
| 280386 | alice | 17441591 | START_1 |
| 280394 | recklessracing | 17422993 | START_1 |
| 280463 | torkandkral | 17460499 | START_1 |
| 280602 | rt2 | 17422992 | START_1 |
| 280634 | activitycenter | 17441589 | START_1 |
| 280647 | funsoccer | 17366807 | START_1 |

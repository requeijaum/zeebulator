# NAND do Zeebo (EFS2) — o que já é extraível e o que falta

Fonte: `1.1.2.bin` (dump TripleOxygen). Conferido por md5:

```text
research/docs/nand-dump/1.1.2.bin            016297a3492483ec5ebd5374fa00ebf4
zeebo-lle/nand/1.1.2.bin                     016297a3492483ec5ebd5374fa00ebf4   -> identicos
```

O `md5.txt` de ambos os projetos traz `057fd078...` que é o hash do `.7z`, não do
binário extraído. Não é divergência.

## Layout provado do dirent (0:EFS2APPS @ 0x3220000)

```text
69 | inode u32 | reclen u8 | type u8 | parent_ref u32 | pad 0x00 | name (reclen-5)
```

Verificado byte a byte em `tectoy.cfg`, `274755`, `mod`, `udata`, `flixfile.dat`.

## O que a varredura entrega

`tools/nand_efs2_recover.py`:

- `efs2_tree.txt` — 69.634 registros de dirent (722 nomes únicos no primeiro corte)
- `chain_manifest.tsv` — 24.515 cadeias de clusters terminadas em 0xFFFFFFFF

## Limite honesto

O campo `inode` NÃO é único no filesystem: `0x60` aparece como `274755`, `.DIAGCFG`,
`apps_err_data_index00_log00.txt` e outros; `0x1fae8` aparece como `faq`. Ou seja,
`(parent_inode, name)` só é único localmente. Sem a tabela de gnodes que dá a
identidade global, **não é possível associar nome a payload**, e portanto não é
possível gravar `mod/`, `sys/` ou `shared/` completos a partir da imagem. O repo
`zeebo-lle` registra exatamente o mesmo limite ("extração genérica gnode table
ainda não revertida por bytes").

## Fatos úteis já medidos

- `fontsize.map`, `preloaded.cfg` e `demo.3gp` **não existem em nenhum lugar da
  imagem** (busca em 128 MiB). A Z-Wheel abre `fontsize.map` e desreferencia o
  handle sem testar nulo; com o arquivo ausente o guest salta para o endereço 0.
  O comportamento correto do emulador é não tratar essa excursão como fatal,
  porque o próprio título possui caminho de erro ("Unable to load font in
  CreateTectoyRollerWidget").
- `flixfile.dat` e `keyboard.cfg` têm dirents reais na imagem; as strings
  `fs:/sys/keyboard.cfg` e `fs:/shared/...` aparecem no código, não como arquivos
  listados.
- As pastas `sys/` e `shared/fonts/` que existem hoje no `debug_nand` do pendrive
  estão **vazias** e não correspondem a dirents da imagem: são criação de
  extração anterior, não estrutura real do dump.
- Fontes de UI do console não estão no EFS como arquivos; o ELF do APPS cita
  fontes Helvetica na própria partição.

## Próximo passo para extrair de verdade

Reverter a tabela de gnodes: localizar, para cada dirent, o registro que guarda
tamanho e primeira cadeia. Três âncoras já validadas por bytes servem de teste:

```text
reksio.mod  dirent inode 0x265e4   cadeia comeca em 0x6d11 (bloco @0x3b1d400)
274755      dirent inode 0x60      cadeia comeca em 0x1dca (bloco @0x3a92000)
tectoy.mod  dirent inode 0x7ff13   bloco @0x6026200
```

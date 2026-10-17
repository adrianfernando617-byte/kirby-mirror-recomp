# KirbyMirrorRecomp

Port nativo (PC, Linux) de **Kirby & The Amazing Mirror** (GBA, USA) via
recompilação estática com o [gbarecomp](https://github.com/mstan/gbarecomp).
O código do jogo é traduzido para C++ nativo uma vez no build e o runtime
roda o binário gerado com vídeo/áudio/input nativos — sem interpretadores
("static-recompiled"), com debug server TCP embutido e proposta de mods em
andamento.

> ⚠️ **Projeto de fã, não-comercial.** Você precisa da sua própria ROM e
> BIOS; nada delas é distribuída aqui. Use/comparta dentro do que a sua
> jurisdição e a [licença](LICENSE) do projeto permitem.

## Estado

| Área | Status |
|---|---|
| Boot e gameplay | ✅ rodando frame-exato (60.105 funções C++ nativas) |
| Save (SRAM) | ✅ automático em `<rom>.sav` |
| Áudio | ✅ (mixer sombra MP2K; alguns drivers caem para mix de hardware) |
| Controles | teclado + gamepad (SDL) |
| Widescreen | ✅ mod v3 (mapa real nas margens — `wide` por padrão) |
| Netplay 2–4P | 📋 planejado (infra existe no runtime) |
| Margens autorais (mundo vizinho) | 📋 fase 3 do widescreen |

Bugs e limitações conhecidos são acompanhados em
[Issues](https://github.com/adrianfernando617-byte/kirby-mirror-recomp/issues).
Reporte o seu com o log de saída anexado (veja [Suporte](#suporte)).

## Requisitos

- Linux x86-64 (Windows/macOS: ainda não testado)
- GCC/Clang com C++20, CMake ≥ 3.20, Ninja ou Make
- **BIOS GBA** (arquivo `gba_bios.bin`): SHA-1
  `300c20df6731a33952ded8c436f7f186d25d3492`
- **ROM USA** de Kirby & The Amazing Mirror: SHA-1
  `274b102b6d940f46861a92b4e65f89a51815c12c`

O runtime se recusa a rodar se qualquer hash não bater — é a garantia de
que o código nativo gerado tem correspondência 1:1 com o conteúdo.

## Build

```sh
git clone https://github.com/adrianfernando617-byte/kirby-mirror-recomp.git
cd kirby-mirror-recomp
git submodule update --init --recursive
```

Coloque a BIOS em `gbarecomp/bios/gba_bios.bin` e a ROM em `roms/kirby_usa.gba`
(ou passe `--bios` / `--rom` na linha de comando).

### 1. Gerar o código nativo (uma vez por revisão de ROM)

O recompilador consome os símbolos do projeto decomp
[jiangzhengwenjz/katam](https://github.com/jiangzhengwenjz/katam):

```sh
# clonar katam e buildar normalmente (tools/agbcc etc.)
# importar símbolos:
python3 gbarecomp/tools/symbol_import/import_decomp_symbols.py \
    --help   # veja as flags exigidas (caminhos do katam, out-dir symbols/)
# recompilar:
gbarecomp/tools/gba_recompile <flags do seu build> katam_recomp.toml
```

O resultado dura em `generated/` (16 shards + dispatch table). Trabalho
com symbols/commit está aqui: `symbols/`.

> Dica: se o runtime acusar *self-this*/**`NOT_STATIC`** com misses
> (`bridged 0x...`), o arquivo `recomp_master_misses_*.toml.frag` gerado
> lista os PCs — revisar e incorporar como `[[extra_func]]`/`[[jump_table]]`
> no `katam_recomp.toml` torna o runtime 100% estático.

### 2. Compilar

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 3. Rodar

```sh
./build/KirbyMirrorRecomp --rom roms/kirby_usa.gba
```

## Controles

| Ação | Teclado |
|---|---|
| A | `X` |
| B | `Z` |
| D-pad | setas |
| Start | `Enter` |
| Select | `Right Shift` |
| L / R | `C` / `V` |
| Turbo | `Tab` |
| Pause | `Shift+P` |
| Fullscreen | `Alt+Enter` |
| Perf overlay | `F` |

Gamepad: mapeado automaticamente (testado com DualShock 4).

## Widescreen (mod v2)

O runtime tem um subsistema declarativo de mods (docs:
`gbarecomp/docs/MOD_PACKAGES.md`). O mod `kirby-mirror.display` está
**ativo por padrão** e estende a view lógica para **304px** (240
nativos + 4 tiles de margem por lado). A largura e a política de margem
são opções do package (manifest) e o estado fica em `build/mods/state.toml`
(rodando pela janela: opções editáveis no menu de mods do launcher).

Opção `width`: `304` (padrão), `284` (16:9 exato), `240` (console).

Opção `margins` (como as colunas extras são preenchidas):

| valor | efeito |
|---|---|
| `wide` (padrão) | **margens mostram o mapa real da sala** — o canvas da sala (RoomTiledBG em RAM, maior que a tela) é amostrado host-side além dos 240px: conteúdo que o GBA nunca exibiria |
| `wrap` | margens mostram o anel de tiles do próprio jogo (repete a borda — aparência das salas unitais) |
| `clamp` | margens continuam a coluna nativa mais próxima |
| `black` | pillarbox sólido |

Como funciona o `wide`: cada sala do KAM carrega em RAM um canvas
decodificado com as dimensões reais (ex.: hub com 62×46 tiles). O plugin
(amarrado ao `g_ws_tilemap_provider` do runtime) lê esse canvas por frame
e calibra ao vivo o mapeamento canvas↔VRAM de cada camada de hardware,
comparando o anel visível contra o canvas com deltas candidatos; só
"arma" a camada quando ≥75% dos pontos casam — salas/menus sem canvas
largo caem no fallback `wrap` (fail-closed). Tiles transparentes
(`0000`) contam como transparentes.>

A política é reafirmada por frame (hook na `gf_VBlankIntr`) sobrevivendo
a savestates. Auditoria e roadmap (grafo de portas, salas vizinhas) em
`docs/audit/bg-widescreen.md`.

## Estrutura do repo

```
kirby-mirror-recomp/
├── CMakeLists.txt           # build do executável (mods ON)
├── katam_recomp.toml        # config do recompilador
├── game.toml                # config do runner (BIOS/ROM/save SRAM)
├── symbols/                 # símbolos importados do katam
├── mods/packages/           # pacote de display embutido
├── src/main.cpp             # ponto de entrada (RunOptions + hash gate)
├── src/mod_widescreen.cpp   # plugin widescreen (ativação via catálogo)
├── gbarecomp/               # submódulo: plataforma (runtime + recompilador)
└── generated/               # SAÍDA (não versionada) do gba_recompile
```

## Licenças

- Este repositório: [PolyForm Noncommercial 1.0.0](LICENSE) — mesmo termo
  do [gbarecomp](https://github.com/mstan/gbarecomp) (Matthew Stanley) e do
  MinishCapRecomp, dos quais a estrutura do runner foi adaptada.
- `gbarecomp/` (submódulo): contém um patch nosso
  (`fix/mod-state-default-roundtrip`, pendente de upstream) sobre o
  upstream `mstan/gbarecomp`.
- Símbolos de `symbols/` derivam do decomp katam (projeto público, sem
  licença declarada — uso pessoal/educacional; revisar antes de qualquer
  redistribuição).

## Suporte

Ao reportar um problema, inclua:

1. O **log completo** de stdout/stderr do executável até o erro;
2. A saída de `./build/KirbyMirrorRecomp --help` (versão e flags);
3. Se for de vídeo: a linha que começa com `host_window: presentation`;
4. Se for de gameplay/emulação: os trechos
   `self_heal` / `bridged 0x...` do log.

Issues com o template acima são muito mais rápidas de reproduzir. Detalhes
técnicos do mapeamento das camadas de vídeo (BG0=HUD, BG2/BG3=mundo,
streaming de tiles ROM→IWRAM→VRAM por DMA3) estão anotados nos comentários
de `src/mod_widescreen.cpp` e nas notas de auditoria em `docs/audit`.

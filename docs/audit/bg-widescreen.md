# Auditoria BG/câmera — Kirby & the Amazing Mirror (runtime vivo)

Nota de auditoria feita em sessão com o jogo rodando no runtime nativo
(`--tcp-observe 19853`, anel `GBARECOMP_MMIO_CAP=1`). Descobertas por
observação direta de registradores + traçado de escritas de IO com
atribuição de PC, resolvida por `cmd:"symbol"`.

## Modo de vídeo e camadas

| Reg | Valor | Interpretação |
|---|---|---|
| DISPCNT | `0x1B40` | Modo 0, BG0+BG2+BG3 ligados, BG1 desligado, OBJ 1D-mapping |
| BG0CNT | `0x1F03` | prio 3, cbb 0, 4bpp, **sb31** — camada HUD/texto, scroll fixo |
| BG2CNT | `0x1D09` | prio 1, cbb 2, 4bpp, **sb29** — mundo (parallax/piso) |
| BG3CNT | `0x1E0A` | prio 2, cbb 2, 4bpp, **sb30** — mundo (fundo) |
| BG1 | desligada | **sb28 sobrando** — espaço livre para BG largo do mod |

Layout de screenblocks idêntico ao FireRed. BG0 (HUD) com scroll fixo →
pillarbox de menus/HUD é viável sem tocar no mundo.

## Pipeline de mundo (ROM → IWRAM → VRAM)

Não existe buffer de sala completa (busca binária de linhas de tilemap
no EWRAM: **zero hits**). O mundo é *streamed* por DMA3:

```
ROM (sala comprimida)
  │  pc 0x08156266 (gf_tfunc) ── DMA3: ROM → IWRAM staging
  ▼
IWRAM 0x030031C0..0x030035C0 (1 KB, anel por câmera)
  │  pc 0x08156E58 (gf_tfunc) ── DMA3: IWRAM → VRAM screenblocks
  ▼
sb29/sb30 anel 32-colunas, recarrega com o scroll da câmera
```

Funções nomeadas: `gf_UpdateScreenDma` (0x08151DC4, programa DMA3),
`gf_VBlankIntr` (0x08152368), `gf_m4aSoundVSync`, `gf_LoadLevelGfx`,
família `gf_AreaMap*` (mapa de espelhos).

## Estado multiplayer visível no memory map

- `gKirbys` @ `0x02020EE0` — 4 structs de jogador (stride `0x6A0`)
- buffers de OAM por jogador: `0x030060B0/61B0/62B0/63B0` (256 B cada)
- `gAIKirbyState`, `gNumHumanPlayers`, `gLocalPlayerId`, `gNumKirbys`
- SIO: `gMultiSioSend/Recv`, `gMultiBootParam`, `gMultiBootDataRecv`
- `gCurLevelInfo` @ `0x02023530` (0x19A0), `gTasks` @ `0x030019F0`,
  `gBgScrollRegs` @ `0x03003680` (espelho de scroll p/ o host),
  heap de IWRAM `gIwramHeap` @ `0x03003A20`, tail `0x03006CC4..0x03008000`
  livre (~5,3 KB — cabe staging de 2 KB para margens).

## Consequências pro widescreen

1. Estratégia "Minish" (sala inteira em RAM) **não se aplica**: o jogo
   não materializa a sala. É o cenário Step C do FireRed: estender o
   anel de tiles p/ 64 colunas (stream + cópia) e objeto de câmera.
2. sb28 livre + tail de IWRAM livre dão os recursos p/ o estendido.
3. Mod v1 (commit inicial do wide): `mod_owns_adaptive_view`,
   `max_view_width=304`, plugin `kirby-mirror.widescreen` via
   `gba_mod_set_view_width`. Margens v1 = conteúdo do anel do próprio
   jogo (repetição da borda) — inofensivo e sem quebrar o HUD (BG0
   rende normal).
4. Fase 2 implementada (plugin v2, `src/mod_widescreen.cpp`):
   - **Opção `margins`** no manifesto (`wrap` | `clamp` | `black`),
     persistida no roundtrip do `state.toml`.
   - `wrap` (default): anel do próprio jogo como em v1.
   - `clamp`: o plugin instala `gba::g_ws_bg_x_provider` (host, read-only)
     e a margem continua exatamente a coluna nativa mais próxima
     (`sample_hx` fixo em `left`/`left+239`) — mata a repetição
     espelhada (a margem direita em wrap mostrava a borda esquerda da
     sala; medido com kirby andando no lobby: BG2 hofs 0→1→0, câmera
     travada por sala).
   - `black`: `gba::g_ws_pillarbox = 1` (pillarbox sólido).
   - **Gancho por frame**: o plugin arma `g_runtime_fn_entry_hook` e
     reafirma a política na entrada de `gf_VBlankIntr` (0x08152388;
     correção do endereço vs. nota acima: symbol_map aponta `0x08152388`,
     o `0x08152368` era a função vizinha), sobrevivendo ao fail-closed
     do pillarbox que o PPU recompõe ao carregar savestate
     (`gba_ppu.cpp:159`).

## Assinaturas de IO por cena (sondadas por TCP, build headless `--tcp`)

| Cena | DISPCNT (camadas) | Seats BG0..3 | Nota |
|---|---|---|---|
| Boot logos | — | — | pillarbox perfeito |
| File/Game select | BG0,1,2 on; BG3 off | 31/30/29 | WINOUT `00bf` |
| Cutscene/intro | **só BG2 + OBJ** | 30 | WINOUT `0000` |
| Sala de campo c/ HUD | BG0,1,3 on; BG2 off | 7/15/23/30 (tutorial) | seats variam por sala |
| Sala Rainbow (HUD) | BG0,1,3 on; BG2 off | var | HUD ok em wide |

A camada HUD não é sempre BG0 (na sala inicial era BG1) — **não** usar
DISPCNT como discriminador campo/menu. Câmera travada dentro de salas
(hofs 0/1), margens = borda repetida da mesma sala em wrap; sprites
(c/ X com sinal de 9 bits) continuam visíveis nas margens.

## Fase 3 (roadmap margens autorais)

Margem com conteúdo do **mundo vizinho** (grafo de portas do KAM:
salas conectadas por teleporte, não por adjacência em memória):
1. RE da rotina de desenho de metatiles (equiv. `DrawMetatileAt` do
   FireRed) e dos buffers residentes da sala;
2. adaptar o modelo do `ws_sidecar` (anel slot↔tile-mundo + cache
   estendido 64x32 + `g_ws_tilemap_provider`);
3. materializar salas vizinhas chamando a própria rotina do guest em
   scratch IWRAM (tail `0x03006CC4..0x03008000`, ~5,3 KB).

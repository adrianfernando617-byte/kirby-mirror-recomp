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
4. Margens autorais (fase 2): hook no streaming
   (0x08156266/0x08156E58) para materializar colunas extras + pillarbox
   por cena via `presentation_request` (menus, batalhas).

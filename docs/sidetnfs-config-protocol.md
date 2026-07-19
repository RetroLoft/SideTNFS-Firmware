# SIDETNFS config-protocol — voorlopig contract (Fase 9B1)

Status: **eerste, minimale proef**. Alleen `GET_CONFIG_INFO` is geïmplementeerd.
Geen serverstructuren, geen flashopslag, geen DNS, geen nieuwe TNFS-sessies.

## Command-ID

`GEMDRVEMUL_SIDETNFS_GET_CONFIG_INFO = (APP_GEMDRVEMUL << 8 | 0x0D) = 0x040D`

Gedefinieerd in `romemul/include/commands.h`. Subcommando `0x0D` binnen de
`APP_GEMDRVEMUL`-namespace (`0x04`), geverifieerd vrij in zowel de Pico-bron
(`romemul/include/commands.h`) als de referentie-Atari-driver
(`sidecart-gemdrive-atari/src/gemdrive.s`, CMD_*-tabel) — hoogste tot nu toe
gebruikte subcode is `0x0C` (`REENTRY_XBIOS_UNLOCK`) resp. `0x8B`
(`DTA_RELEASE_CALL`); `0x0D–0x18` is vrij op beide zijden.

## Requestpayload

Geen. 0 bytes. Het commando wijzigt geen configuratie, gebruikt geen SD,
WiFi, TNFS of flash, en sluit geen handles of sessies.

## Shared-memory locatie

Vast, 4-byte-uitgelijnd blok in het ROM3-gedeelde-geheugenvenster
(`memory_shared_address = ROM3_START_ADDRESS`), direct na het laatst
bewezen-gebruikte offset:

- `GEMDRVEMUL_EXEC_PD` (= `GEMDRVEMUL_SHARED_VARIABLES + 256`) is het laatst
  benoemde offset in `gemdrvemul.h`. Het enige schrijfaccess erop is
  `memcpy(pexec_pd, origin, sizeof(PD))` in `gemdrvemul.c`
  (`GEMDRVEMUL_PEXEC_CALL`/`GEMDRVEMUL_SAVE_BASEPAGE`), en `sizeof(PD) == 256`
  bytes (`struct _pd`, laatste member `p_cmdlin[128]` op offset `0x80`, dus
  struct eindigt exact op `0x100`). Geen enkele andere macro of letterlijke
  offset in `romemul/gemdrvemul.c` gaat verder dan
  `GEMDRVEMUL_EXEC_PD + 256`.
- Nieuw blok start dus op `GEMDRVEMUL_SIDETNFS_CONFIG = GEMDRVEMUL_EXEC_PD + 256`
  = offset `0x4398` (17304) in het 64 KB-venster (`CONFIGURATOR_SHARED_MEMORY_SIZE_BYTES`
  = 65536 bytes) — al 4-byte uitgelijnd, ~48 KB vrije ruimte erna.

| Veld | Offset-macro | Offset (dec/hex) | Grootte | Betekenis |
|---|---|---|---|---|
| protocolversie | `GEMDRVEMUL_SIDETNFS_CONFIG_VERSION` | 17304 / `0x4398` | 4 bytes | vast op `1` (`SIDETNFS_CONFIG_PROTOCOL_VERSION`) |
| max. serveraantal | `GEMDRVEMUL_SIDETNFS_CONFIG_MAX_SERVERS` | 17308 / `0x439C` | 4 bytes | vast op `8` (`SIDETNFS_CONFIG_MAX_SERVERS`) |
| huidig serveraantal | `GEMDRVEMUL_SIDETNFS_CONFIG_SERVER_COUNT` | 17312 / `0x43A0` | 4 bytes | voorlopig altijd `0` (geen flashopslag deze fase) |
| status | `GEMDRVEMUL_SIDETNFS_CONFIG_STATUS` | 17316 / `0x43A4` | 4 bytes | `0` = OK (`SIDETNFS_CONFIG_STATUS_OK`) |

Totaal responseblok: 16 bytes, vier vaste `uint32_t`-velden — geen variabele
lengte, geen strings, geen pointers.

## Byte-order (68000 ↔ RP2040) — gecorrigeerd in Fase 9B1-fix

RP2040 is little-endian, 68000 is big-endian. Een kale
`*((volatile uint32_t*)adres) = waarde;` laat de twee 16-bit helften van de
long in RP2040-native volgorde staan (laag 16-bit woord op het lage adres,
hoog 16-bit woord op adres+2). De Atari leest ditzelfde geheugen echter als
één big-endian `move.l` (hoog woord verwacht op het lage adres, laag woord
op adres+2) — dus zonder correctie komt elke long **woord-verwisseld** aan
(waarde `1` = `0x00000001` werd gelezen als `0x00010000` = 65536; alleen
bij symmetrische waarden als `0` of `0xFFFFFFFF` blijft de fout onzichtbaar,
wat de eerste versie van dit contract liet doorglippen).

De vier responsevelden gebruiken daarom, zoals de rest van dit protocol
al deed voor echte long-waarden (`GEMDRVEMUL_DFREE_STRUCT`, zie
`gemdrvemul.c:2459-2462`), de bestaande macro
`WRITE_AND_SWAP_LONGWORD(address, offset, data)` uit `memfunc.h:22-24` —
die swapt de twee 16-bit helften vóór het schrijven, zodat de resulterende
byte-layout in het gedeelde geheugen exact overeenkomt met wat een 68000
`move.l` verwacht. Geen wijziging aan het busprotocol zelf, geen
Atari-zijdige aanpassing nodig — alleen consistent gebruik van het al
bestaande, bewezen schrijfpatroon. (Tekstvelden zoals `ConfigEntry` blijven
apart `swap_data()` gebruiken — dat is een ander mechanisme voor
woordparen van ASCII-tekens, niet voor scalaire longs.)

## Random-token-handshake

Ongewijzigd, bestaand mechanisme (zie `write_random_token()`/
`generate_random_token_seed()` in `gemdrvemul.c:56-63`):
1. Atari genereert een 32-bit seed, stuurt die mee als onderdeel van de
   command-payload (ROM3 address-encoded write/read-sequentie).
2. Pico verwerkt het commando synchroon in de hoofd-dispatchlus, schrijft
   de vier responsevelden, en schrijft daarna dezelfde seed terug op
   `GEMDRVEMUL_RANDOM_TOKEN` (offset `0x0`).
3. Atari pollt op die vaste offset tot de waarde overeenkomt (of een eigen
   timeout verstrijkt — dat gedrag zit in de 68k-driver, niet in dit
   repository).
4. Pico reset `active_command_id` naar `0xFFFF` zodra de respons klaar
   staat, zodat een volgend commando geaccepteerd wordt.

## Statuscodes

- `0` = OK (`SIDETNFS_CONFIG_STATUS_OK`). Enige waarde die deze fase ooit
  teruggeeft — er is nog geen foutpad, want er wordt niets gelezen dat kan
  falen (geen flash, geen SD, geen netwerk).

## Protocolversie

`SIDETNFS_CONFIG_PROTOCOL_VERSION = 1`. Op te hogen zodra het responseformaat
ooit wijzigt (extra velden, andere velden, ander formaat).

# SIDETNFS config-protocol — contract (protocolversie 2, Fase 9C)

Status: **persistente drivelijst, nog niet actief in de runtime**. Eén
verplichte configdrive (alleen een driveletter) plus maximaal acht gewone
drives (SD of TNFS) leven in RAM en kunnen via
`GET_CONFIG_INFO`/`GET_DRIVE`/`SET_DRIVE`/`DELETE_DRIVE`/
`SET_CONFIG_DRIVE` gelezen en gewijzigd worden; `SAVE_CONFIG` is het enige
commando dat ooit naar flash schrijft. Nog niet geïmplementeerd: echte
gelijktijdige multi-drive-emulatie, en het laten meewegen van deze lijst
door de daadwerkelijke TNFS-runtime (die blijft hardcoded, zie
"Bestaande runtime" hieronder) — dat is Fase 9D of later.

Dit vervangt het Fase 9B2-model (max. 8 servers, geen configdrive) volledig
— dat model is nooit gecommit.

## Command-ID's

Alle SIDETNFS-configcommando's zitten in de `APP_GEMDRVEMUL`-namespace
(`0x04`), subcommando's `0x0D`–`0x12`, gedefinieerd in
`romemul/include/commands.h`. Geverifieerd vrij tegen zowel deze bron als de
referentie-Atari-driver (`sidecart-gemdrive-atari/src/gemdrive.s` CMD_*-tabel,
hoogste gebruikte code `0x0C`/`0x8B`); `0x0D`–`0x18` is vrij op beide zijden.

| Waarde | Constante | Betekenis |
|---|---|---|
| `0x040D` | `GEMDRVEMUL_SIDETNFS_GET_CONFIG_INFO` | protocolversie, capaciteit, huidige telling, configdriveletter |
| `0x040E` | `GEMDRVEMUL_SIDETNFS_GET_DRIVE` | lees één gewone drive |
| `0x040F` | `GEMDRVEMUL_SIDETNFS_SET_DRIVE` | schrijf/wijzig één gewone drive (RAM) |
| `0x0410` | `GEMDRVEMUL_SIDETNFS_DELETE_DRIVE` | wis één gewone drive (RAM) |
| `0x0411` | `GEMDRVEMUL_SIDETNFS_SET_CONFIG_DRIVE` | wijzig de configdriveletter (RAM) |
| `0x0412` | `GEMDRVEMUL_SIDETNFS_SAVE_CONFIG` | valideer + schrijf de volledige lijst naar flash |

`0x040E` verving het nooit-gecommitte Fase 9B2 `GEMDRVEMUL_SIDETNFS_GET_SERVER`
op hetzelfde codepunt — het serverlijst-model is vervangen, niet uitgebreid.

## Flashsector (ongewijzigd bewijs sinds Fase 9B2)

```c
#define SIDETNFS_CONFIG_FLASH_OFFSET 0x100000u
#define SIDETNFS_CONFIG_FLASH_SIZE   4096u
```

Opnieuw gecontroleerd tegen de huidige linker-/buildlayout
(`romemul/memmap_romemul.ld`) en de Pico W-flashgrootte:

- `ROM_FLASH` (`ORIGIN 0x100E0000, LENGTH 128k`) eindigt op exact
  `0x10100000 = XIP_BASE + 0x100000` — deze sector sluit er direct op aan,
  zonder gat of overlap.
- `CONFIG_FLASH` (`0x100DE000`, 8k) en `ROM_FLASH` zijn de enige andere
  benoemde flash-`MEMORY`-regio's onder `0x100000`; geen van beide wordt
  door een `SECTIONS`-regel daadwerkelijk gebruikt (puur
  documentatie/reservering) — `SIDETNFS_CONFIG_FLASH` volgt dezelfde stijl.
- Geen enkele runtime flash-schrijfoperatie elders in deze repo
  (`config.c`, `romloader.c`, `network.c`, `filesys.c`) raakt `0x100000` of
  hoger aan.
- `__flash_binary_end` van de gebouwde firmware zit rond offset `0x86xxx`
  (~540KB) — ruim onder zowel `CONFIG_FLASH_OFFSET` (`0xDE000`) als
  `0x100000`.
- Board is expliciet `pico_w`; `PICO_FLASH_SIZE_BYTES` in de meegeleverde
  pico-sdk (`pico-sdk/src/boards/include/boards/pico_w.h`) bevestigt de
  standaard 2MB-flashchip voor dit board (niet fysiek los bevraagd).

## Datamodel

`romemul/include/sidetnfs_config.h`:

```c
#define SIDETNFS_MAX_DRIVES     8   // gewone drives; de configdrive telt niet mee
#define SIDETNFS_NICKNAME_LEN  24
#define SIDETNFS_HOST_LEN      64
#define SIDETNFS_MOUNTPATH_LEN 32
#define SIDETNFS_SDPATH_LEN    64

typedef enum {
    SIDETNFS_DRIVE_SD   = 1,
    SIDETNFS_DRIVE_TNFS = 2
} sidetnfs_drive_type_t;

typedef enum {
    SIDETNFS_TRANSPORT_UDP = 0,
    SIDETNFS_TRANSPORT_TCP = 1  // opgeslagen als toekomstige waarde; nog niet functioneel, UI kiest alleen UDP
} sidetnfs_transport_t;

#define SIDETNFS_CONFIG_MAGIC         0x53544446u  // "STDF"
#define SIDETNFS_CONFIG_FLASH_VERSION 2u

typedef struct {
    uint8_t  used;
    uint8_t  drive_letter;   // uppercase ASCII, bv. 'N'
    uint8_t  type;
    uint8_t  transport;
    uint16_t port;
    uint8_t  reserved0[2];

    char nickname[24];
    char host[64];
    char mount_path[32];
    char sd_path[64];
} sidetnfs_drive_config_t;  // sizeof == 192 bytes, compile-time geverifieerd, geen union

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t  config_drive_letter;
    uint8_t  drive_count;   // aantal used-records in drives[]; sluit de configdrive uit
    uint8_t  reserved[2];
    sidetnfs_drive_config_t drives[8];
    uint32_t crc32;
} sidetnfs_drive_flash_t;  // sizeof == 1552 bytes (12 + 8*192 + 4), compile-time geverifieerd
```

SD gebruikt `nickname`/`drive_letter`/`sd_path`; TNFS gebruikt
`nickname`/`drive_letter`/`transport`/`host`/`port`/`mount_path`. Niet-
relevante velden staan altijd op nul (afgedwongen door
`sidetnfs_config_set_drive()` en opnieuw door `sidetnfs_config_save()`).
Geen pointers, geen union — een vast, pointervrij record per het
firmwareprotocol.

Twee `_Static_assert`s bewaken de recordgrootte (`192`) en de totale
structgrootte (`1552`, ruim binnen de 4KB-sector).

## Laden en valideren (`sidetnfs_config_init()`)

Precies één keer aangeroepen, in `main.c` vlak vóór `init_gemdrvemul()` in
de `GEMDRIVE_EMULATOR`-tak — dus altijd vóór GEMDRIVE deze commando's kan
verwerken. Alleen een XIP-flashlezing; geen `flash_range_erase`/
`flash_range_program` in dit pad.

Validatievolgorde (bij één ongeldige stap wordt het **hele** blok verworpen
— nooit een gedeeltelijk geldig/corrupt record gebruikt):

1. `magic == SIDETNFS_CONFIG_MAGIC` (`"STDF"`) — oude Fase 9B2 `"STNF"`-flash
   (nooit gecommit, maar mogelijk al eens geflashed tijdens ontwikkeling)
   faalt hier vanzelf en valt terug op de defaults hieronder.
2. `version == SIDETNFS_CONFIG_FLASH_VERSION` (`2`)
3. CRC32 over het hele blok exclusief het `crc32`-veld zelf
4. `config_drive_letter` geldig (hoofdletter, niet A/B) en
   `drive_count <= SIDETNFS_MAX_DRIVES`
5. Voor iedere `used`-record: geldig type, type-specifieke velden geldig
   (zie Validatie hieronder), driveletter geldig en niet gelijk aan
   `config_drive_letter`, en uniek t.o.v. alle andere `used`-records
6. `drive_count` == het werkelijke aantal `used`-records

Bij een ongeldige of lege flashsector wordt in RAM deze standaardconfig
opgebouwd (nooit automatisch naar flash geschreven):

```
config_drive_letter: 'S'
drive_count: 1
drives[0]:
  used:         1
  drive_letter: 'N'
  type:         TNFS
  nickname:     "RetroLoft"
  transport:    UDP
  host:         "192.168.178.10"
  port:         16384
  mount_path:   "Atari.ST"
  sd_path:      leeg
```

Alle strings worden zowel op het flash-pad als het defaultpad expliciet op
`NUL` geforceerd op hun laatste byte, ongeacht wat er op flash stond.

CRC32: dezelfde bit-voor-bit IEEE 802.3/zlib-methode als Fase 9B2 (polynoom
`0xEDB88320`, init/eind-XOR `0xFFFFFFFF`), geen tabel.

## `GET_CONFIG_INFO` (`0x040D`)

Requestpayload: geen. Geen SD/WiFi/TNFS/flash-toegang.

Responseblok (`GEMDRVEMUL_SIDETNFS_CONFIG`, in het 64KB ROM3-gedeelde-
geheugenvenster, direct na `GEMDRVEMUL_EXEC_PD + 256` — zelfde bewezen-vrije
locatie als Fase 9B1/9B2): vijf `uint32_t`-velden, allemaal via
`WRITE_AND_SWAP_LONGWORD` (bewezen Atari-compatibele longword-woordvolgorde,
zie Fase 9B1-fix).

| Veld | Offset-macro | Betekenis |
|---|---|---|
| protocolversie | `GEMDRVEMUL_SIDETNFS_CONFIG_VERSION` | vast op `2` |
| max. gewone drives | `GEMDRVEMUL_SIDETNFS_CONFIG_MAX_DRIVES` | vast op `8` |
| huidig aantal gewone drives | `GEMDRVEMUL_SIDETNFS_CONFIG_DRIVE_COUNT` | uit RAM-config |
| configdriveletter | `GEMDRVEMUL_SIDETNFS_CONFIG_DRIVE_LETTER` | ASCII-code, uit RAM-config |
| status | `GEMDRVEMUL_SIDETNFS_CONFIG_STATUS` | `0` = OK |

Totaal: 20 bytes.

## Gedeeld drive-responseblok

`GEMDRVEMUL_SIDETNFS_DRIVE`, direct na het 20-byte `GET_CONFIG_INFO`-blok —
zelfde bewijs van vrije ruimte (niets in `gemdrvemul.c` schrijft ooit
voorbij dit venster; ~48KB blijft over in het 64KB-venster). Dit ene blok
wordt gedeeld door `GET_DRIVE` (volledig) en door
`SET_DRIVE`/`DELETE_DRIVE`/`SET_CONFIG_DRIVE`/`SAVE_CONFIG` (alleen het
`STATUS`-veld — geen van die vier heeft de rest tegelijk nodig).

| Veld | Offset-macro | Grootte | Woordvolgorde |
|---|---|---|---|
| status | `GEMDRVEMUL_SIDETNFS_DRIVE_STATUS` | 4 bytes | `uint32_t`, `WRITE_AND_SWAP_LONGWORD` |
| used | `GEMDRVEMUL_SIDETNFS_DRIVE_USED` | 2 bytes | `uint16_t`, `WRITE_WORD` (los woord, geen swap nodig) |
| drive_letter | `GEMDRVEMUL_SIDETNFS_DRIVE_LETTER` | 2 bytes | `uint16_t`, `WRITE_WORD` (ASCII-code) |
| type | `GEMDRVEMUL_SIDETNFS_DRIVE_TYPE` | 2 bytes | `uint16_t`, `WRITE_WORD` (0=leeg/ongeldig, 1=SD, 2=TNFS) |
| transport | `GEMDRVEMUL_SIDETNFS_DRIVE_TRANSPORT` | 2 bytes | `uint16_t`, `WRITE_WORD` |
| port | `GEMDRVEMUL_SIDETNFS_DRIVE_PORT` | 2 bytes | `uint16_t`, `WRITE_WORD` |
| nickname | `GEMDRVEMUL_SIDETNFS_DRIVE_NICKNAME` | 24 | byte-kopie + `CHANGE_ENDIANESS_BLOCK16` in-place |
| host | `GEMDRVEMUL_SIDETNFS_DRIVE_HOST` | 64 | byte-kopie + `CHANGE_ENDIANESS_BLOCK16` in-place |
| mount_path | `GEMDRVEMUL_SIDETNFS_DRIVE_MOUNT_PATH` | 32 | byte-kopie + `CHANGE_ENDIANESS_BLOCK16` in-place |
| sd_path | `GEMDRVEMUL_SIDETNFS_DRIVE_SD_PATH` | 64 | byte-kopie + `CHANGE_ENDIANESS_BLOCK16` in-place |

Totaal: 198 bytes. Geen directe `memcpy()` van de flashstruct — dit is een
expliciet, onafhankelijk wire-formaat. De string-aanpak is hetzelfde
patroon dat `populate_dta()` al gebruikt voor Pico→Atari-bestandsnamen.

**Fase 9C-R (hardware-bevestigd):** een tussentijdse revisie verwijderde
`CHANGE_ENDIANESS_BLOCK16` hier op basis van een host-only simulatie die
Atari-leesacties modelleerde als kale, ongewijzigde bytelezingen van
Pico-RAM. Hardwaretest weerlegde dit direct: zonder de swap kwam
"RetroLoft" terug als "eRtLofo" — een exacte paarsgewijze byteverwisseling.
De echte ROM3-bus verwisselt kennelijk bytes binnen een 16-bit woord bij
byte-granulaire lezingen (68000 UDS/LDS-byteselectie tegen de RP2040's
little-endian opslag) — iets wat de hostsimulatie niet modelleerde.
`WRITE_WORD`/`WRITE_AND_SWAP_LONGWORD`-velden zijn hier niet door geraakt
(één woord/long-bustransactie, geen reeks losse bytelezingen). De swap is
dus vereist en is hersteld; zie ook `romemul/gemdrvemul.c`'s commentaar bij
`GEMDRVEMUL_SIDETNFS_GET_DRIVE`.

## `GET_DRIVE` (`0x040E`)

Request: één `uint32_t index` (0–7, alleen gewone drives — de configdrive
heeft geen index, zie `GET_CONFIG_INFO`'s `config_drive_letter`).

Response: het volledige blok hierboven. `INVALID_INDEX` als
`index >= SIDETNFS_MAX_DRIVES`; `EMPTY_SLOT` als de slot niet `used` is
(blok dan volledig op nul); anders `OK` met het record ingevuld.

## `SET_DRIVE` (`0x040F`)

Wijzigt uitsluitend de RAM-kopie — geen flashwrite.

Requestpayload: `uint32_t index`, gevolgd door exact dezelfde veldvolgorde
als `GET_DRIVE`'s response (zonder `status`): `used`, `drive_letter`,
`type`, `transport`, `port` (elk één 16-bit woord), dan `nickname`/`host`/
`mount_path`/`sd_path` (woord-voor-woord, zelfde
`COPY_AND_CHANGE_ENDIANESS_BLOCK16`-conventie als
`GEMDRVEMUL_FOPEN_CALL`/`DSETPATH_CALL` al gebruiken om Atari→Pico-strings
te lezen — geen nieuw mechanisme).

Gedrag:
- `index >= SIDETNFS_MAX_DRIVES` → `INVALID_INDEX`.
- `used == 0` → de slot wordt volledig gewist (equivalent aan
  `DELETE_DRIVE` voor die index); geen type-specifieke veldvalidatie.
- `used != 0` → volledige validatie (zie Validatie hieronder) plus
  uniciteitscontrole van `drive_letter` tegen alle andere `used`-drives
  én tegen de huidige configdriveletter; bij succes worden
  niet-relevante velden voor het type op nul gezet vóór opslag in RAM.

Response: alleen `status` (`GEMDRVEMUL_SIDETNFS_DRIVE_STATUS`).

## `DELETE_DRIVE` (`0x0410`)

Request: `uint32_t index`. Wist het record volledig in RAM en herberekent
`drive_count`. `INVALID_INDEX` als index buiten bereik, `EMPTY_SLOT` als de
slot al niet `used` was. Kan de configdrive nooit raken — die heeft geen
index in deze array. Response: alleen `status`.

## `SET_CONFIG_DRIVE` (`0x0411`)

Request: `uint32_t new_config_drive_letter` (ASCII-code). Wijzigt
uitsluitend `config_drive_letter` in RAM. Weigert A/B
(`INVALID_DRIVE_LETTER`) en elke letter die al aan een `used` gewone drive
toegewezen is (`DUPLICATE_DRIVE_LETTER`). Response: alleen `status`.

## `SAVE_CONFIG` (`0x0412`)

Request: geen. Het enige commando dat ooit flash aanraakt:

1. herberekent `drive_count` uit de `used`-bitmap van de RAM-config;
2. valideert de volledige RAM-config (configdriveletter, ieder
   `used`-record, volledige letter-uniciteit) — bij een fout wordt er
   niets gewist of geschreven, en de validatiefoutcode wordt teruggegeven;
3. bouwt een schone kopie: `reserved`-bytes op nul, ongebruikte records
   volledig op nul, niet-relevante velden per type op nul, strings
   NUL-getermineerd;
4. berekent de CRC32 over die schone kopie;
5. schakelt interrupts alleen uit rond het erase+program-paar
   (`save_and_disable_interrupts()`/`restore_interrupts()`, exact het
   patroon dat `romemul/config.c`'s `write_all_entries()` al gebruikt);
6. `flash_range_erase()` van precies één 4KB-sector
   (`SIDETNFS_CONFIG_FLASH_OFFSET`/`SIZE`);
7. `flash_range_program()` van alleen de benodigde, op een 256-byte
   flash-pagina afgeronde byte-hoeveelheid (1552 bytes → 1792 bytes/7
   pagina's — nooit de hele sector);
8. leest terug uit XIP-flash en valideert magic, versie en CRC32 opnieuw;
9. rapporteert pas ná die verificatie succes; bij een mislukte
   readback-verificatie wordt `FLASH_WRITE_FAILED` (magic/versie mismatch)
   of `CRC_MISMATCH` teruggegeven — geen automatische retry, geen reboot,
   de Pico blijft responsief.

Verandert de momenteel actieve TNFS-sessie niet — een reboot is nodig
voordat toekomstige runtimecode de opgeslagen lijst gebruikt. Response:
alleen `status`.

## Validatie (details)

- `config_drive_letter`/elke `drive_letter`: hoofdletter A–Z, nooit `A`/`B`.
- Elke `used` gewone drive heeft een unieke letter, en gebruikt nooit de
  configdriveletter.
- `drive_count` moet (na herberekening bij `SAVE_CONFIG`) gelijk zijn aan
  het werkelijke aantal `used`-records.
- Onbekend `type` (niet SD, niet TNFS) → `INVALID_TYPE`.
- TNFS: `transport` moet UDP of TCP zijn (`INVALID_TRANSPORT`), `port != 0`
  (`INVALID_PORT`), `host` niet leeg (`INVALID_HOST`), `mount_path` niet
  leeg (`INVALID_MOUNT_PATH`).
- SD: `sd_path` niet leeg (`INVALID_SD_PATH`).
- Alle strings worden defensief op hun laatste byte op `NUL` geforceerd,
  ook wanneer de bron al correct leek.
- Bij `SAVE_CONFIG`: `reserved`/`reserved0`-bytes altijd op nul,
  ongebruikte records volledig op nul.

## Statuscodes

| Waarde | Constante |
|---|---|
| 0 | `SIDETNFS_CONFIG_STATUS_OK` |
| 1 | `SIDETNFS_CONFIG_STATUS_INVALID_INDEX` |
| 2 | `SIDETNFS_CONFIG_STATUS_EMPTY_SLOT` |
| 3 | `SIDETNFS_CONFIG_STATUS_INVALID_DRIVE_LETTER` |
| 4 | `SIDETNFS_CONFIG_STATUS_DUPLICATE_DRIVE_LETTER` |
| 5 | `SIDETNFS_CONFIG_STATUS_INVALID_TYPE` |
| 6 | `SIDETNFS_CONFIG_STATUS_INVALID_TRANSPORT` |
| 7 | `SIDETNFS_CONFIG_STATUS_INVALID_PORT` |
| 8 | `SIDETNFS_CONFIG_STATUS_INVALID_HOST` |
| 9 | `SIDETNFS_CONFIG_STATUS_INVALID_MOUNT_PATH` |
| 10 | `SIDETNFS_CONFIG_STATUS_INVALID_SD_PATH` |
| 11 | `SIDETNFS_CONFIG_STATUS_TOO_MANY_DRIVES` (via `SET_DRIVE` structureel onbereikbaar — index is altijd 0–7 in de vaste 8-slot array; gedefinieerd voor volledigheid en een eventueel toekomstig bulk-commando) |
| 12 | `SIDETNFS_CONFIG_STATUS_FLASH_WRITE_FAILED` |
| 13 | `SIDETNFS_CONFIG_STATUS_CRC_MISMATCH` |
| 14 | `SIDETNFS_CONFIG_STATUS_UNSUPPORTED_VERSION` (gereserveerd; deze fase heeft nog geen los pad dat dit teruggeeft — `GET_CONFIG_INFO` meldt altijd versie 2) |

## Random-token-handshake

Ongewijzigd, bestaand mechanisme (zie `write_random_token()`/
`generate_random_token_seed()` in `gemdrvemul.c`): de Atari genereert een
32-bit seed als onderdeel van de command-payload, de Pico verwerkt het
commando synchroon, schrijft de respons, schrijft daarna dezelfde seed
terug op `GEMDRVEMUL_RANDOM_TOKEN` (offset 0) als "klaar"-signaal, en reset
`active_command_id` naar `0xFFFF`.

## Bestaande runtime

De daadwerkelijke TNFS-client (`sidetnfs_probe.c`) gebruikt deze fase nog
steeds zijn eigen hardcoded waarden — **niet** de RAM- of flash-drivelijst:

```
N:
192.168.178.10
UDP 16384
Atari.ST
```

Deze fase voegt geen multi-drivegedrag toe en wijzigt geen open handles,
DTA's, TNFS-sessies of `hd_folder`. Het laten meewegen van de opgeslagen
lijst door de daadwerkelijke runtime (en het daadwerkelijk activeren van
meerdere drives) is toekomstig werk, ná een reboot met de nieuwe flash-
inhoud.

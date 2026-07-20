# SIDETNFS config-protocol — contract (protocolversie 2, Fase 9C/9D)

Status: **persistente drivelijst, sinds Fase 9D actief voor de eerste
TNFS-drive**. Eén verplichte configdrive (alleen een driveletter) plus
maximaal acht gewone drives (SD of TNFS) leven in RAM en kunnen via
`GET_CONFIG_INFO`/`GET_DRIVE`/`SET_DRIVE`/`DELETE_DRIVE`/
`SET_CONFIG_DRIVE` gelezen en gewijzigd worden; `SAVE_CONFIG` is het enige
commando dat ooit naar flash schrijft. Sinds Fase 9D bepaalt de eerste
gebruikte TNFS/UDP-drive uit deze lijst bij boot de daadwerkelijke actieve
server EN de GEMDRIVE-driveletter (zie "Bestaande runtime" hieronder) —
wijzigingen via `SIDETNFS.PRG` worden pas actief na een herstart. Nog niet
geïmplementeerd: echte gelijktijdige multi-drive-emulatie (nog steeds maar
één actieve TNFS-drive tegelijk).

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

## Bestaande runtime (Fase 9D)

De daadwerkelijke TNFS-client (`sidetnfs_probe.c`) gebruikt niet langer
hardcoded compile-time constanten. Bij boot roept `main.c` (na
`sidetnfs_config_init()`, vóór `init_gemdrvemul()`) éénmalig
`sidetnfs_probe_load_active_server()` aan: deze doorloopt de RAM-drivelijst
(index 0..`SIDETNFS_MAX_DRIVES-1`) en neemt de **eerste** `used`-record met
`type == SIDETNFS_DRIVE_TNFS` én `transport == SIDETNFS_TRANSPORT_UDP` over
als actieve server (`host`, `port`, `mount_path`, `drive_letter`). TCP-drives
worden overgeslagen (TCP blijft expliciet niet-ondersteund) — het scannen
gaat door naar de volgende gebruikte drive. Bij lege/ongeldige config of
alleen SD/TCP-drives blijft de actieve server "niet geconfigureerd": elke
netwerkaanroep in `sidetnfs_probe.c` heeft al een bestaande
`if (!ipaddr_aton(...))`-controle die dan gewoon zijn bestaande, veilige
faalpad neemt (`ipaddr_aton("")` faalt altijd) — geen extra guards nodig,
GEMDRIVE blijft zonder WiFi/netwerk volledig responsief (zelfde gedrag als
`sidetnfs_mark_network_skipped()`).

De GEMDRIVE-driveletter komt sindsdien ook uit deze actieve server
(`sidetnfs_probe_get_active_drive_letter()`) in plaats van uitsluitend uit
`PARAM_GEMDRIVE_DRIVE` (`gemdrvemul.c`, `init_gemdrvemul()`) — met
`PARAM_GEMDRIVE_DRIVE`/`'C'` als terugval wanneer geen bruikbare TNFS/UDP-
drive gevonden is.

Deze fase voegt geen multi-drivegedrag toe (nog steeds maar één actieve
TNFS-drive) en wijzigt geen open handles, DTA's, TNFS-sessies of
`hd_folder` tijdens een lopende sessie — een wijziging via `SIDETNFS.PRG`
(`SET_DRIVE`/`SAVE_CONFIG`) wordt pas bij de eerstvolgende boot opgepikt,
omdat `sidetnfs_probe_load_active_server()` alleen bij boot draait.
`SIDETNFS_ENABLE_SD_SUPPORT` blijft een onafhankelijke compile-time schakelaar
(SD/FatFS-toegang voor WiFi-wachtwoord/DEBUG.TXT), losstaand van deze
TNFS-serverkeuze.

# WiFi/netwerkconfiguratie via GEMDRIVE (Fase 11A)

Status: **nieuw, backwards compatible** — geen wijziging aan protocolversie
2 of aan de bestaande drive-records/commando's hierboven. Maakt WiFi/
netwerkconfiguratie (SSID, wachtwoord, auth-mode, land, DHCP/statisch IP)
bereikbaar vanuit `SIDETNFS.PRG` terwijl GEMDRIVE draait, zonder de
bestaande `APP_CONFIGURATOR`-commando's (`GET_CONFIG`/`PUT_CONFIG_STRING`/
`PUT_CONFIG_INTEGER`/`PUT_CONFIG_BOOL`/`SAVE_CONFIG`, afgehandeld in
`romemul/romloader.c`) aan te raken of ermee te routeren — twee volledig
gescheiden command-ID-namespaces boven dezelfde opslag.

Hergebruikt de bestaande `configData`/`PARAM_WIFI_*`-entries
(`romemul/config.c`) en de bestaande 8KB `CONFIG_FLASH`-sector — geen
tweede permanente netwerkconfigsector.

## Command-ID's

Subcommando's `0x13`–`0x15` in de `APP_GEMDRVEMUL`-namespace. Opnieuw
geverifieerd vrij: hoogste gebruikte lage code vóór deze toevoeging was
`0x12`/`SAVE_CONFIG`, eerstvolgende gebruikte code is `0x19`/`DGETDRV_CALL`
— `0x13`–`0x18` was en is vrij, zowel in `romemul/include/commands.h` als in
de referentie-Atari-driver (`sidecart-gemdrive-atari/src/gemdrive.s`
CMD_*-tabel, niets gedefinieerd op `0x13`–`0x15`).

| Waarde | Constante | Betekenis |
|---|---|---|
| `0x0413` | `GEMDRVEMUL_SIDETNFS_GET_NETWORK_CONFIG` | lees de huidige WiFi/netwerkconfig |
| `0x0414` | `GEMDRVEMUL_SIDETNFS_SET_NETWORK_CONFIG` | valideer + stage een nieuwe config (RAM only) |
| `0x0415` | `GEMDRVEMUL_SIDETNFS_SAVE_NETWORK_CONFIG` | valideer de staging-copy opnieuw + schrijf naar flash |

## Wire-record (`sidetnfs_network_config_t`, `romemul/include/sidetnfs_netconfig.h`)

```c
typedef struct {
    uint16_t auth_mode;                  // 0-8, zie authenticatiemapping hieronder
    uint16_t use_dhcp;                   // 0 of 1
    char ssid[36];                       // MAX_SSID_LENGTH uit network.h, <=32 bytes inhoud
    char password[68];                   // MAX_PASSWORD_LENGTH uit network.h, <=64 bytes inhoud
    char country[4];                     // exact 2 letters + NUL + 1 padding, zelfde conventie als ConnectionData.wifi_country
    char ip_address[16];                 // IPV4_ADDRESS_LENGTH uit network.h
    char netmask[16];
    char gateway[16];
    char primary_dns[16];
} sidetnfs_network_config_t;             // sizeof == 176 bytes, _Static_assert bewaakt
```

Alle veldlengtes zijn identiek aan bestaande, al elders in deze codebase
gebruikte constanten (`MAX_SSID_LENGTH`/`MAX_PASSWORD_LENGTH`/
`IPV4_ADDRESS_LENGTH`/`wifi_country[4]` in `romemul/include/network.h`) —
geen nieuwe, losstaande maten verzonnen. Geen enkel veld heeft
compiler-padding nodig (elk veld is `uint16_t` of een array met even
lengte); dit struct wordt nooit als geheel op het gedeelde geheugen
gecastst — elk shared-memory-offset is expliciet gedefinieerd in
`romemul/include/gemdrvemul.h` (zelfde stijl als het bestaande
drive-record hierboven).

## Gedeeld netwerk-responseblok

`GEMDRVEMUL_SIDETNFS_NETWORK`, direct na het bestaande 198-byte
drive-record (`GEMDRVEMUL_SIDETNFS_DRIVE_SD_PATH + SIDETNFS_SDPATH_LEN`,
offset 17522 van het 64KB ROM3-venster — ~46KB blijft over). Gedeeld door
`GET_NETWORK_CONFIG` (volledig) en door `SET_NETWORK_CONFIG`/
`SAVE_NETWORK_CONFIG` (alleen `STATUS`).

| Veld | Offset-macro | Grootte | Woordvolgorde |
|---|---|---|---|
| status | `GEMDRVEMUL_SIDETNFS_NETWORK_STATUS` | 4 | `uint32_t`, `WRITE_AND_SWAP_LONGWORD` |
| auth_mode | `GEMDRVEMUL_SIDETNFS_NETWORK_AUTH_MODE` | 2 | `uint16_t`, `WRITE_WORD` (los woord, geen swap) |
| use_dhcp | `GEMDRVEMUL_SIDETNFS_NETWORK_USE_DHCP` | 2 | `uint16_t`, `WRITE_WORD` |
| ssid | `GEMDRVEMUL_SIDETNFS_NETWORK_SSID` | 36 | byte-kopie + `CHANGE_ENDIANESS_BLOCK16` in-place |
| password | `GEMDRVEMUL_SIDETNFS_NETWORK_PASSWORD` | 68 | byte-kopie + `CHANGE_ENDIANESS_BLOCK16` in-place |
| country | `GEMDRVEMUL_SIDETNFS_NETWORK_COUNTRY` | 4 | byte-kopie + `CHANGE_ENDIANESS_BLOCK16` in-place |
| ip_address | `GEMDRVEMUL_SIDETNFS_NETWORK_IP_ADDRESS` | 16 | byte-kopie + `CHANGE_ENDIANESS_BLOCK16` in-place |
| netmask | `GEMDRVEMUL_SIDETNFS_NETWORK_NETMASK` | 16 | byte-kopie + `CHANGE_ENDIANESS_BLOCK16` in-place |
| gateway | `GEMDRVEMUL_SIDETNFS_NETWORK_GATEWAY` | 16 | byte-kopie + `CHANGE_ENDIANESS_BLOCK16` in-place |
| primary_dns | `GEMDRVEMUL_SIDETNFS_NETWORK_DNS` | 16 | byte-kopie + `CHANGE_ENDIANESS_BLOCK16` in-place |

Totaal: 180 bytes (4 status + 176 `sidetnfs_network_config_t`).

**Endianness — zelfde hardware-bewezen regels als het drive-record
hierboven, ongewijzigd op basis van hosttests alleen:** Pico→Atari
stringvelden gebruiken byte-kopie + `CHANGE_ENDIANESS_BLOCK16` in-place
(zelfde patroon als `populate_dta()` en `GET_DRIVE`); Atari→Pico
stringvelden gebruiken `COPY_AND_CHANGE_ENDIANESS_BLOCK16` (zelfde patroon
als `SET_DRIVE`/`GEMDRVEMUL_FOPEN_CALL`/`DSETPATH_CALL`). `auth_mode`/
`use_dhcp` zijn gewone 16-bit woorden (`WRITE_WORD`/`GET_PAYLOAD_PARAM16`,
geen swap) — zelfde conventie als de drive-record-velden `used`/
`drive_letter`/`type`/`transport`/`port`.

## `GET_NETWORK_CONFIG` (`0x0413`)

Requestpayload: geen. Geen SD/WiFi/flash-I/O — `sidetnfs_netconfig_get()`
roept uitsluitend `find_entry()` aan (pure RAM-lookup tegen de bestaande
`configData`). Response: het volledige blok hierboven, status altijd `OK`.
Een lege opgeslagen `WIFI_COUNTRY`-entry wordt genormaliseerd naar `"XX"`
voor weergave/bewerking (de flash-entry zelf blijft ongewijzigd). Geeft het
bestaande `WIFI_PASSWORD` ongewijzigd terug — `SIDETNFS.PRG` moet dit
kunnen uitlezen en terugschrijven.

## `SET_NETWORK_CONFIG` (`0x0414`)

Requestpayload: exact dezelfde veldvolgorde als `GET_NETWORK_CONFIG`'s
response (zonder `status`): `auth_mode`, `use_dhcp` (elk één 16-bit woord),
dan `ssid`/`password`/`country`/`ip_address`/`netmask`/`gateway`/
`primary_dns` (woord-voor-woord, `COPY_AND_CHANGE_ENDIANESS_BLOCK16`).

Valideert eerst **alles** (zie Validatie hieronder); alleen bij
`SIDETNFS_NETCONFIG_STATUS_OK` wordt de RAM-only staging-copy
overschreven. Bij een fout blijft de vorige staging-copy (indien aanwezig)
volledig intact — `configData`, flash en de actieve netwerkverbinding
worden hoe dan ook nooit aangeraakt. Response: alleen `status`.

## `SAVE_NETWORK_CONFIG` (`0x0415`)

Request: geen. Het enige commando in dit blok dat ooit flash aanraakt:

1. `NOT_STAGED` als er sinds boot nooit een succesvolle `SET_NETWORK_CONFIG`
   is geweest;
2. valideert de staging-copy **opnieuw** volledig (defense in depth);
3. bouwt een schone lokale kopie (NUL-terminatie afgedwongen, `country`
   uppercase);
4. werkt de negen bestaande `PARAM_WIFI_*`-`configData`-entries bij via
   `put_integer()`/`put_bool()`/`put_string()`;
5. roept `write_all_entries()` aan (zie de Fase-11A-uitlijningsfix
   hieronder) — schrijft de bestaande 8KB `CONFIG_FLASH`-sector;
6. leest de negen entries terug via een echte XIP-herlezing
   (`XIP_BASE + CONFIG_FLASH_OFFSET`, niet de al-bijgewerkte
   in-RAM-`configData`) en vergelijkt elk van de negen velden
   byte-voor-byte;
7. rapporteert alleen `OK` na een exacte match; anders
   `FLASH_WRITE_FAILED` (schrijf zelf mislukt) of `FLASH_VERIFY_FAILED`
   (readback komt niet overeen).

**Wanneer wordt de nieuwe configuratie actief?** Nooit tijdens deze
sessie — `SAVE_NETWORK_CONFIG` verbreekt of herstart de actieve
WiFi-verbinding niet terwijl `SIDETNFS.PRG` draait. De nieuwe configuratie
wordt pas opgepikt via een latere, aparte apply-/herinitialisatiepad (nog
niet onderdeel van deze fase) — in de praktijk vandaag: een Pico-reboot,
zelfde moment waarop de bestaande `PARAM_WIFI_*`-config al gelezen wordt
door `network_wifi_init()`/`main.c`.

## `write_all_entries()`-uitlijningsfix (`romemul/config.c`)

Vóór deze fase riep `write_all_entries()` `flash_range_program()` aan met
`sizeof(ConfigData)` (4136 bytes op dit target) als lengte — geen
veelvoud van `FLASH_PAGE_SIZE` (256). De Pico SDK's eigen
`invalid_params_if(FLASH, count & (FLASH_PAGE_SIZE-1))` documenteert deze
eis, maar die assertie compileert standaard weg
(`PARAM_ASSERTIONS_ENABLE_FLASH` staat nergens aan), dus dit bleef tot nu
toe onopgemerkt. Opgelost naar exact hetzelfde patroon dat
`sidetnfs_config.c`'s `sidetnfs_config_save()` al gebruikt voor zijn eigen,
aparte sector: een statische, op een 256-byte-pagina afgeronde, met nullen
opgevulde bufer (`4136` → `4352` bytes, ruim binnen de 8KB
`CONFIG_FLASH_SIZE`) — geen buffer van meerdere kilobytes op de 4KB stack.
Dit is een generieke fix in gedeelde code: ze geldt voor zowel de
bestaande `APP_CONFIGURATOR`/`SAVE_CONFIG`-route als voor
`SAVE_NETWORK_CONFIG` hierboven.

## Authenticatiemapping (`auth_mode`, bevestigd, niet aangenomen)

Onderzocht in `romemul/network.c`'s `get_auth_pico_code()` (de enige
bestaande, functionerende definitie van wat deze negen waarden betekenen)
én in de Atari-zijde (`AtariConfig`/`sidecart-configurator-atari`,
read-only geraadpleegd). **Bevinding: de Atari-zijde heeft momenteel geen
auth-mode-concept** — `AtariConfig/include/netconfig.h`'s `NetConfig`
(Fase AC-5, lokale UI zonder wire-protocol) heeft geen `auth_mode`-veld en
`AtariConfig/src/dialog.c`'s WiFi-dialoog heeft geen auth-keuze-widget; er
is dus geen bestaande Atari-zijdige waarde om tegen te bevestigen. De
onderstaande mapping is daarom uitsluitend gebaseerd op de Pico-zijdige,
functionerende code:

| Opgeslagen waarde (`WIFI_AUTH`/`auth_mode`) | CYW43-constante |
|---|---|
| 0 | `CYW43_AUTH_OPEN` |
| 1, 2 | `CYW43_AUTH_WPA_TKIP_PSK` |
| 3, 4, 5 | `CYW43_AUTH_WPA2_AES_PSK` |
| 6, 7, 8 | `CYW43_AUTH_WPA2_MIXED_PSK` |
| overig | `CYW43_AUTH_OPEN` (bestaande `default`-tak in `get_auth_pico_code()`) |

`sidetnfs_netconfig_validate()` wijst elke waarde `> 8` af
(`INVALID_AUTH_MODE`) — strenger dan `get_auth_pico_code()`'s eigen
lenient-fallback-naar-OPEN, om te voorkomen dat de configdrive stilzwijgend
een niet-bestaande auth-mode accepteert. **Aanbeveling voor
AtariConfig-Claude:** voeg een auth-mode-keuze toe aan de WiFi-dialoog met
exact deze vijf groepen/negen waarden, aangezien dit de enige
geverifieerde betekenis is.

## Validatie (details)

- `ssid`: max. 32 bytes inhoud, leeg toegestaan (schakelt WiFi uit).
- `password`: max. 64 bytes inhoud. Bestaande firmwaresemantiek
  (`network.c`'s `network_init()`) koppelt wachtwoordlengte niet hard aan
  `auth_mode` — een leeg wachtwoord wordt als `NULL` doorgegeven aan
  `cyw43_arch_wifi_connect_*()`, en pas de daadwerkelijke verbindingspoging
  bepaalt of dat werkt. Deze validatielaag legt dezelfde, niet-strengere
  regel op: alleen de lengte wordt gecontroleerd, geen koppeling met
  `auth_mode`.
- `auth_mode`: `0`–`8`, zie mapping hierboven; anders `INVALID_AUTH_MODE`.
- `country`: exact twee letters, hoofdletterongevoelig ingevoerd, ná
  validatie uppercase opgeslagen; geaccepteerd via `get_country_code()`
  zelf (geen losse kopie van de lijst) — een niet-herkende code valt bij
  die functie stil terug op `"XX"` zonder dat te melden, dus
  `sidetnfs_netconfig_validate()` vergelijkt expliciet de
  hoofdlettergemaakte invoer met wat `get_country_code()` teruggeeft:
  ongelijk → `INVALID_COUNTRY`. Nooit leeg in een request (alleen
  `GET_NETWORK_CONFIG` normaliseert een lege opgeslagen waarde naar
  `"XX"` voor weergave).
- `use_dhcp`: uitsluitend `0` of `1`, anders `INVALID_DHCP`.
- Bij `use_dhcp == 1`: `ip_address`/`netmask`/`gateway`/`primary_dns` worden
  niet gevalideerd (mogen leeg zijn of iets anders bevatten — ze worden
  toch niet gebruikt).
- Bij `use_dhcp == 0`: elk van de vier moet een geldig IPv4-dotted-quad
  zijn volgens `ipaddr_aton()` (dezelfde lwIP-functie die
  `sidetnfs_probe.c` al overal gebruikt) — anders respectievelijk
  `INVALID_IP`/`INVALID_NETMASK`/`INVALID_GATEWAY`/`INVALID_DNS`.
- `WIFI_DNS` bevat in deze eerste implementatie uitsluitend het primaire
  DNS-adres — geen komma-gescheiden tweede server.
- Alle stringvelden worden gecontroleerd op aantoonbare NUL-terminatie
  (`strnlen(...) < buffergrootte`) vóórdat enige andere validatie op de
  inhoud plaatsvindt.

## Statuscodes (`sidetnfs_netconfig_status_t`)

32-bit swapped statusveld, zelfde conventie als het bestaande
SideTNFS-driveprotocol (`WRITE_AND_SWAP_LONGWORD`).

| Waarde | Constante |
|---|---|
| 0 | `SIDETNFS_NETCONFIG_STATUS_OK` |
| 1 | `SIDETNFS_NETCONFIG_STATUS_INVALID_SSID` |
| 2 | `SIDETNFS_NETCONFIG_STATUS_INVALID_PASSWORD` |
| 3 | `SIDETNFS_NETCONFIG_STATUS_INVALID_AUTH_MODE` |
| 4 | `SIDETNFS_NETCONFIG_STATUS_INVALID_COUNTRY` |
| 5 | `SIDETNFS_NETCONFIG_STATUS_INVALID_DHCP` |
| 6 | `SIDETNFS_NETCONFIG_STATUS_INVALID_IP` |
| 7 | `SIDETNFS_NETCONFIG_STATUS_INVALID_NETMASK` |
| 8 | `SIDETNFS_NETCONFIG_STATUS_INVALID_GATEWAY` |
| 9 | `SIDETNFS_NETCONFIG_STATUS_INVALID_DNS` |
| 10 | `SIDETNFS_NETCONFIG_STATUS_NOT_STAGED` |
| 11 | `SIDETNFS_NETCONFIG_STATUS_FLASH_WRITE_FAILED` |
| 12 | `SIDETNFS_NETCONFIG_STATUS_FLASH_VERIFY_FAILED` |

## Hosttests

`tests/host_netconfig/` (zelfde opzet als `tests/host_configdrive/` uit
Fase 10B2: een sandbox-directory met symlinks naar de echte
`sidetnfs_netconfig.c`/`.h`, plus stub-headers voor `network.h`/`config.h`/
`lwip/ip_addr.h` omdat de echte headers Pico-SDK/lwIP/cyw43-afhankelijkheden
meenemen die niet host-compileerbaar zijn). Dekt: GET met bestaande
defaults, SET→SAVE→GET-roundtrip, maximale SSID/wachtwoordlengtes, NL/XX/
kleine-letters/ongeldige landcodes, DHCP aan met lege statische adressen,
DHCP uit met geldige en ongeldige IPv4-velden, string-endianness met
oneven tekstlengtes (rechtstreeks tegen `CHANGE_ENDIANESS_BLOCK16`), een
mislukte `SET` die de staging-copy intact laat, de 256-byte-uitlijning van
de programmalengte, en een negen-velden-readbackvergelijking. 51/51
checks slagen.

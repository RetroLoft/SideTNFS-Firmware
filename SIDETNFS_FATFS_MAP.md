# SIDETNFS — FatFS/SD-afhankelijkheidskaart in GEMDRIVE

Basis: tag `v1.0.1`, branch `sidetnfs-v101-baseline`. Alleen analyse, geen codewijziging.

## Onderzochte bestanden

- `romemul/gemdrvemul.c` (2552 regels) — hoofdlogica GEMDOS-redirector, bevat vrijwel
  alle relevante code binnen de grote command-dispatch-switch in `init_gemdrvemul()`
  (regel 761 t/m einde bestand).
- `romemul/include/gemdrvemul.h` — structuren (`FileDescriptors`, `DTANode`, `PD`,
  `ExecHeader`), geen FatFS-calls zelf.
- `romemul/filesys.c` (2034 regels) — gedeelde filesystem-helperbibliotheek, gebruikt
  door floppy-, ROM- en GEMDRIVE-code.
- `romemul/include/filesys.h` — signatures van de gedeelde helpers.
- `romemul/main.c` — geen directe FatFS/SD-calls; roept alleen `init_gemdrvemul()` aan.
- `romemul/config.c` — geen directe FatFS/SD-calls.
- `romemul/network.c` — geen directe FatFS/SD-calls.
- `romemul/floppyemul.c` — eigen, aparte FatFS-calls (zie groep 10), niet aangepast of
  meegenomen in de GEMDRIVE-inventarisatie zelf.

**Belangrijk scope-detail:** een aantal generieke `filesys.c`-helpers
(`get_dir_files`, `show_dir_files`, `get_card_info`, `get_sdcard_data`,
`calculate_folder_count`, `checkDiskSpace`, `copy_file`, `MSA_to_ST`,
`create_blank_ST_image`, `load_rom_from_fs`) worden **niet** door GEMDRIVE gebruikt —
alleen door floppy-emulatie, ROM-emulatie en de configurator. Deze vallen buiten de
GEMDRIVE-TNFS-scope en zijn niet verder geïnventariseerd.

---

## 1. Boot/init/mount

### Item 1

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_PING` (binnen `init_gemdrvemul`)
Regelnummer(s): 1180–1222
Gebruikte FatFS-call/type/helper: `sd_init_driver()`, `f_mount(&fs, "0:", 1)`, `FATFS fs`, `FRESULT fr`
GEMDOS/GEMDRIVE-operatie: eerste PING vanaf de Atari-side ROM na boot; lazy init van SD-kaart en mount van het FAT-filesystem, plus het uitlezen van `PARAM_GEMDRIVE_FOLDERS` (`hd_folder`).
Beschrijving: SD-kaart en FatFS worden pas gemount bij de eerste PING-call, niet bij firmware-boot zelf. Bij falen wordt `GEMDRVEMUL_PING_STATUS = 0x0` teruggegeven; de Atari-side driver ziet dan geen harde schijf.
Waarom relevant voor TNFS: dit is hét punt waar "SD mount" vervangen/aangevuld moet worden door "TNFS-verbinding opzetten". Bepaalt succes/falen van de hele GEMDRIVE-sessie.
Kandidaat voor backend-interface: ja
Risico bij wijzigen: hoog (single point of failure voor de hele GEMDRIVE-sessie)
Aanbevolen fase voor ombouw: vroege fase — eerste backend-abstractiepunt (init/mount)

### Item 2

Bestand: `romemul/gemdrvemul.c`
Functie: `init_gemdrvemul()` — WiFi-wachtwoord uitlezen
Regelnummer(s): 864–884
Gebruikte FatFS-call/type/helper: `sd_init_driver()`, `read_and_trim_file()` (filesys.c-helper, gebruikt intern `f_stat`/`f_open`/`f_read`/`f_close`)
GEMDOS/GEMDRIVE-operatie: geen GEMDOS-call; leest `WIFI_PASS_FILE_NAME` van SD om daarna te verbinden met WiFi (voor NTP/RTC-sync binnen GEMDRIVE).
Beschrijving: alleen uitgevoerd als `GEMDRIVE_RTC=true` én een WiFi-SSID geconfigureerd is.
Waarom relevant voor TNFS: niet direct GEMDOS-bestandsverkeer, maar wel een vroege, onafhankelijke SD-toegang tijdens GEMDRIVE-init. Moet blijven werken (of expliciet anders ingericht worden) ongeacht het gekozen bestand-backend.
Kandidaat voor backend-interface: nee (is config/WiFi-gerelateerd, geen GEMDOS-bestandstoegang)
Risico bij wijzigen: laag
Aanbevolen fase voor ombouw: n.v.t. — apart van de FatFS→TNFS-ombouw houden

---

## 2. Directory listing

### Item 3

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_FSFIRST_CALL`
Regelnummer(s): 1516–1688
Gebruikte FatFS-call/type/helper: `f_findfirst`, `f_findnext`, `DIR *dj` (malloc), `FILINFO *fno` (malloc)
GEMDOS/GEMDRIVE-operatie: `Fsfirst` — start een directory-listing/pattern-match voor een DTA (Disk Transfer Address).
Beschrijving: alloceert een `DIR`/`FILINFO`-paar, filtert macOS-achtige `.`/`._`-bestanden weg, filtert op attributen, converteert bestandsnamen (`filter_fname`/`upper_fname`/`shorten_fname`) en slaat het resultaat op in de DTA-hashtabel (`insertDTA`) voor latere `Fsnext`-calls.
Waarom relevant voor TNFS: kernfunctie van directory-browsing; `DIR`/`FILINFO` zijn FatFS-specifieke iteratorstructuren die 1-op-1 door een TNFS-directory-iterator vervangen moeten worden.
Kandidaat voor backend-interface: ja
Risico bij wijzigen: hoog (complexe state-machine met DTA-hashtabel, malloc/free-discipline, en Atari-specifieke naam/attribuutfilters)
Aanbevolen fase voor ombouw: kernfase van de FatFS→TNFS-ombouw, na fase 1 (init/mount)

### Item 4

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_FSNEXT_CALL`
Regelnummer(s): 1689–1778
Gebruikte FatFS-call/type/helper: `f_findnext(dtaNode->dj, dtaNode->fno)`
GEMDOS/GEMDRIVE-operatie: `Fsnext` — vervolgt een eerder gestarte directory-listing.
Beschrijving: leest de eerder opgeslagen `DIR`/`FILINFO` uit de DTA-hashtabel en roept `f_findnext` aan; zelfde naam/attribuutfilters als Item 3.
Waarom relevant voor TNFS: onlosmakelijk gekoppeld aan Item 3 — moet in dezelfde stap worden vervangen door de TNFS-tegenhanger van "volgende directory-entry".
Kandidaat voor backend-interface: ja
Risico bij wijzigen: hoog (zelfde reden als Item 3)
Aanbevolen fase voor ombouw: samen met Item 3

### Item 5

Bestand: `romemul/gemdrvemul.c`
Functie: `insertDTA`, `releaseDTA`, `cleanDTAHashTable`, `lookupDTA`, `countDTA`, `initializeDTAHashTable`
Regelnummer(s): 76–237
Gebruikte FatFS-call/type/helper: `DIR *`, `FILINFO *`, `f_closedir` (in `releaseDTA` regel 173 en `cleanDTAHashTable` regel 225)
GEMDOS/GEMDRIVE-operatie: ondersteunend aan `Fsfirst`/`Fsnext`/`Fsetdta`/DTA-exist/DTA-release-calls.
Beschrijving: een eigen hashtabel die per Atari-DTA-adres een FatFS `DIR`/`FILINFO`-paar plus het gebruikte zoekpatroon (`pat`) bijhoudt, zodat een lopende directory-iteratie tussen losse GEMDOS-calls in stand blijft.
Waarom relevant voor TNFS: deze hashtabel bepaalt de levensduur/vorm van "open directory handles"; een TNFS-implementatie heeft een vergelijkbare per-DTA state nodig (al is die mogelijk eenvoudiger, afhankelijk van het TNFS-protocol).
Kandidaat voor backend-interface: ja
Risico bij wijzigen: hoog (eigen malloc/free-beheer; makkelijk om memory leaks of dangling pointers te introduceren)
Aanbevolen fase voor ombouw: samen met Item 3/4

---

## 3. Path/current directory

### Item 6

Bestand: `romemul/gemdrvemul.c`
Functie: `get_local_full_pathname()`
Regelnummer(s): 535–579
Gebruikte FatFS-call/type/helper: geen directe FatFS-call; bouwt een pad-string die later aan `f_open`/`f_stat`/`f_mkdir`/`f_unlink`/etc. wordt doorgegeven
GEMDOS/GEMDRIVE-operatie: gebruikt door o.a. `Fopen`, `Fcreate`, `Fdelete`, `Fattrib`, `Dcreate`, `Ddelete`.
Beschrijving: vertaalt een Atari-pad (met of zonder driveletter, absoluut of relatief t.o.v. `dpath_string`) naar een volledig lokaal pad onder `hd_folder`.
Waarom relevant voor TNFS: dit is dé centrale path-mapping-functie. Een TNFS-backend heeft een vergelijkbare vertaalslag nodig (Atari-pad → TNFS-pad relatief aan mount-root `Atari.ST`), maar zonder afhankelijkheid van een lokaal FatFS-pad-format.
Kandidaat voor backend-interface: ja
Risico bij wijzigen: middel (puur string-logica, geen FatFS-state, maar wordt door bijna alle file-operaties gebruikt — fouten hier raken alles)
Aanbevolen fase voor ombouw: vroege fase, samen met Item 1 (mount/root-pad)

### Item 7

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_DGETPATH_CALL`
Regelnummer(s): 1304–1328
Gebruikte FatFS-call/type/helper: geen directe FatFS-call — leest alleen de globale `dpath_string`
GEMDOS/GEMDRIVE-operatie: `Dgetpath` — huidige werkdirectory opvragen.
Beschrijving: geeft `dpath_string` terug aan de Atari-side, met wat separator-conversie.
Waarom relevant voor TNFS: geen FatFS-afhankelijkheid, maar hoort logisch bij de "current directory"-state die bij een backend-wissel behouden moet blijven.
Kandidaat voor backend-interface: nee (geen FatFS-call)
Risico bij wijzigen: laag
Aanbevolen fase voor ombouw: n.v.t.

### Item 8

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_DSETPATH_CALL`
Regelnummer(s): 1329–1387
Gebruikte FatFS-call/type/helper: `directory_exists()` (filesys.c-helper, wrapt `f_stat`)
GEMDOS/GEMDRIVE-operatie: `Dsetpath` — werkdirectory wijzigen.
Beschrijving: bouwt het nieuwe pad op (relatief/absoluut, met/zonder driveletter), concateneert met `hd_folder`, en valideert existentie via `directory_exists()` vóórdat `dpath_string` wordt bijgewerkt.
Waarom relevant voor TNFS: validatie-stap (`directory_exists`) moet een TNFS-equivalent krijgen ("bestaat dit pad op de TNFS-server?").
Kandidaat voor backend-interface: ja
Risico bij wijzigen: middel
Aanbevolen fase voor ombouw: samen met Item 6

### Item 9

Bestand: `romemul/filesys.c`
Functie: `directory_exists()`
Regelnummer(s): 718–729
Gebruikte FatFS-call/type/helper: `f_stat`, `FILINFO`, `AM_DIR`
GEMDOS/GEMDRIVE-operatie: ondersteunend aan `Dsetpath`, `Dcreate`, `Ddelete`.
Beschrijving: kleine wrapper die `f_stat` gebruikt om te bepalen of een pad bestaat én een map is.
Waarom relevant voor TNFS: kleine, geïsoleerde functie — goede eerste kandidaat om achter een backend-interface te zetten (laag risico, hoge hergebruikswaarde).
Kandidaat voor backend-interface: ja
Risico bij wijzigen: laag
Aanbevolen fase voor ombouw: vroege fase, geschikt als eerste "oefen"-ombouw

### Item 10

Bestand: `romemul/filesys.c`
Functie: `split_fullpath()`
Regelnummer(s): 1507–1538
Gebruikte FatFS-call/type/helper: geen — pure string-parsing (drive/folders/filePattern splitsen)
GEMDOS/GEMDRIVE-operatie: gebruikt door `Fsfirst` (via `seach_path_2_st`) en `Frename`.
Beschrijving: splitst een volledig Atari-pad in driveletter, foldergedeelte en bestandspatroon.
Waarom relevant voor TNFS: geen FatFS-afhankelijkheid zelf, maar het uitvoerformaat (drive/folders/pattern) wordt door de FatFS-specifieke calls verderop gebruikt; blijft waarschijnlijk herbruikbaar.
Kandidaat voor backend-interface: nee (geen FatFS-call)
Risico bij wijzigen: laag
Aanbevolen fase voor ombouw: n.v.t.

---

## 4. File open/read/seek/close

### Item 11

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_FOPEN_CALL`
Regelnummer(s): 1779–1842
Gebruikte FatFS-call/type/helper: `f_open`, `FIL file_object`
GEMDOS/GEMDRIVE-operatie: `Fopen` — bestand openen (read/write/read-write mode-mapping naar `FA_READ`/`FA_WRITE`).
Beschrijving: opent het bestand via FatFS en registreert het in de eigen `FileDescriptors`-linked-list (`add_file`) met een nieuw Atari-file-descriptor-nummer.
Waarom relevant voor TNFS: `FIL`-object en filehandle-levensduur moeten vervangen worden door een TNFS-bestandshandle; de `FileDescriptors`-lijst zelf (fd-toewijzing, pad, offset) is backend-onafhankelijk en kan blijven bestaan.
Kandidaat voor backend-interface: ja
Risico bij wijzigen: hoog (kernoperatie, elke file-actie hangt hiervan af)
Aanbevolen fase voor ombouw: kernfase, na Item 1/6

### Item 12

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_FCLOSE_CALL`
Regelnummer(s): 1843–1880
Gebruikte FatFS-call/type/helper: `f_close`
GEMDOS/GEMDRIVE-operatie: `Fclose`
Beschrijving: sluit het FatFS-bestandsobject en verwijdert de entry uit `FileDescriptors` (`delete_file_by_fdesc`).
Waarom relevant voor TNFS: 1-op-1 te vervangen door TNFS close-call.
Kandidaat voor backend-interface: ja
Risico bij wijzigen: middel
Aanbevolen fase voor ombouw: samen met Item 11

### Item 13

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_FSEEK_CALL`
Regelnummer(s): 1996–2049
Gebruikte FatFS-call/type/helper: `f_size(&(file->fobject))` (alleen voor `SEEK_END`-berekening)
GEMDOS/GEMDRIVE-operatie: `Fseek`
Beschrijving: houdt de bestandspositie bij als een eigen `offset`-veld in `FileDescriptors` (dus geen directe `f_lseek` hier); alleen voor SEEK_END wordt de FatFS-bestandsgrootte opgevraagd. De echte seek gebeurt pas bij de volgende read/write.
Waarom relevant voor TNFS: enige externe afhankelijkheid is `f_size` voor SEEK_END — TNFS heeft een vergelijkbare "bestandsgrootte opvragen"-operatie nodig.
Kandidaat voor backend-interface: ja (klein oppervlak)
Risico bij wijzigen: laag
Aanbevolen fase voor ombouw: samen met Item 11/14

### Item 14

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_READ_BUFF_CALL`
Regelnummer(s): 2276–2336
Gebruikte FatFS-call/type/helper: `f_lseek`, `f_read`
GEMDOS/GEMDRIVE-operatie: `Fread` (via het "read buffer"-protocol tussen Atari-side en RP2040)
Beschrijving: verplaatst de FatFS-bestandspositie naar `file->offset` en leest tot `DEFAULT_FOPEN_READ_BUFFER_SIZE` bytes; werkt de eigen offset bij; wisselt eindianness om (16-bit words) voor de Atari-side.
Waarom relevant voor TNFS: het meest frequent aangeroepen datapad tijdens normaal gebruik (elke bestandslezing). Prestatie en foutafhandeling van de TNFS-vervanger zijn hier kritiek.
Kandidaat voor backend-interface: ja
Risico bij wijzigen: hoog (performance-kritisch pad, veel bestaande edge cases rond buffergrootte/endianness)
Aanbevolen fase voor ombouw: kernfase

### Item 15

Bestand: `romemul/gemdrvemul.c`
Functie: `FileDescriptors`-linked-list beheer: `add_file`, `get_file_by_fpath`, `get_file_by_fdesc`, `delete_file_by_fpath`, `delete_file_by_fdesc`, `get_first_available_fd`, `count_fdesc`, `close_all_files`, `delete_all_files`
Regelnummer(s): 375–532, 582–613
Gebruikte FatFS-call/type/helper: bevat een `FIL fobject`-veld per node; `close_all_files` roept indirect `f_close` aan (via dezelfde structuur als Item 12)
GEMDOS/GEMDRIVE-operatie: ondersteunend aan alle file-calls (`Fopen`/`Fclose`/`Fcreate`/`Fdelete`/`Fseek`/`Fread`/`Fwrite`/`Fdatime`).
Beschrijving: eigen linked-list die per open Atari-file-descriptor een pad, FatFS-bestandsobject en leesoffset bijhoudt.
Waarom relevant voor TNFS: het `FIL fobject`-veld is het enige FatFS-specifieke onderdeel van deze structuur — bij een TNFS-backend wordt dit een TNFS-handle-type. De rest (fd-toewijzing, pad, offset) is generiek en herbruikbaar.
Kandidaat voor backend-interface: ja (alleen het handle-type wijzigt)
Risico bij wijzigen: middel (kerndatastructuur, maar de wijziging is klein en lokaal: één typewijziging in `include/gemdrvemul.h`)
Aanbevolen fase voor ombouw: samen met Item 11, als eerste stap (typedefinitie aanpassen vóór de call-sites)

---

## 5. File create/write

### Item 16

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_FCREATE_CALL`
Regelnummer(s): 1882–1931
Gebruikte FatFS-call/type/helper: `f_open` met `FA_READ | FA_WRITE | FA_CREATE_ALWAYS`, `FIL file_object`
GEMDOS/GEMDRIVE-operatie: `Fcreate`
Beschrijving: opent (creëert) het bestand in "create always"-modus en registreert het net als `Fopen` in `FileDescriptors`. Bevat een `// MISSING ATTRIBUTE MODIFICATION`-commentaar — attributen worden bij create nog niet doorgezet.
Waarom relevant voor TNFS: zelfde patroon als Item 11 (Fopen); TNFS-equivalent van "maak/open bestand, overschrijf indien bestaand" nodig.
Kandidaat voor backend-interface: ja
Risico bij wijzigen: hoog (zelfde reden als Fopen)
Aanbevolen fase voor ombouw: samen met Item 11

### Item 17

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_WRITE_BUFF_CALL`
Regelnummer(s): 2337–2407
Gebruikte FatFS-call/type/helper: `f_lseek`, `f_write`
GEMDOS/GEMDRIVE-operatie: `Fwrite` (via het "write buffer"-protocol)
Beschrijving: verplaatst FatFS-positie naar `file->offset`, berekent een 16-bit checksum over de buffer, wisselt eindianness om, en schrijft de bytes. Checksum wordt teruggegeven aan de Atari-side voor verificatie.
Waarom relevant voor TNFS: tweede meest kritieke datapad (elke bestandsschrijving). De checksum-conventie moet behouden blijven ongeacht backend, omdat de Atari-side ROM deze verwacht.
Kandidaat voor backend-interface: ja
Risico bij wijzigen: hoog
Aanbevolen fase voor ombouw: kernfase, samen met Item 14

### Item 18

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_WRITE_BUFF_CHECK`
Regelnummer(s): 2408–2432
Gebruikte FatFS-call/type/helper: geen directe FatFS-call — werkt alleen `file->offset` bij
GEMDOS/GEMDRIVE-operatie: vervolg op `Fwrite`-protocol (bevestiging van geschreven bytes, offset ophogen)
Beschrijving: zuivere boekhouding, geen FatFS-aanroep.
Waarom relevant voor TNFS: geen wijziging nodig, kan ongewijzigd blijven.
Kandidaat voor backend-interface: nee
Risico bij wijzigen: laag
Aanbevolen fase voor ombouw: n.v.t.

---

## 6. Delete/rename/mkdir/rmdir

### Item 19

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_DCREATE_CALL`
Regelnummer(s): 1388–1421
Gebruikte FatFS-call/type/helper: `f_mkdir`
GEMDOS/GEMDRIVE-operatie: `Dcreate` — map aanmaken.
Beschrijving: valideert eerst dat het ouderpad bestaat (`directory_exists`), roept dan `f_mkdir` aan.
Kandidaat voor backend-interface: ja
Risico bij wijzigen: middel
Aanbevolen fase voor ombouw: kernfase, met Items 6/9

### Item 20

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_DDELETE_CALL`
Regelnummer(s): 1422–1469
Gebruikte FatFS-call/type/helper: `f_unlink`
GEMDOS/GEMDRIVE-operatie: `Ddelete` — map verwijderen.
Beschrijving: FatFS-foutcodes (`FR_DENIED`, `FR_NO_PATH`) worden expliciet naar GEMDOS-foutcodes gemapt (zie ook groep 9).
Kandidaat voor backend-interface: ja
Risico bij wijzigen: middel
Aanbevolen fase voor ombouw: kernfase

### Item 21

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_FDELETE_CALL`
Regelnummer(s): 1932–1995
Gebruikte FatFS-call/type/helper: `f_close` (als bestand nog open staat), `f_unlink`
GEMDOS/GEMDRIVE-operatie: `Fdelete` — bestand verwijderen.
Beschrijving: sluit eerst een eventueel open file-handle (consistent met `FileDescriptors`), verwijdert dan pas het bestand. Zelfde FatFS→GEMDOS-foutmapping als Item 20.
Kandidaat voor backend-interface: ja
Risico bij wijzigen: middel-hoog (combineert twee operaties: close + unlink, volgorde is functioneel belangrijk)
Aanbevolen fase voor ombouw: kernfase, samen met Item 12

### Item 22

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_FRENAME_CALL`
Regelnummer(s): 2102–2175
Gebruikte FatFS-call/type/helper: `f_rename`, plus `split_fullpath` (Item 10) voor drive-vergelijking
GEMDOS/GEMDRIVE-operatie: `Frename`
Beschrijving: weigert de rename als bron en doel op verschillende "drives" staan; converteert daarna beide paden via `get_local_full_pathname` en roept `f_rename` aan.
Kandidaat voor backend-interface: ja
Risico bij wijzigen: middel
Aanbevolen fase voor ombouw: kernfase

---

## 7. Attributes/date/time

### Item 23

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_FATTRIB_CALL`
Regelnummer(s): 2050–2101
Gebruikte FatFS-call/type/helper: `f_stat`, `f_chmod`, `FILINFO fno`
GEMDOS/GEMDRIVE-operatie: `Fattrib` — attributen opvragen (`FATTRIB_INQUIRE`) of zetten.
Beschrijving: gebruikt `attribs_fat2st`/`attribs_st2fat` (filesys.c) voor conversie tussen FAT- en Atari-attribuutbits.
Kandidaat voor backend-interface: ja
Risico bij wijzigen: middel
Aanbevolen fase voor ombouw: later dan de kernfase (minder kritiek dan open/read/write)

### Item 24

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_FDATETIME_CALL`
Regelnummer(s): 2176–2274
Gebruikte FatFS-call/type/helper: `f_stat` (inquire), `f_utime` (set), `FILINFO fno`
GEMDOS/GEMDRIVE-operatie: `Fdatime` — bestandsdatum/tijd opvragen of zetten.
Beschrijving: FAT-datum/tijd-bitvelden (`fno.fdate`/`fno.ftime`) worden direct doorgegeven aan de Atari-side (die hetzelfde DOS-datumformaat gebruikt) — geen aparte conversiefunctie nodig, alleen `#if _DEBUG` mens-leesbare logging.
Waarom relevant voor TNFS: TNFS-datum/tijd-representatie moet naar hetzelfde FAT/DOS-bitformaat vertaald worden dat de Atari-side verwacht (zie ook Y2K-patch, `PARAM_RTC_Y2K_PATCH`, elders in het bestand).
Kandidaat voor backend-interface: ja
Risico bij wijzigen: middel (datumconversiefouten zijn moeilijk te zien zonder expliciete test)
Aanbevolen fase voor ombouw: later dan de kernfase

### Item 25

Bestand: `romemul/filesys.c`
Functie: `attribs_fat2st()`, `attribs_st2fat()`, `get_attribs_st_str()`
Regelnummer(s): 1777–1891
Gebruikte FatFS-call/type/helper: FatFS-attribuutconstanten `AM_RDO`, `AM_HID`, `AM_SYS`, `AM_DIR`, `AM_ARC`
GEMDOS/GEMDRIVE-operatie: ondersteunend aan `Fsfirst`/`Fsnext`/`Fattrib`.
Beschrijving: pure bit-conversiefuncties, geen FatFS-call, maar wel afhankelijk van FatFS-attribuutconstanten.
Kandidaat voor backend-interface: ja (kleine aanpassing: TNFS-attribuutbits kunnen afwijken van FAT-bits)
Risico bij wijzigen: laag
Aanbevolen fase voor ombouw: samen met Items 23/24

---

## 8. Disk info/free space

### Item 26

Bestand: `romemul/gemdrvemul.c`
Functie: `case GEMDRVEMUL_DFREE_CALL`
Regelnummer(s): 1277–1303
Gebruikte FatFS-call/type/helper: `f_getfree`, `FATFS *fs`, `DWORD fre_clust`
GEMDOS/GEMDRIVE-operatie: `Dfree` — vrije/totale schijfruimte opvragen.
Beschrijving: berekent vrije bytes uit clustergrootte (`fs->csize`) en sectorgrootte; schrijft cluster-, sector- en totaalinformatie naar shared memory.
Waarom relevant voor TNFS: TNFS heeft mogelijk geen cluster-concept — deze berekening moet vervangen worden door iets dat een vergelijkbare (eventueel benaderde/vaste) waarde teruggeeft, aangezien sommige programma's hierop controleren vóór een schrijfactie.
Kandidaat voor backend-interface: ja
Risico bij wijzigen: middel (functioneel niet kritiek voor basale bestandstoegang, maar kan schrijfacties in Atari-software blokkeren als de waarde onrealistisch is)
Aanbevolen fase voor ombouw: kan relatief laat, na de kernfuncties (open/read/write/directory)

---

## 9. Error mapping

### Item 27

Bestand: `romemul/gemdrvemul.c`
Functie: verspreid over meerdere handlers — representatieve locaties: `DDELETE_CALL` (1444–1458), `FDELETE_CALL` (1963–1983), `FRENAME_CALL` (2144–2163)
Regelnummer(s): zie hierboven (patroon herhaalt zich in vrijwel elke schrijvende/verwijderende call)
Gebruikte FatFS-call/type/helper: `FRESULT`-waarden `FR_OK`, `FR_DENIED`, `FR_NO_PATH`, `FR_NO_FILE`, `FR_INVALID_OBJECT`
GEMDOS/GEMDRIVE-operatie: alle schrijvende/verwijderende/hernoemende calls.
Beschrijving: er is **geen centrale foutmapping-functie** — elke handler bevat zijn eigen inline `if (fr == FR_DENIED) ... else if (fr == FR_NO_PATH) ...`-ladder die naar de bijpassende GEMDOS-foutcode (`GEMDOS_EACCDN`, `GEMDOS_EPTHNF`, `GEMDOS_EFILNF`, `GEMDOS_EINTRN`, `GEMDOS_EIHNDL`) vertaalt.
Waarom relevant voor TNFS: dit is een verborgen risico: TNFS-foutcodes zullen niet 1-op-1 overeenkomen met `FRESULT`-waarden. Zonder een centrale mapping moet elke call-site apart worden nagelopen en aangepast, wat foutgevoelig is en makkelijk inconsistenties oplevert tussen handlers.
Kandidaat voor backend-interface: ja — sterke kandidaat om als eerste te centraliseren (één mapping-functie `backend_error_to_gemdos()` i.p.v. verspreide ladders)
Risico bij wijzigen: hoog (subtiele functionele regressies zijn moeilijk te zien; foute foutcodes leiden tot verwarrend gedrag in Atari-software, niet tot crashes)
Aanbevolen fase voor ombouw: vroeg overwegen als apart, klein voorbereidend refactor-item (nog steeds pas ná deze analysefase, met apart akkoord)

---

## 10. Floppy-gerelateerde FatFS — alleen ter referentie

Niet onderdeel van de GEMDRIVE-scope; alleen genoteerd zodat bekend is dat floppy-emulatie
zijn eigen, aparte FatFS-toegang heeft en niet geraakt wordt door een GEMDRIVE→TNFS-ombouw.

- `romemul/floppyemul.c:103-115` — `f_lseek`/`f_read`/`f_close` (lezen van een floppy-image)
- `romemul/floppyemul.c:160-199` — `f_open`/`f_size`/`f_lseek`/`f_close` (openen/positioneren)
- `romemul/floppyemul.c:846` — `f_mount` (eigen, aparte SD-mount voor floppy-emulatie)
- `romemul/floppyemul.c:1187-1199` — `f_lseek`/`f_close`/`f_read` (leesbuffer-protocol, analoog aan GEMDRIVE's READ_BUFF)
- `romemul/floppyemul.c:1270-1281` — `f_lseek`/`f_close`/`f_write` (schrijfbuffer-protocol, analoog aan GEMDRIVE's WRITE_BUFF)

---

## Samenvatting

- **27 items** gedocumenteerd, verdeeld over 9 inhoudelijke groepen plus 1 referentiegroep.
- **21 directe FatFS-call-sites** geteld in `gemdrvemul.c` zelf (zie ook de ruwe telling
  hieronder), plus een aantal indirecte via `filesys.c`-helpers (`directory_exists`,
  `attribs_fat2st`/`attribs_st2fat`, `read_and_trim_file`).
- Vrijwel de **volledige GEMDOS-command-dispatch** zit in één functie
  (`init_gemdrvemul()`, gemdrvemul.c:761–2552) — er is geen aparte laag tussen
  "GEMDOS-call ontvangen" en "FatFS aanroepen"; dat is precies waarom een
  backend-interface nuttig is, maar ook waarom de ombouw voorzichtig moet gebeuren
  (alles zit in één grote `switch`).

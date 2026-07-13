# SIDETNFS — SD/FatFS vs TNFS directory-listing verschillen (Fase 5J)

Analysedocument, geen codewijziging aan GEMDRIVE. Basis: `romemul/gemdrvemul.c`
(`GEMDRVEMUL_FSFIRST_CALL`/`GEMDRVEMUL_FSNEXT_CALL`, DTA-helpers), `romemul/filesys.c`
(naam/attribuut-helpers), `romemul/sidetnfs_probe.c` (huidige TNFS OPENDIRX/READDIRX-test),
en de eerdere referentie-implementatie `fs_tnfs.c` uit de losse SIDETNFS-poging.

## 1. Padmapping

| | SD/FatFS (huidig, in GEMDRIVE) | TNFS (nu, in de probe) |
|---|---|---|
| Atari-pad | `N:\CONFIG\*.*` (drive-letter + backslashes) | n.v.t. — nog geen Atari-integratie |
| Stripping driveletter | `Fsfirst`: als `tmp_string[1] == ':'`, spring 2 posities door (`gemdrvemul.c:1586-1592`) | MOUNT-payload gebruikt zelf geen driveletter |
| Backslash→slash | `back_2_forwardslash()` (`filesys.c`), toegepast vóór en na `seach_path_2_st()` | n.v.t. — TNFS-paden zijn al slash-based |
| Relatief pad | als geen leidende `/`: concateneer met `dpath_string` (`gemdrvemul.c:1598-1603`) | onze probe gebruikt altijd het vaste pad `/` (root van de mount) — nog geen `dpath_string`-integratie |
| Root na mount | n.v.t. | mount-payload bevat `/Atari.ST`; alle latere paden (OPENDIRX/READDIRX) zijn **relatief aan die root**, dus root = `/` |
| Subdir-pad | `internal_path = hd_folder + "/" + path_forwardslash` (`seach_path_2_st`, `gemdrvemul.c:240-273`) | zou worden `/CONFIG` (geen `hd_folder`-prefix nodig, TNFS-server kent zijn eigen root al) |

**Wat TNFS straks nodig heeft:** een eigen "path-mapping"-functie analoog aan `seach_path_2_st()`/`get_local_full_pathname()`, maar die **geen** `hd_folder`-prefix toevoegt (de TNFS-server doet dat zelf via de mount) — alleen driveletter strippen + backslash→slash + relatief-pad-resolutie tegen `dpath_string`.

## 2. Wildcards/patterns

- **FatFS** (`f_findfirst(dj, fno, internal_path, pattern)`): gebruikt FatFS's eigen ingebouwde wildcard-matcher (`*`/`?`, case-insensitive, DOS-FCB-stijl). `pattern` komt uit `split_fullpath()` (laatste padcomponent na de laatste slash).
- **TNFS OPENDIRX**: heeft ook een `pattern`-veld in de requestpayload (wij gebruiken nu altijd `"*"` = alles). **Onbekend/ongetest:** of de TNFS-server dezelfde DOS-FCB-achtige semantiek hanteert als FatFS (bv. of `FOLDER.*` een kale map `FOLDER` zonder extensie matcht). Dit moet apart getest worden — niet aannemen dat serverzijdige pattern-matching identiek is aan FatFS.
- **Aanbeveling:** voorlopig **lokaal** (in firmware) filteren op het GEMDOS-pattern, na ontvangst van de volledige (ongefilterde, `"*"`) TNFS-listing — net zoals FatFS het al doet vóór de attributen-filter in `Fsfirst`. Dat garandeert identiek gedrag ongeacht wat de TNFS-server zelf doet, en is de veiligste keuze.
- Speciale gevallen te testen: `*.*` (alles), `*.PRG`, `CONFIG.*`, `FOLDER.*` tegen een map zonder punt, lege extensie, directories zonder punt in de naam.

## 3. Namen en 8.3

- **FatFS-pad:** `FILINFO.fname` (kan LFN of 8.3 zijn, afhankelijk van FatFS-configuratie — in dit project lijkt 8.3 al front-and-center) → `filter_fname()` (behoudt alfanumeriek + toegestane symbolen, `filesys.c:1913`) → `upper_fname()` (uppercase, max 13 chars, `filesys.c:1892`) → `shorten_fname()` (dwingt 8.3-formaat af met `~1`-suffix bij te lange namen, vervangt dubbele punten door underscores, `filesys.c:1633-1735`).
- **TNFS entry name:** komt rechtstreeks van de server (in ons READDIRX-antwoord: null-terminated string na de vaste 13-byte velden). Kan in principe elke lengte/case hebben (afhankelijk van het onderliggende OS van de TNFS-server, typisch Linux/ext4 — dus lowercase, lange namen, punten in de naam, etc. zijn heel normaal).
- **Mac-metadata-filter:** de bestaande `Fsfirst`-lus skipt entries die beginnen met `"._"` (AppleDouble-bestanden) via een while-lus die net zo lang doorgaat tot een niet-`"._"`-naam gevonden is (`gemdrvemul.c:1635-1665`). Dit is SD-specifiek gedrag (macOS-gebruikers die bestanden op de kaart zetten) — TNFS-serverzijdig (Linux) speelt dit waarschijnlijk niet, maar als de TNFS-share zelf ooit vanaf een Mac gevuld wordt, is dezelfde filter nodig.
- **Conclusie:** TNFS-namen moeten door **dezelfde** `filter_fname → upper_fname → shorten_fname`-keten als SD-namen, om identieke Atari-zichtbare bestandsnamen te garanderen — anders krijgt de gebruiker inconsistent gedrag tussen SD- en TNFS-backend.

## 4. Attributen

- **FatFS `fattrib`-bits:** `AM_RDO` (read-only), `AM_HID` (hidden), `AM_SYS` (system), `AM_DIR` (directory), `AM_ARC` (archive).
- **Atari ST-attributen** (`filesys.h`): `FS_ST_READONLY (0x1)`, `FS_ST_HIDDEN (0x2)`, `FS_ST_SYSTEM (0x4)`, `FS_ST_LABEL (0x8)`, `FS_ST_FOLDER (0x10)`, `FS_ST_ARCH (0x20)`.
- **`attribs_fat2st()`** (`filesys.c:1777`) mapt 1-op-1: RDO→READONLY, HID→HIDDEN, SYS→SYSTEM, DIR→FOLDER, ARC→ARCH. Geen `LABEL`-equivalent vanuit FatFS (die wordt apart afgehandeld in GEMDRIVE).
- **TNFS READDIRX flags:** alleen 3 bits bekend uit de eerdere referentie-implementatie: `TNFS_DIRENTRY_DIR (0x01)`, `TNFS_DIRENTRY_HIDDEN (0x02)`, `TNFS_DIRENTRY_SPECIAL (0x04)`. **Geen** read-only/system/archive-equivalent in de TNFS-flags die we tot nu toe gebruiken.
- **Wat verloren gaat:** `FS_ST_READONLY`, `FS_ST_SYSTEM` en `FS_ST_ARCH` hebben (vooralsnog) geen TNFS-bron — die zouden op een vaste/afgeleide waarde gezet moeten worden (bv. altijd "niet read-only, geen system, wel archive") tenzij een uitgebreidere TNFS-STAT-call ooit meer info geeft.
- **Mapping-voorstel:** `TNFS_DIRENTRY_DIR → FS_ST_FOLDER`, `TNFS_DIRENTRY_HIDDEN → FS_ST_HIDDEN`, `TNFS_DIRENTRY_SPECIAL` → waarschijnlijk skippen (net als nu al gebeurt in onze telling) i.p.v. naar een Atari-bit mappen.

## 5. Grootte

- **FatFS `fsize`**: exacte bestandsgrootte in bytes; voor directories is dit meestal 0 (FatFS kent geen "directory-grootte"-concept zoals sommige Unix-filesystems).
- **TNFS READDIRX size-veld**: 4-byte LE getal per entry, direct na de flags-byte. Voor directories op een Linux-achtige TNFS-server kan dit een niet-nul waarde zijn (bv. 4096, de "inode-blokgrootte" van de onderliggende directory-inode) — dat is **geen** bruikbare "hoeveelheid data"-maat voor GEMDOS en moet vermoedelijk altijd op 0 gezet worden voor directory-entries richting Atari, ongeacht wat TNFS teruggeeft.
- **Wat GEMDOS verwacht:** een 32-bit lengte in bytes voor bestanden; voor mappen is de eigen praktijk (via FatFS) al 0, dus TNFS moet dat gedrag repliceren i.p.v. de rauwe TNFS-size 1-op-1 door te geven voor directories.

## 6. Datum/tijd

- **FatFS `fdate`/`ftime`**: DOS-bitvelden (dezelfde indeling die GEMDOS zelf gebruikt — vandaar dat `populate_dta()` deze **ongewijzigd** doorschrijft naar `d_time`/`d_date`, zie `gemdrvemul.c:309-310`). Geen conversie nodig tussen FatFS en Atari.
- **TNFS mtime/ctime**: in de eerdere referentie-implementatie 4-byte LE velden, vermoedelijk Unix-epoch-seconden (niet bevestigd/getest door ons) — een heel ander formaat dan het DOS-bitveld dat GEMDOS verwacht.
- **Conversie nodig:** ja — Unix-epoch → DOS date/time-bitvelden (jaar-offset 1980, dag/maand/jaar in 5/4/7 bits, uur/minuut/seconde/2 in 5/6/5 bits), vergelijkbaar met de conversie die al bestaat voor de RTC/NTP-datumafhandeling elders in dit project (`rtcemul.c`/`set_ikb_datetime_msg`). Dit moet apart geverifieerd worden zodra we echte mtime-waarden van de TNFS-server bekijken (nog niet gedaan — onze huidige READDIRX-test negeert size/mtime/ctime bewust, zie punt 5/6 hierboven).

## 7. Sortering

- **FatFS-volgorde**: bestandssysteem-native (meestal directory-entry-volgorde op de FAT-tabel, dus min of meer "aanmaakvolgorde", niet alfabetisch).
- **TNFS OPENDIRX**: heeft een `sortopts`-veld in de request (wij gebruiken nu `0x00` = default/ascending-by-name volgens de eerdere referentie-implementatie's commentaar).
- **GEM/TOS-verwachting**: GEM/TOS-listings tonen doorgaans gewoon de volgorde die GEMDOS teruggeeft (geen eigen hersortering) — dus als FatFS "willekeurig" (aanmaakvolgorde) teruggeeft, is dat al de huidige praktijk en hoeft TNFS niet per se dezelfde volgorde te matchen, zolang de gebruiker maar een consistente, zinnige volgorde ziet. Alfabetisch (TNFS-default) is zelfs waarschijnlijk prettiger dan FatFS's huidige gedrag.
- **Conclusie**: geen blocker, mogelijk zelfs een verbetering; niet iets om nu al op te lossen.

## 8. EOF / einde directory

- **FatFS**: `f_findnext()` retourneert `FR_OK` met een **lege** `fno->fname` (`fno->fname[0] == '\0'`) om "geen entries meer" aan te geven (zie de `Fsfirst`/`Fsnext`-lussen die expliciet op `fno->fname[0]` checken).
- **TNFS READDIRX**: `rc == 0x21` (TNFS_EOF) of `batch_count == 0` (zie onze huidige implementatie in `sidetnfs_probe.c`, functie `parse_readdirx_entries`/de callback-tak voor `TNFS_CMD_READDIRX`).
- **Naar GEMDOS**: in de bestaande code resulteert "geen (meer) entries" in `GEMDOS_EFILNF` (bij Fsfirst, `gemdrvemul.c:1701-1711`) resp. `GEMDOS_ENMFIL` (bij Fsnext, via `populate_dta()`'s `gemdos_err_code`-parameter, `gemdrvemul.c:352-365`). Een TNFS-backend zou dezelfde twee foutcodes moeten produceren op hetzelfde punt in de Fsfirst/Fsnext-cyclus.

## 9. Foutcodes

- **FatFS `FRESULT`**: `FR_OK`, `FR_DENIED`, `FR_NO_PATH`, `FR_NO_FILE`, `FR_INVALID_OBJECT`, etc. — al gecentraliseerd gemapt naar GEMDOS-codes via `scfs_fresult_to_gemdos_error()` (Fase 4D), zij het nu alleen gebruikt voor `Ddelete`.
- **TNFS rc**: eigen kleine foutcode-set (`0x00`=OK, `0x21`=EOF, en een reeks POSIX-achtige errno-achtige codes voor de rest — nog niet volledig geïnventariseerd in dit project; alleen OK/EOF zijn tot nu toe daadwerkelijk onderscheiden in onze code).
- **Mapping nodig**: ja, een aparte `tnfs_rc_to_gemdos_error()`-achtige functie zal nodig zijn zodra TNFS echt als GEMDRIVE-backend dient — kan niet zomaar de bestaande `scfs_fresult_to_gemdos_error()` hergebruiken (andere foutcode-ruimte), maar kan wel dezelfde aanpak/structuur volgen.

## 10. DTA-state: SD/FatFS nu vs. TNFS straks

**SD/FatFS nu** (`insertDTA`/`releaseDTA`/`lookupDTA`, `gemdrvemul.c:77-237`): per Atari-DTA-adres (hash-tabel op `ndta`) wordt bewaard:
- `DIR *dj` (FatFS directory-object, met eigen `pat`-veld voor het actieve zoekpatroon)
- `FILINFO *fno` (laatst-gevonden entry)
- `attribs` (het GEMDOS-attributenfilter van de oorspronkelijke Fsfirst-call)
- een gekopieerd `pat`-zoekpatroon

Bij `releaseDTA()` wordt `f_closedir(dj)` aangeroepen en het geheugen vrijgegeven — dit is **synchroon/blocking** (lokale SD-I/O), wat voor TNFS niet zomaar 1-op-1 kan (netwerk is async).

**Wat TNFS straks per DTA moet bewaren** (in plaats van `DIR*`/`FILINFO*`):
- TNFS session id (of een verwijzing naar de ene gedeelde sessie)
- directory handle (van OPENDIRX)
- het oorspronkelijke GEMDOS-pattern + attributenfilter (voor lokale filtering, zie punt 2)
- een klein "huidige batch"-buffer (laatste READDIRX-respons, plus een cursor binnen die batch — want een enkele READDIRX-respons bevat meerdere entries, en Fsfirst/Fsnext geven er telkens maar één terug aan de Atari)
- EOF-status (is de laatste batch al binnen?)
- een "pending request"-vlag (is er al een READDIRX-verzoek onderweg voor déze DTA, om te voorkomen dat Fsnext een dubbel verzoek stuurt terwijl het vorige nog niet beantwoord is)

**Belangrijk verschil met de huidige architectuur:** FatFS's `Fsfirst`/`Fsnext` zijn **synchroon** — de Atari-kant wacht per call op een direct antwoord, en de RP2040-firmware blokkeert daarbij kort op de SD-kaart (microseconden, onmerkbaar). Een TNFS-backend zou **niet** meer synchroon kunnen zijn (netwerklatentie), tenzij Fsfirst/Fsnext zelf een klein aantal keren pollen totdat de READDIRX-respons binnen is (net als onze huidige probe-service dat doet, maar dan binnen de tijdsdruk van een lopende GEMDOS-call vanaf de Atari — dit is een fundamenteel architecturaal aandachtspunt voor een latere fase, niet iets wat nu al opgelost moet worden).

---

## Samenvatting — belangrijkste verschillen

1. **Padmapping**: TNFS heeft geen `hd_folder`-prefix nodig (mount regelt dat al), maar wel dezelfde driveletter/backslash/relatief-pad-logica als SD.
2. **Wildcards**: TNFS-serverzijdige pattern-matching is ongetest/onbekend — veiligste aanpak is lokaal filteren, net als nu.
3. **Namen**: TNFS-namen moeten door dezelfde 8.3-conversieketen als SD-namen voor consistente Atari-namen.
4. **Attributen**: TNFS heeft alleen dir/hidden/special-bits — read-only/system/archive gaan (vooralsnog) verloren en moeten op een vaste waarde gezet worden.
5. **Grootte**: directory-size van TNFS is waarschijnlijk niet 0 en moet net als bij FatFS worden geforceerd naar 0 voor mappen.
6. **Datum/tijd**: TNFS mtime is vermoedelijk Unix-epoch en heeft een aparte conversie naar DOS-bitvelden nodig (nog niet geverifieerd).
7. **Sortering**: geen blocker, TNFS's alfabetische default is prima.
8. **EOF**: goed 1-op-1 te mappen (`rc==0x21`/`batch==0` → `GEMDOS_EFILNF`/`ENMFIL`, exact zoals FatFS's lege-naam-signaal nu al wordt afgehandeld).
9. **Foutcodes**: aparte TNFS-rc→GEMDOS-mapping nodig, kan qua structuur op `scfs_fresult_to_gemdos_error()` lijken.
10. **DTA-state**: grootste architecturale verschil — TNFS heeft een async-bewuste per-DTA-batch/cursor/pending-state nodig i.p.v. de huidige synchrone `DIR*`/`FILINFO*`-aanpak.

/**
 * File: sidetnfs_netconfig.h
 * Description: Fase 11A -- WiFi/network configuration accessible while
 * GEMDRIVE is running (GEMDRVEMUL_SIDETNFS_GET/SET/SAVE_NETWORK_CONFIG).
 * Reuses the existing PARAM_WIFI_* configData entries and the existing 8KB
 * CONFIG_FLASH sector (romemul/config.c) -- no second permanent network
 * config sector. Mirrors the shape (validate -> RAM stage -> flash +
 * readback) already proven by sidetnfs_config.c's drive-list protocol, but
 * against a completely separate storage (configData, not the SIDETNFS
 * drive-list flash block) and a completely separate command-ID space
 * (0x13-0x15, never routed to/from the APP_CONFIGURATOR command IDs in
 * romloader.c).
 */
#ifndef SIDETNFS_NETCONFIG_H
#define SIDETNFS_NETCONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "network.h" // MAX_SSID_LENGTH(36)/MAX_PASSWORD_LENGTH(68)/IPV4_ADDRESS_LENGTH(16) -- same
                     // field-size constants already used elsewhere in this codebase
                     // (WifiNetworkAuthInfo/ConnectionData), not new invented sizes.

// Fase 11A: matches ConnectionData's own wifi_country[4] convention in
// network.h (2-letter ISO-3166-1 alpha-2 code + NUL + 1 padding byte for
// CHANGE_ENDIANESS_BLOCK16's even-length requirement).
#define SIDETNFS_NET_COUNTRY_LEN 4

// Fase 11A: one fixed wire record, every field either uint16_t or a char[]
// of even length -- no field ever needs compiler-inserted padding, and
// this struct is never memcpy'd as a whole onto the ROM3 shared-memory
// window (see GEMDRVEMUL_SIDETNFS_NETWORK_* explicit offsets in
// gemdrvemul.h) -- it is only ever a RAM-side value (GET result, SET
// request, or the staging copy), field-by-field into/out of shared memory.
typedef struct
{
    uint16_t auth_mode;                        // 0-8, see sidetnfs_netconfig_status_t/docs for the confirmed mapping
    uint16_t use_dhcp;                          // 0 or 1
    char ssid[MAX_SSID_LENGTH];                 // 36, <=32 real chars, empty allowed (disables WiFi)
    char password[MAX_PASSWORD_LENGTH];         // 68, <=64 real chars
    char country[SIDETNFS_NET_COUNTRY_LEN];     // 4, exactly 2 letters (incl. "XX"), never empty in a request
    char ip_address[IPV4_ADDRESS_LENGTH];       // 16
    char netmask[IPV4_ADDRESS_LENGTH];          // 16
    char gateway[IPV4_ADDRESS_LENGTH];          // 16
    char primary_dns[IPV4_ADDRESS_LENGTH];      // 16
} sidetnfs_network_config_t;

// Fase 11A: exact size guarded at compile time -- 2+2+36+68+4+16*4 = 176.
// If this ever fails, GEMDRVEMUL_SIDETNFS_NETWORK_* offsets in gemdrvemul.h
// (which are computed independently, field-length-by-field-length, not
// from sizeof()) and this struct have silently drifted apart.
_Static_assert(sizeof(sidetnfs_network_config_t) == 176, "sidetnfs_network_config_t must stay exactly 176 bytes");

typedef enum
{
    SIDETNFS_NETCONFIG_STATUS_OK = 0,
    SIDETNFS_NETCONFIG_STATUS_INVALID_SSID = 1,
    SIDETNFS_NETCONFIG_STATUS_INVALID_PASSWORD = 2,
    SIDETNFS_NETCONFIG_STATUS_INVALID_AUTH_MODE = 3,
    SIDETNFS_NETCONFIG_STATUS_INVALID_COUNTRY = 4,
    SIDETNFS_NETCONFIG_STATUS_INVALID_DHCP = 5,
    SIDETNFS_NETCONFIG_STATUS_INVALID_IP = 6,
    SIDETNFS_NETCONFIG_STATUS_INVALID_NETMASK = 7,
    SIDETNFS_NETCONFIG_STATUS_INVALID_GATEWAY = 8,
    SIDETNFS_NETCONFIG_STATUS_INVALID_DNS = 9,
    SIDETNFS_NETCONFIG_STATUS_NOT_STAGED = 10,
    SIDETNFS_NETCONFIG_STATUS_FLASH_WRITE_FAILED = 11,
    SIDETNFS_NETCONFIG_STATUS_FLASH_VERIFY_FAILED = 12
} sidetnfs_netconfig_status_t;

// GET_NETWORK_CONFIG: fills *out from the nine existing PARAM_WIFI_* config
// entries. No SD/WiFi/flash I/O -- find_entry() is a pure RAM lookup. An
// empty stored country is normalized to "XX" (display convenience only;
// the flash entry itself is left untouched). Returns the existing
// PARAM_WIFI_PASSWORD verbatim -- SIDETNFS.PRG must be able to read it back
// for editing (see report).
void sidetnfs_netconfig_get(sidetnfs_network_config_t *out);

// Pure validation, no side effects whatsoever -- never touches configData,
// flash, or the staging copy. Checked in this order: ssid length, password
// length, auth_mode range, country (exactly 2 letters, must be one
// get_country_code() accepts, incl. "XX"), use_dhcp (0/1 only), then --
// only when use_dhcp == 0 -- ip_address/netmask/gateway/primary_dns must
// each be valid IPv4 dotted-quad (ipaddr_aton()); when use_dhcp == 1 these
// four may be empty (or anything -- not re-validated, since DHCP means
// they're unused).
sidetnfs_netconfig_status_t sidetnfs_netconfig_validate(const sidetnfs_network_config_t *cfg);

// SET_NETWORK_CONFIG: validates first; only on SIDETNFS_NETCONFIG_STATUS_OK
// does it overwrite the RAM-only staging copy. On any validation failure,
// the previous staging copy (if any) is left completely untouched. Never
// touches configData, flash, or the active network connection.
sidetnfs_netconfig_status_t sidetnfs_netconfig_stage(const sidetnfs_network_config_t *cfg);

// SAVE_NETWORK_CONFIG: SIDETNFS_NETCONFIG_STATUS_NOT_STAGED if
// sidetnfs_netconfig_stage() was never called successfully (or only ever
// failed) since boot. Otherwise re-validates the staged copy (defense in
// depth -- staging already validated it once), builds a clean local copy,
// updates the nine existing PARAM_WIFI_* configData entries via
// put_string()/put_integer()/put_bool(), calls write_all_entries() (see
// its own Fase 11A page-alignment fix in config.c), reads the same nine
// entries back via a fresh load_all_entries()-independent XIP re-read, and
// only reports SIDETNFS_NETCONFIG_STATUS_OK if every one of the nine
// fields byte-for-byte round-trips. Never disconnects/restarts the active
// WiFi connection -- the new configuration only takes effect on a later,
// separate apply/reinit path (not part of this phase).
sidetnfs_netconfig_status_t sidetnfs_netconfig_save(void);

// Test/introspection helper: true once sidetnfs_netconfig_stage() has
// succeeded at least once (and stays true across a later failed stage/save
// -- only ever cleared by another successful stage overwriting it, never
// automatically). Mirrors the same "never silently reset" contract SAVE
// itself relies on for NOT_STAGED.
bool sidetnfs_netconfig_is_staged(void);

#endif // SIDETNFS_NETCONFIG_H

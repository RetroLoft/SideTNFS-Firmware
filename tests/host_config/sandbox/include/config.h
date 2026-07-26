/* Fase 2 host-test stub: the real romemul/include/config.h pulls in Pico
 * SDK hardware headers indirectly via config.c. This stub provides only
 * what sidetnfs_system_config.c's migration bridge actually touches:
 * the ConfigEntry/ConfigData layout, the legacy PARAM_ keys (exact same
 * string values as the real config.h), and find_entry()/configData,
 * which the test itself populates directly (never a real
 * load_all_entries()/write_all_entries() call).
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_ENTRIES 48
#define MAX_KEY_LENGTH 20
#define MAX_STRING_VALUE_LENGTH 64

#define PARAM_GEMDRIVE_RTC "GEMDRIVE_RTC"
#define PARAM_RTC_NTP_SERVER_HOST "RTC_NTP_SERVER_HOST"
#define PARAM_RTC_UTC_OFFSET "RTC_UTC_OFFSET"

#define PARAM_WIFI_AUTH "WIFI_AUTH"
#define PARAM_WIFI_COUNTRY "WIFI_COUNTRY"
#define PARAM_WIFI_DHCP "WIFI_DHCP"
#define PARAM_WIFI_DNS "WIFI_DNS"
#define PARAM_WIFI_IP "WIFI_IP"
#define PARAM_WIFI_NETMASK "WIFI_NETMASK"
#define PARAM_WIFI_GATEWAY "WIFI_GATEWAY"
#define PARAM_WIFI_PASSWORD "WIFI_PASSWORD"
#define PARAM_WIFI_SSID "WIFI_SSID"

typedef uint16_t DataType;

typedef struct
{
    char key[MAX_KEY_LENGTH];
    DataType dataType;
    char value[MAX_STRING_VALUE_LENGTH];
} ConfigEntry;

typedef struct
{
    uint32_t magic;
    ConfigEntry entries[MAX_ENTRIES];
    size_t count;
} ConfigData;

extern ConfigData configData;

ConfigEntry *find_entry(const char *key);

// Fase 2B: test driver provides this directly (settable), mirroring
// config.c's real config_loaded_from_real_flash().
bool config_loaded_from_real_flash(void);

#endif // CONFIG_H

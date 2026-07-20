/* Fase 11A host-test stub: the real romemul/include/config.h pulls in Pico
 * SDK hardware headers (hardware/flash.h, hardware/sync.h, hardware/dma.h,
 * hardware/watchdog.h, pico/cyw43_arch.h) a host toolchain can't build.
 * This stub provides the same ConfigEntry/ConfigData layout and PARAM_WIFI_*
 * keys (exact same values as the real romemul/include/config.h), plus
 * declarations for find_entry()/put_string()/put_integer()/put_bool()/
 * write_all_entries() -- faithful copies of config.c's real logic (config.c
 * itself is not host-compilable) live in test_netconfig.c, including a
 * fake-flash-backed write_all_entries() that mirrors the real Fase 11A
 * page-alignment fix exactly, so the SAVE readback path in
 * sidetnfs_netconfig.c (XIP_BASE + CONFIG_FLASH_OFFSET) resolves to that
 * fake flash buffer instead of a real XIP address.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MAX_ENTRIES 48
#define MAX_KEY_LENGTH 20
#define MAX_STRING_VALUE_LENGTH 64

#define PARAM_WIFI_AUTH "WIFI_AUTH"
#define PARAM_WIFI_COUNTRY "WIFI_COUNTRY"
#define PARAM_WIFI_DHCP "WIFI_DHCP"
#define PARAM_WIFI_DNS "WIFI_DNS"
#define PARAM_WIFI_GATEWAY "WIFI_GATEWAY"
#define PARAM_WIFI_IP "WIFI_IP"
#define PARAM_WIFI_NETMASK "WIFI_NETMASK"
#define PARAM_WIFI_PASSWORD "WIFI_PASSWORD"
#define PARAM_WIFI_SSID "WIFI_SSID"

#define TYPE_INT ((uint16_t)0)
#define TYPE_STRING ((uint16_t)1)
#define TYPE_BOOL ((uint16_t)2)

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
int put_bool(const char key[MAX_KEY_LENGTH], bool value);
int put_string(const char key[MAX_KEY_LENGTH], const char *value);
int put_integer(const char key[MAX_KEY_LENGTH], int value);
int write_all_entries(void);

// Fase 11A: resolves sidetnfs_netconfig.c's
// "(const uint8_t *)(XIP_BASE + CONFIG_FLASH_OFFSET)" readback expression
// to g_fake_flash (defined in test_netconfig.c), not a real XIP address.
extern uint8_t g_fake_flash[8192];
#define XIP_BASE ((uintptr_t)g_fake_flash)
#define CONFIG_FLASH_OFFSET ((uint32_t)0)
#define CONFIG_FLASH_SIZE ((uint32_t)sizeof(g_fake_flash))

#endif // CONFIG_H

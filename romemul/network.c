#include "include/network.h"

static ConnectionStatus connection_status = DISCONNECTED;
static ConnectionStatus previous_connection_status = NOT_SUPPORTED;
WifiScanData wifiScanData;
static char wifi_hostname[32];
static ip_addr_t current_ip;
static uint8_t cyw43_mac[6];
static bool cyw43_initialized = false;

int time_passed(absolute_time_t *t, uint32_t ms)
{
    if (t == NULL)
        return -1; // Error: invalid pointer

    absolute_time_t t_now = get_absolute_time(); // Get the current time

    // If *t is not initialized, or if the desired time has passed
    if (to_us_since_boot(*t) == 0 ||
        absolute_time_diff_us(*t, t_now) >= (ms * 1000))
    {

        *t = t_now; // Reset *t to the current time for the next check
        return 1;   // Time has passed
    }

    return 0; // Time has not yet passed
}

u_int32_t get_auth_pico_code(u_int16_t connect_code)
{
    switch (connect_code)
    {
    case 0:
        return CYW43_AUTH_OPEN;
    case 1:
    case 2:
        return CYW43_AUTH_WPA_TKIP_PSK;
    case 3:
    case 4:
    case 5:
        return CYW43_AUTH_WPA2_AES_PSK;
    case 6:
    case 7:
    case 8:
        return CYW43_AUTH_WPA2_MIXED_PSK;
    default:
        return CYW43_AUTH_OPEN;
    }
}

ConnectionStatus get_connection_status()
{
    return connection_status;
}

ConnectionStatus get_previous_connection_status()
{
    return previous_connection_status;
}

void network_swap_auth_data(uint16_t *dest_ptr_word)
{
    // We need to change the endianness of the ssid and password
    char *base = (char *)(dest_ptr_word);
    WifiNetworkAuthInfo *authInfo = (WifiNetworkAuthInfo *)base;

    // Create a temporary buffer to hold the SSID data
    char tmp[MAX_SSID_LENGTH] = {0};
    memcpy(tmp, authInfo->ssid, MAX_SSID_LENGTH);    // Copy the SSID data to the temporary buffer
    CHANGE_ENDIANESS_BLOCK16(&tmp, MAX_SSID_LENGTH); // Swap the SSID data
    // Write the result back to the SSID field safely
    memcpy(authInfo->ssid, tmp, MAX_SSID_LENGTH);

    // Create a temporary buffer to hold the password data
    char tmp_password[MAX_PASSWORD_LENGTH] = {0};
    memcpy(tmp_password, authInfo->password, MAX_PASSWORD_LENGTH); // Copy the password data to the temporary buffer
    CHANGE_ENDIANESS_BLOCK16(&tmp_password, MAX_PASSWORD_LENGTH);  // Swap the password data
    // Write the result back to the password field safely
    memcpy(authInfo->password, tmp_password, MAX_PASSWORD_LENGTH);

    // No need to swap the auth_mode uint16_t
}

void network_swap_data(uint16_t *dest_ptr_word, uint16_t total_items)
{
    // Skip the MAGIC number (assumed to be a 32-bit value)
    char *ssid_base = (char *)(dest_ptr_word) + sizeof(uint32_t);
    WifiNetworkInfo *netInfo = (WifiNetworkInfo *)ssid_base;

    for (uint16_t i = 0; i < total_items; i++)
    {
        // Create a temporary buffer to hold the SSID data
        char tmp[MAX_SSID_LENGTH] = {0};
        memcpy(tmp, netInfo[i].ssid, MAX_SSID_LENGTH);   // Copy the SSID data to the temporary buffer
        CHANGE_ENDIANESS_BLOCK16(&tmp, MAX_SSID_LENGTH); // Swap the SSID data
        // Write the result back to the SSID field safely
        memcpy(netInfo[i].ssid, tmp, MAX_SSID_LENGTH);

        // Create a temporary buffer to hold the BSSID data
        char tmp_bssid[MAX_BSSID_LENGTH] = {0};
        memcpy(tmp_bssid, netInfo[i].bssid, MAX_BSSID_LENGTH);  // Copy the BSSID data to the temporary buffer
        CHANGE_ENDIANESS_BLOCK16(&tmp_bssid, MAX_BSSID_LENGTH); // Swap the BSSID data
        // Write the result back to the BSSID field safely
        memcpy(netInfo[i].bssid, tmp_bssid, MAX_BSSID_LENGTH);
    }
}

void network_swap_connection_data(uint16_t *dest_ptr_word)
{
    // No need to swap the uint16_t
    CHANGE_ENDIANESS_BLOCK16(dest_ptr_word, sizeof(ConnectionData) - sizeof(uint16_t) * 6);
}

uint32_t get_country_code(char *c, char **valid_country_str)
{
    *valid_country_str = "XX";
    // empty configuration select worldwide
    if (strlen(c) == 0)
    {
        return CYW43_COUNTRY_WORLDWIDE;
    }

    if (strlen(c) != 2)
    {
        return CYW43_COUNTRY_WORLDWIDE;
    }

    // current supported country code https://www.raspberrypi.com/documentation/pico-sdk/networking.html#CYW43_COUNTRY_
    // ISO-3166-alpha-2
    // XX select worldwide
    char *valid_country_code[] = {
        "XX", "AU", "AR", "AT", "BE", "BR", "CA", "CL",
        "CN", "CO", "CZ", "DK", "EE", "FI", "FR", "DE",
        "GR", "HK", "HU", "IS", "IN", "IL", "IT", "JP",
        "KE", "LV", "LI", "LT", "LU", "MY", "MT", "MX",
        "NL", "NZ", "NG", "NO", "PE", "PH", "PL", "PT",
        "SG", "SK", "SI", "ZA", "KR", "ES", "SE", "CH",
        "TW", "TH", "TR", "GB", "US"};

    char country[3] = {toupper(c[0]), toupper(c[1]), 0};
    for (int i = 0; i < (sizeof(valid_country_code) / sizeof(valid_country_code[0])); i++)
    {
        if (!strcmp(country, valid_country_code[i]))
        {
            *valid_country_str = valid_country_code[i];
            return CYW43_COUNTRY(country[0], country[1], 0);
        }
    }
    return CYW43_COUNTRY_WORLDWIDE;
}

const char *pico_serial_str()
{
    static char buf[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
    pico_unique_board_id_t board_id;

    memset(&board_id, 0, sizeof(board_id));
    pico_get_unique_board_id(&board_id);
    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++)
        snprintf(&buf[i * 2], 3, "%02x", board_id.id[i]);

    return buf;
}

static int16_t get_rssi(void)
{
    static absolute_time_t ABSOLUTE_TIME_INITIALIZED_VAR(rssi_poll_counter, 0);
    static int32_t rssi_tmp = 0;
    static int32_t rssi_polling_interval = 0;

    if (rssi_polling_interval == 0)
    {
        rssi_polling_interval = get_network_status_polling_ms();
    }

    if (time_passed(&rssi_poll_counter, rssi_polling_interval))
    {
        cyw43_ioctl(&cyw43_state, 254, sizeof rssi_tmp, (uint8_t *)&rssi_tmp, CYW43_ITF_STA);
    }
    return (int16_t)rssi_tmp;
}

void wifi_link_callback(struct netif *netif)
{
    DPRINTF("WiFi Link: %s\n", (netif_is_link_up(netif) ? "UP" : "DOWN"));
}

void network_status_callback(struct netif *netif)
{
    if (netif_is_up(netif))
    {
        DPRINTF("WiFi Status: UP (%s)\n", ipaddr_ntoa(netif_ip_addr4(netif)));
        ip_addr_set(&current_ip, netif_ip_addr4(netif));
    }
    else
    {
        DPRINTF("WiFi Status: DOWN\n");
    }
}

// We MUST call this function and avoid the cy43_arch_deinit() function to avoid a crash
void network_terminate()
{
    // This flag is important, because calling a cyw43 function before the initialization will cause a crash
    cyw43_initialized = false;
    cyw43_arch_deinit();
}

// Fase 2B: the real WiFi-driver bring-up, parameterized on an
// already-resolved country code -- no ConfigEntry access at all. Shared
// by both network_wifi_init() (legacy, resolves country from
// ConfigEntry first) and the SideTNFS path (resolves country from
// sidetnfs_system_settings_t first) below.
static int network_wifi_init_with_country_code(uint32_t country)
{
    // This flag is important, because calling a cyw43 function before the initialization will cause a crash
    cyw43_initialized = true;
    DPRINTF("CYW43 Logging level: %d\n", CYW43_VERBOSE_DEBUG);

    int res;
    DPRINTF("Initialization WiFi...\n");

    if ((res = cyw43_arch_init_with_country(country)))
    {
        DPRINTF("Failed to initialize WiFi: %d\n", res);
        return -1;
    }

    DPRINTF("Enabling STA mode...\n");
    cyw43_arch_enable_sta_mode();

    // Setting the power management -- PARAM_WIFI_POWER stays a legacy-only
    // knob (not part of sidetnfs_system_config's WiFi/Network/RTC field
    // list; Fase 2B deliberately leaves it as-is for both callers).
    uint32_t pm_value = 0xa11140; // 0: Disable PM
    ConfigEntry *pm_entry = find_entry(PARAM_WIFI_POWER);
    if (pm_entry != NULL)
    {
        pm_value = strtoul(pm_entry->value, NULL, 16);
    }
    if (pm_value < 5)
    {
        switch (pm_value)
        {
        case 0:
            pm_value = 0xa11140; // DISABLED_PM
            break;
        case 1:
            pm_value = CYW43_PERFORMANCE_PM; // PERFORMANCE_PM
            break;
        case 2:
            pm_value = CYW43_AGGRESSIVE_PM; // AGGRESSIVE_PM
            break;
        case 3:
            pm_value = CYW43_DEFAULT_PM; // DEFAULT_PM
            break;
        default:
            pm_value = CYW43_NO_POWERSAVE_MODE; // NO_POWERSAVE_MODE
            break;
        }
    }
    DPRINTF("Setting power management to: %08x\n", pm_value);
    cyw43_wifi_pm(&cyw43_state, pm_value);
    return 0;
}

// Legacy entry point -- resolves country from the old ConfigEntry store,
// same behavior as before Fase 2B (including the put_string() write-back
// of the normalized country code). Only called from network_init()'s own
// legacy adapter below, and from any other pre-Fase-2B caller that used
// to call this directly.
int network_wifi_init()
{
    uint32_t country = CYW43_COUNTRY_WORLDWIDE;
    ConfigEntry *country_entry = find_entry(PARAM_WIFI_COUNTRY);
    if (country_entry != NULL)
    {
        char *valid;
        country = get_country_code(country_entry->value, &valid);
        put_string(PARAM_WIFI_COUNTRY, valid);
    }
    int res = network_wifi_init_with_country_code(country);
    if (res == 0)
    {
        DPRINTF("Country: %s\n", country_entry != NULL ? country_entry->value : "?");
    }
    return res;
}

// Fase 2B: SideTNFS path -- resolves country from *settings (RAM only,
// never writes back to ConfigEntry, unlike the legacy path above).
// Fase 4 (CYW43-initialisatie en WiFi-timeouts): no longer static -- this
// is now the single, sole CYW43 init call for the whole boot, called
// exactly once from main() (before anything else touches cyw43, so
// blink_morse()/the factory-reset and force-config-recovery LED
// indicators all work correctly off this one init). network_init_with_settings()
// below still calls this too, but only reaches it when `cyw43_initialized`
// is false -- never true after main()'s own call succeeds, so this is a
// no-op there for the whole rest of the boot except across an explicit
// network_terminate() + reinit retry cycle (see gemdrvemul.c's bounded
// WiFi-connect retry loop). Replaces the old pattern of a generic,
// country-less cyw43_arch_init() in main() followed by a
// cyw43_arch_deinit()+cyw43_arch_init_with_country() cycle here.
int network_wifi_init_for_settings(const sidetnfs_system_settings_t *settings)
{
    // get_country_code() takes a non-const char* (it uppercases in
    // place) -- settings is const here (this function must never modify
    // the caller's copy), so a local mutable copy is made first rather
    // than casting the const away.
    char country_copy[SIDETNFS_SYS_COUNTRY_LEN];
    strncpy(country_copy, settings->country, sizeof(country_copy) - 1);
    country_copy[sizeof(country_copy) - 1] = '\0';

    char *valid = NULL;
    uint32_t country = get_country_code(country_copy, &valid);
    return network_wifi_init_with_country_code(country);
}

int network_init_with_settings(bool force, bool async, char **pass, const sidetnfs_system_settings_t *settings)
{
    if (!cyw43_initialized)
    {
        // Setup the underlying WiFi stack
        network_wifi_init_for_settings(settings);
    }

    int res;

    // Set hostname -- PARAM_HOSTNAME stays a legacy-only field (not part
    // of sidetnfs_system_config's WiFi/Network/RTC scope).
    char *hostname = find_entry(PARAM_HOSTNAME)->value;

    struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];

    cyw43_arch_lwip_begin();

    if ((hostname != NULL) && (strlen(hostname) > 0))
    {
        strncpy(wifi_hostname, hostname, sizeof(wifi_hostname));
    }
    else
    {
        snprintf(wifi_hostname, sizeof(wifi_hostname), "SidecarT-%s", pico_serial_str());
    }
    DPRINTF("Hostname: %s\n", wifi_hostname);
    netif_set_hostname(n, wifi_hostname);

    // Set callbacks
    netif_set_link_callback(n, wifi_link_callback);
    netif_set_status_callback(n, network_status_callback);

    // DHCP or static IP
    if (settings->use_dhcp)
    {
        DPRINTF("DHCP enabled\n");
    }
    else
    {
        DPRINTF("Static IP enabled\n");
        dhcp_stop(n);
        ip_addr_t ipaddr, netmask, gw;
        ipaddr.addr = ipaddr_addr(settings->ip_address);
        netmask.addr = ipaddr_addr(settings->netmask);
        gw.addr = ipaddr_addr(settings->gateway);
        netif_set_addr(n, &ipaddr, &netmask, &gw);
        DPRINTF("IP: %s\n", ipaddr_ntoa(&ipaddr));
        DPRINTF("Netmask: %s\n", ipaddr_ntoa(&netmask));
        DPRINTF("Gateway: %s\n", ipaddr_ntoa(&gw));

        // Fase 2B: sidetnfs_system_settings_t only ever carries one DNS
        // server (primary_dns) -- the SideTNFS config protocol itself
        // never supported the legacy store's comma-separated
        // second-DNS convention, so this is not a new limitation.
        if (strlen(settings->primary_dns) == 0)
        {
            DPRINTF("Error: DNS configuration is missing.\n");
        }
        else
        {
            ip_addr_t dns1_ip;
            if ((dns1_ip.addr = ipaddr_addr(settings->primary_dns)) == IPADDR_NONE)
            {
                DPRINTF("Error: Invalid DNS1 address.\n");
            }
            else
            {
                dns_setserver(0, &dns1_ip);
                DPRINTF("DNS1: %s\n", ipaddr_ntoa(&dns1_ip));
            }
        }
    }
    netif_set_up(n);

    cyw43_arch_lwip_end();

    // Get the MAC address
    if ((res = cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, cyw43_mac)))
    {
        DPRINTF("Failed to get MAC address: %d\n", res);
        cyw43_arch_deinit();
        return -2;
    }

    if (strlen(settings->ssid) == 0)
    {
        DPRINTF("No SSID found in config. Can't connect\n");
        return -3;
    }
    // auth_mode has no "unset" sentinel of its own in sidetnfs_system_settings_t
    // (unlike the legacy PARAM_WIFI_AUTH, which can be an empty string) --
    // 0 (CYW43_AUTH_OPEN via get_auth_pico_code()) is a valid, meaningful
    // value here, so there is nothing to reject the way the legacy path
    // rejects an empty PARAM_WIFI_AUTH string.
    char *password_value = NULL;
    if (*pass == NULL)
    {
        if (strlen(settings->password) > 0)
        {
            password_value = strdup(settings->password);
        }
        else
        {
            DPRINTF("No password found in config. Trying to connect without password\n");
        }
    }
    else
    {
        password_value = strdup(*pass);
    }
    DPRINTF("The password is: %s\n", password_value);

    uint32_t auth_value = get_auth_pico_code((uint16_t)settings->auth_mode);
    int error_code = 0;
    if (!async)
    {
        // PARAM_WIFI_CONNECT_TIMEOUT stays a legacy-only knob (not part of
        // sidetnfs_system_config's field list) -- Fase 2B leaves it as-is
        // for both callers; in practice GEMDRIVE always calls with
        // async=true (see gemdrvemul.c), so this branch is not exercised
        // by the SideTNFS path.
        uint32_t network_timeout = NETWORK_CONNECTION_TIMEOUT;
        if (find_entry(PARAM_WIFI_CONNECT_TIMEOUT) != NULL)
        {
            network_timeout = atoi(find_entry(PARAM_WIFI_CONNECT_TIMEOUT)->value) * 1000;
        }
        uint16_t retries = 3;
        do
        {
            DPRINTF("Connecting to SSID=%s, password=%s, auth=%08x. SYNC. Retry: %d\n", settings->ssid, password_value, auth_value, retries);
            error_code = cyw43_arch_wifi_connect_timeout_ms(settings->ssid, password_value, auth_value, network_timeout);
        } while (error_code != 0 && retries--);
    }
    else
    {
        DPRINTF("Connecting to SSID=%s, password=%s, auth=%08x. ASYNC\n", settings->ssid, password_value, auth_value);
        error_code = cyw43_arch_wifi_connect_async(settings->ssid, password_value, auth_value);
    }
    free(password_value);
    if (error_code != 0)
    {
        DPRINTF("Failed to connect to WiFi: %d\n", error_code);
        return -5;
    }
    DPRINTF("Connected. Check the connection status...\n");
    return 0;
}

// Fase 2B: legacy adapter -- builds a sidetnfs_system_settings_t from the
// old ConfigEntry store's PARAM_WIFI_* entries (exact same fields/
// fallback-to-empty behavior network_init() always had) and delegates to
// network_init_with_settings(), the one real implementation. Used by the
// old configurator/floppy-emulator/standalone RTC-emulator -- GEMDRIVE/
// SideTNFS never calls this as of Fase 2B (see gemdrvemul.c, which calls
// network_init_with_settings() directly with settings from
// sidetnfs_system_config_get()).
int network_init(bool force, bool async, char **pass)
{
    sidetnfs_system_settings_t legacy;
    memset(&legacy, 0, sizeof(legacy));

    ConfigEntry *auth_entry = find_entry(PARAM_WIFI_AUTH);
    legacy.auth_mode = (uint16_t)((auth_entry != NULL && strlen(auth_entry->value) > 0) ? atoi(auth_entry->value) : 0);

    ConfigEntry *dhcp_entry = find_entry(PARAM_WIFI_DHCP);
    legacy.use_dhcp = (uint16_t)((dhcp_entry != NULL && (dhcp_entry->value[0] == 't' || dhcp_entry->value[0] == 'T')) ? 1 : 0);

    ConfigEntry *ssid_entry = find_entry(PARAM_WIFI_SSID);
    if (ssid_entry != NULL) strncpy(legacy.ssid, ssid_entry->value, sizeof(legacy.ssid) - 1);

    ConfigEntry *password_entry = find_entry(PARAM_WIFI_PASSWORD);
    if (password_entry != NULL) strncpy(legacy.password, password_entry->value, sizeof(legacy.password) - 1);

    ConfigEntry *country_entry = find_entry(PARAM_WIFI_COUNTRY);
    if (country_entry != NULL) strncpy(legacy.country, country_entry->value, sizeof(legacy.country) - 1);

    ConfigEntry *ip_entry = find_entry(PARAM_WIFI_IP);
    if (ip_entry != NULL) strncpy(legacy.ip_address, ip_entry->value, sizeof(legacy.ip_address) - 1);

    ConfigEntry *netmask_entry = find_entry(PARAM_WIFI_NETMASK);
    if (netmask_entry != NULL) strncpy(legacy.netmask, netmask_entry->value, sizeof(legacy.netmask) - 1);

    ConfigEntry *gateway_entry = find_entry(PARAM_WIFI_GATEWAY);
    if (gateway_entry != NULL) strncpy(legacy.gateway, gateway_entry->value, sizeof(legacy.gateway) - 1);

    // Fase 2B: the legacy PARAM_WIFI_DNS entry can hold a comma-separated
    // pair ("dns1,dns2") -- network_init_with_settings() only ever reads
    // a single primary_dns, matching sidetnfs_system_settings_t's own
    // shape (see that function's own comment). Only the first address is
    // carried over here; a legacy caller relying on a second DNS server
    // loses it via this adapter -- acceptable for the legacy-only modes
    // this path now serves (unchanged from what SideTNFS's own protocol
    // already supported).
    ConfigEntry *dns_entry = find_entry(PARAM_WIFI_DNS);
    if (dns_entry != NULL)
    {
        char dns_copy[MAX_STRING_VALUE_LENGTH];
        strncpy(dns_copy, dns_entry->value, sizeof(dns_copy) - 1);
        dns_copy[sizeof(dns_copy) - 1] = '\0';
        char *dns1 = strtok(dns_copy, ",");
        if (dns1 != NULL)
        {
            strncpy(legacy.primary_dns, dns1, sizeof(legacy.primary_dns) - 1);
        }
    }

    return network_init_with_settings(force, async, pass, &legacy);
}

void network_scan()
{
    if (!cyw43_initialized)
    {
        network_wifi_init();
    }
    int scan_result(void *env, const cyw43_ev_scan_result_t *result)
    {
        int bssid_exists(WifiNetworkInfo * network)
        {
            for (size_t i = 0; i < wifiScanData.count; i++)
            {
                if (strcmp(wifiScanData.networks[i].bssid, network->bssid) == 0)
                {
                    return 1; // BSSID found
                }
            }
            return 0; // BSSID not found
        }
        if (result && wifiScanData.count < MAX_NETWORKS)
        {
            WifiNetworkInfo network;

            // Copy SSID
            snprintf(network.ssid, sizeof(network.ssid), "%s", result->ssid);

            // Format BSSID
            snprintf(network.bssid, sizeof(network.bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                     result->bssid[0], result->bssid[1], result->bssid[2], result->bssid[3], result->bssid[4], result->bssid[5]);

            // Store authentication mode
            network.auth_mode = result->auth_mode;

            // Store signal strength
            network.rssi = result->rssi;

            // Check if BSSID already exists
            if (!bssid_exists(&network))
            {
                if (strlen(network.ssid) > 0)
                {
                    wifiScanData.networks[wifiScanData.count] = network;
                    wifiScanData.count++;
                    DPRINTF("FOUND NETWORK %s (%s) with auth %d and RSSI %d\n", network.ssid, network.bssid, network.auth_mode, network.rssi);
                }
            }
        }
        return 0;
    }
    if (!cyw43_wifi_scan_active(&cyw43_state))
    {
        DPRINTF("Scanning networks...\n");
        cyw43_wifi_scan_options_t scan_options = {0};
        int err = cyw43_wifi_scan(&cyw43_state, &scan_options, NULL, scan_result);
        if (err == 0)
        {
            DPRINTF("Performing wifi scan\n");
        }
        else
        {
            DPRINTF("Failed to start scan: %d\n", err);
        }
    }
    else
    {
        DPRINTF("Scan already in progress\n");
    }
}

void dhcp_set_ntp_servers(u8_t num_ntp_servers, const ip4_addr_t *ntp_server_addrs)
{
    if (num_ntp_servers > LWIP_DHCP_MAX_NTP_SERVERS)
    {
        num_ntp_servers = LWIP_DHCP_MAX_NTP_SERVERS;
    }
    for (u8_t i = 0; i < num_ntp_servers; i++)
    {
        DPRINTF("Reading NTP server %d: %s\n", i, ip4addr_ntoa(&ntp_server_addrs[i]));
    }
}

void network_wifi_disconnect()
{
    // The library seems to have a bug when disconnecting. It doesn't work. So I'm using the ioctl directly
    int custom_cyw43_wifi_leave(cyw43_t * self, int itf)
    {
        // Disassociate with SSID
        cyw43_wifi_set_up(self,
                          itf,
                          false,
                          CYW43_COUNTRY_WORLDWIDE);
        return cyw43_ioctl(self, 0x76, 0, NULL, itf);
    }

    int error = custom_cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    if (error == 0)
    {
        DPRINTF("Disconnected\n");
    }
    else
    {
        DPRINTF("Failed to disconnect: %d\n", error);
    }
    connection_status = DISCONNECTED;
}

inline void wait_cyw43_with_polling(uint32_t milliseconds)
{
    uint64_t start_time = time_us_64();
    cyw43_arch_poll();
    cyw43_arch_wait_for_work_until(make_timeout_time_ms(milliseconds * 0.1));
    while (time_us_64() - start_time < 1000 * milliseconds * 0.9)
    {
        sleep_ms(10);
    }
}

ConnectionStatus get_network_connection_status()
{
    ConnectionStatus old_previous_connection_status = previous_connection_status;
    previous_connection_status = connection_status;
    int link_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    switch (link_status)
    {
    case CYW43_LINK_DOWN:
        connection_status = DISCONNECTED;
        break;
    case CYW43_LINK_JOIN:
        connection_status = CONNECTED_WIFI;
        break;
    case CYW43_LINK_NOIP:
        connection_status = CONNECTED_WIFI_NO_IP;
        break;
    case CYW43_LINK_UP:
        connection_status = CONNECTED_WIFI_IP;
        break;
    case CYW43_LINK_FAIL:
        connection_status = GENERIC_ERROR;
        break;
    case CYW43_LINK_NONET:
        connection_status = CONNECT_FAILED_ERROR;
        break;
    case CYW43_LINK_BADAUTH:
        connection_status = BADAUTH_ERROR;
        break;
    default:
        connection_status = GENERIC_ERROR;
    }

    if (connection_status != old_previous_connection_status)
    {
        switch (link_status)
        {
        case CYW43_LINK_DOWN:
            DPRINTF("Link down\n");
            break;
        case CYW43_LINK_JOIN:
            DPRINTF("Link join. Connected!\n");
            break;
        case CYW43_LINK_NOIP:
            DPRINTF("Link no IP\n");
            break;
        case CYW43_LINK_UP:
            DPRINTF("Link up\n");
            break;
        case CYW43_LINK_FAIL:
            DPRINTF("Link fail\n");
            break;
        case CYW43_LINK_NONET:
            DPRINTF("Link no net\n");
            break;
        case CYW43_LINK_BADAUTH:
            DPRINTF("Link bad auth\n");
            break;
        default:
            DPRINTF("Link unknown\n");
        }
    }
    return connection_status;
}

void network_safe_poll()
{
    if (cyw43_initialized)
    {
        cyw43_arch_poll();
    }
}

uint32_t get_network_status_polling_ms()
{
    uint32_t network_status_polling_ms = NETWORK_POLL_INTERVAL * 1000;
    ConfigEntry *default_network_status_polling_sec = find_entry(PARAM_NETWORK_STATUS_SEC);
    if (default_network_status_polling_sec != NULL)
    {
        network_status_polling_ms = atoi(default_network_status_polling_sec->value) * 1000;
        // If the value is too small, set the minimum value
        if (network_status_polling_ms < NETWORK_POLL_INTERVAL_MIN * 1000)
        {
            network_status_polling_ms = NETWORK_POLL_INTERVAL_MIN * 1000;
            DPRINTF("NETWORK_STATUS_SEC value too small. Changing to minimum value: %d\n", network_status_polling_ms);
        }
    }
    else
    {
        DPRINTF("%s not found in the config file. Using default value: %d\n", PARAM_NETWORK_STATUS_SEC, network_status_polling_ms);
    }
    return network_status_polling_ms;
}

uint16_t get_wifi_scan_poll_secs()
{
    uint16_t value = WIFI_SCAN_POLL_COUNTER;
    ConfigEntry *default_config_entry = find_entry(PARAM_WIFI_SCAN_SECONDS);
    if (default_config_entry != NULL)
    {
        value = atoi(default_config_entry->value);
    }
    else
    {
        DPRINTF("WIFI_SCAN_SECONDS not found in the config file. Disabling polling.\n");
    }
    if (value < WIFI_SCAN_POLL_COUNTER_MIN)
    {
        value = WIFI_SCAN_POLL_COUNTER_MIN;
        DPRINTF("WIFI_SCAN_SECONDS value too small. Changing to minimum value: %d\n", value);
    }
    return value;
}

u_int32_t get_ip_address()
{
    DPRINTF("IP: %s\n", ipaddr_ntoa(&current_ip));
    return cyw43_state.netif[0].ip_addr.addr;
}

u_int8_t *get_mac_address()
{
    DPRINTF("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", cyw43_mac[0], cyw43_mac[1], cyw43_mac[2], cyw43_mac[3], cyw43_mac[4], cyw43_mac[5]);
    return cyw43_state.mac;
}

u_int32_t get_netmask()
{
    DPRINTF("Netmask: %s\n", ipaddr_ntoa(&cyw43_state.netif[0].netmask));
    return cyw43_state.netif[0].netmask.addr;
}

u_int32_t get_gateway()
{
    DPRINTF("Gateway: %s\n", ipaddr_ntoa(&cyw43_state.netif[0].gw));
    return cyw43_state.netif[0].gw.addr;
}

u_int32_t get_dns()
{
    const ip_addr_t *dns_ip = dns_getserver(0);
    DPRINTF("DNS: %s\n", ipaddr_ntoa(dns_ip));
    return dns_ip->addr;
}

char *print_ipv4(u_int32_t ip)
{
    char *ip_str = malloc(16);
    snprintf(ip_str, 16, "%d.%d.%d.%d", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    return ip_str;
}

char *print_mac(uint8_t *mac_address)
{
    static char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac_address[0],
             mac_address[1],
             mac_address[2],
             mac_address[3],
             mac_address[4],
             mac_address[5]);
    return mac_str;
}

void get_connection_data(ConnectionData *connection_data)
{
    ConfigEntry *ssid = find_entry(PARAM_WIFI_SSID);
    ConfigEntry *wifi_auth = find_entry(PARAM_WIFI_AUTH);
    ConfigEntry *wifi_scan_interval = find_entry(PARAM_WIFI_SCAN_SECONDS);
    ConfigEntry *network_status_scan_interval = find_entry(PARAM_NETWORK_STATUS_SEC);
    ConfigEntry *file_downloading_timeout = find_entry(PARAM_DOWNLOAD_TIMEOUT_SEC);
    ConfigEntry *wifi_country = find_entry(PARAM_WIFI_COUNTRY);
    ConfigEntry *wifi_rssi_visible = find_entry(PARAM_WIFI_RSSI);
    connection_data->network_status = (u_int16_t)connection_status;
    snprintf(connection_data->ipv4_address, sizeof(connection_data->ipv4_address), "%s", "Not connected" + '\0');
    snprintf(connection_data->ipv6_address, sizeof(connection_data->ipv6_address), "%s", "Not connected" + '\0');
    snprintf(connection_data->mac_address, sizeof(connection_data->mac_address), "%s", "Not connected" + '\0');
    snprintf(connection_data->gw_ipv4_address, sizeof(connection_data->gw_ipv4_address), "%s", "Not connected" + '\0');
    snprintf(connection_data->netmask_ipv4_address, sizeof(connection_data->netmask_ipv4_address), "Not connected" + '\0');
    snprintf(connection_data->dns_ipv4_address, sizeof(connection_data->dns_ipv4_address), "%s", "Not connected" + '\0');
    connection_data->wifi_auth_mode = (uint16_t)atoi(wifi_auth->value);
    connection_data->wifi_scan_interval = get_wifi_scan_poll_secs();
    connection_data->network_status_poll_interval = (uint16_t)(get_network_status_polling_ms() / 1000);
    connection_data->file_downloading_timeout = (uint16_t)atoi(file_downloading_timeout->value);
    connection_data->rssi = 0;

    // If the country is empty, set it to XX. Otherwise, copy the first two characters
    if (wifi_country->value[0] == '\0')
    { // Check if the country value is empty
        snprintf(connection_data->wifi_country, 4, "XX\0\0");
    }
    else
    {
        snprintf(connection_data->wifi_country, 4, "%.2s\0\0", wifi_country->value);
    }

    switch (connection_status)
    {
    case CONNECTED_WIFI_IP:
    {
        snprintf(connection_data->ssid, sizeof(connection_data->ssid), "%s", ssid->value);
        snprintf(connection_data->ipv4_address, sizeof(connection_data->ipv4_address), "%s", print_ipv4(get_ip_address()));
        snprintf(connection_data->ipv6_address, sizeof(connection_data->ipv6_address), "%s", "Not implemented" + '\0');
        snprintf(connection_data->mac_address, sizeof(connection_data->mac_address), "%s", print_mac(get_mac_address()));
        snprintf(connection_data->gw_ipv4_address, sizeof(connection_data->gw_ipv4_address), "%s", print_ipv4(get_gateway()));
        snprintf(connection_data->gw_ipv6_address, sizeof(connection_data->gw_ipv6_address), "%s", "Not implemented" + '\0');
        snprintf(connection_data->netmask_ipv4_address, sizeof(connection_data->netmask_ipv4_address), "%s", print_ipv4(get_netmask()));
        snprintf(connection_data->netmask_ipv6_address, sizeof(connection_data->netmask_ipv6_address), "%s", "Not implemented" + '\0');
        snprintf(connection_data->dns_ipv4_address, sizeof(connection_data->dns_ipv4_address), "%s", print_ipv4(get_dns()));
        snprintf(connection_data->dns_ipv6_address, sizeof(connection_data->dns_ipv6_address), "%s", "Not implemented" + '\0');
        if ((wifi_rssi_visible != NULL) && (wifi_rssi_visible->value[0] == 't' || wifi_rssi_visible->value[0] == 'T'))
        {
            connection_data->rssi = get_rssi();
        }
        else {
            connection_data->rssi = 0;
        }
        break;
    }
    case CONNECTED_WIFI:
    case CONNECTED_WIFI_NO_IP:
    {
        snprintf(connection_data->ssid, sizeof(connection_data->ssid), "%s", ssid->value);
        snprintf(connection_data->ipv4_address, sizeof(connection_data->ipv4_address), "%s", "Waiting address" + '\0');
        snprintf(connection_data->ipv6_address, sizeof(connection_data->ipv6_address), "%s", "Waiting address" + '\0');
        snprintf(connection_data->mac_address, sizeof(connection_data->mac_address), "%s", "Waiting address" + '\0');
        snprintf(connection_data->gw_ipv4_address, sizeof(connection_data->gw_ipv4_address), "%s", "Waiting address" + '\0');
        snprintf(connection_data->gw_ipv6_address, sizeof(connection_data->gw_ipv6_address), "%s", "Waiting address" + '\0');
        snprintf(connection_data->netmask_ipv4_address, sizeof(connection_data->netmask_ipv4_address), "%s", "Waiting address" + '\0');
        snprintf(connection_data->netmask_ipv6_address, sizeof(connection_data->netmask_ipv6_address), "%s", "Waiting address" + '\0');
        snprintf(connection_data->dns_ipv4_address, sizeof(connection_data->dns_ipv4_address), "%s", "Waiting address" + '\0');
        snprintf(connection_data->dns_ipv6_address, sizeof(connection_data->dns_ipv6_address), "%s", "Waiting address" + '\0');
        if ((wifi_rssi_visible != NULL) && (wifi_rssi_visible->value[0] == 't' || wifi_rssi_visible->value[0] == 'T'))
        {
            connection_data->rssi = get_rssi();
        }
        else {
            connection_data->rssi = 0;
        }
        break;
    }
    case CONNECTING:
        snprintf(connection_data->ssid, MAX_SSID_LENGTH, "%s", "Initializing" + '\0');
        snprintf(connection_data->ipv4_address, sizeof(connection_data->ipv4_address), "%s", "Initializing" + '\0');
        snprintf(connection_data->ipv6_address, sizeof(connection_data->ipv6_address), "%s", "Initializing" + '\0');
        snprintf(connection_data->mac_address, sizeof(connection_data->mac_address), "%s", "Initializing" + '\0');
        snprintf(connection_data->gw_ipv4_address, sizeof(connection_data->gw_ipv4_address), "%s", "Initializing" + '\0');
        snprintf(connection_data->gw_ipv6_address, sizeof(connection_data->gw_ipv6_address), "%s", "Initializing" + '\0');
        snprintf(connection_data->netmask_ipv4_address, sizeof(connection_data->netmask_ipv4_address), "%s", "Initializing" + '\0');
        snprintf(connection_data->netmask_ipv6_address, sizeof(connection_data->netmask_ipv6_address), "%s", "Initializing" + '\0');
        snprintf(connection_data->dns_ipv4_address, sizeof(connection_data->dns_ipv4_address), "%s", "Initializing" + '\0');
        snprintf(connection_data->dns_ipv6_address, sizeof(connection_data->dns_ipv6_address), "%s", "Initializing" + '\0');
        break;
    case DISCONNECTED:
        snprintf(connection_data->ssid, MAX_SSID_LENGTH, "%s", "Not connected" + '\0');
        break;
    case CONNECT_FAILED_ERROR:
        snprintf(connection_data->ssid, MAX_SSID_LENGTH, "%s", "CONNECT FAILED ERROR!" + '\0');
        break;
    case BADAUTH_ERROR:
        snprintf(connection_data->ssid, MAX_SSID_LENGTH, "%s", "BAD AUTH ERROR!" + '\0');
        break;
    case NOT_SUPPORTED:
        snprintf(connection_data->ssid, MAX_SSID_LENGTH, "%s", "NETWORKING NOT SUPPORTED!" + '\0');
        break;
    default:
        snprintf(connection_data->ssid, MAX_SSID_LENGTH, "%s", "ERROR!" + '\0');
    }
}

void show_connection_data(ConnectionData *connection_data)
{
    DPRINTF("SSID: %s (%ddb) - Status: %d - IPv4: %s - IPv6: %s - GW:%s - Mask:%s - MAC:%s DNS:%s\n",
            connection_data->ssid,
            connection_data->rssi,
            connection_data->network_status,
            connection_data->ipv4_address,
            connection_data->ipv6_address,
            connection_data->gw_ipv4_address,
            connection_data->netmask_ipv4_address,
            connection_data->mac_address,
            connection_data->dns_ipv4_address);
    DPRINTF("WiFi country: %s - Auth mode: %d - Scan interval: %d - Network status poll interval: %d - File downloading timeout: %d\n",
            connection_data->wifi_country,
            connection_data->wifi_auth_mode,
            connection_data->wifi_scan_interval,
            connection_data->network_status_poll_interval,
            connection_data->file_downloading_timeout);
}

// This file ends here on purpose: it provides WiFi, DHCP, DNS and the
// connection/status plumbing, and nothing else.
//
// There is no HTTP client, no downloader and no firmware version or release
// check anywhere in SideTNFS -- the lwIP HTTP stack (pico_lwip_http) is not
// linked at all. Anything network-facing beyond the entry points above is
// TNFS (sidetnfs_probe.c) or NTP (rtcemul.c).

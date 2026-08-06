/**
 * File: sidetnfs_update_check.c
 * Description: See include/sidetnfs_update_check.h. Fetches version.txt
 * from the SideTNFS-Firmware GitHub repo over TLS and compares it against
 * this build's own RELEASE_VERSION.
 *
 * Certificate verification is intentionally OFF (ALTCP_MBEDTLS_AUTHMODE,
 * lwipopts.h). This connection only ever reads a short, public,
 * non-secret version string -- no credentials, no write, no code ever
 * executed from the response -- so the worst case of a spoofed response
 * is a wrong "update available"/"up to date" verdict, not a security
 * compromise. Skipping verification avoids embedding (and having to keep
 * current) a root CA certificate purely for this one low-stakes check.
 *
 * No TLS Server Name Indication (SNI) either: confirmed against the real
 * server that raw.githubusercontent.com's HTTP virtual-hosting is driven
 * entirely by the request's Host: header, not by SNI -- the connection
 * and response work identically with or without it, so the extra
 * complexity of reaching into lwIP's altcp_tls-internal mbedtls context
 * to set a hostname was not worth adding.
 */
#include "include/sidetnfs_update_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "include/debug.h"
#include "include/network.h" // network_safe_poll()

#define SIDETNFS_UPDATE_CHECK_HOST "raw.githubusercontent.com"
#define SIDETNFS_UPDATE_CHECK_PATH "/RetroLoft/SideTNFS-Firmware/refs/heads/main/version.txt"
#define SIDETNFS_UPDATE_CHECK_PORT 443u

// Whole check (DNS + TLS handshake + HTTP GET + response) bounded to
// this, in one shot -- there is no background retry, matching the same
// "one bounded attempt, no generation mechanism" contract the boot-time
// NTP wait already uses (gemdrvemul.c).
#define SIDETNFS_UPDATE_CHECK_TIMEOUT_MS 15000u
#define SIDETNFS_UPDATE_CHECK_POLL_STEP_US 1000u

// A version.txt response plus its (small, fixed) set of HTTP headers
// comfortably fits in a fraction of this -- generous headroom, not a
// tight fit.
#define SIDETNFS_UPDATE_RESP_BUF_SIZE 512u

typedef struct
{
    volatile bool dns_done;
    volatile bool dns_ok;
    ip_addr_t dns_addr;

    volatile bool request_sent;
    volatile bool closed;
    volatile bool error;

    char resp[SIDETNFS_UPDATE_RESP_BUF_SIZE];
    uint16_t resp_len;
} sidetnfs_update_ctx_t;

static void update_dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    sidetnfs_update_ctx_t *ctx = (sidetnfs_update_ctx_t *)arg;
    if (ipaddr != NULL)
    {
        ctx->dns_addr = *ipaddr;
        ctx->dns_ok = true;
    }
    ctx->dns_done = true;
}

static err_t update_recv_cb(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err)
{
    sidetnfs_update_ctx_t *ctx = (sidetnfs_update_ctx_t *)arg;
    if (err != ERR_OK || p == NULL)
    {
        // NULL pbuf is lwIP's normal "peer closed the connection" signal
        // -- expected here, since the request sends "Connection: close"
        // and this is how we know the whole response has arrived.
        ctx->closed = true;
        if (p != NULL)
        {
            altcp_recved(pcb, p->tot_len);
            pbuf_free(p);
        }
        return ERR_OK;
    }

    uint16_t copy_len = (uint16_t)p->tot_len;
    uint16_t space_left = (uint16_t)(sizeof(ctx->resp) - 1 - ctx->resp_len);
    if (copy_len > space_left)
    {
        copy_len = space_left; // defensive clamp -- never write past resp[]
    }
    if (copy_len > 0)
    {
        pbuf_copy_partial(p, ctx->resp + ctx->resp_len, copy_len, 0);
        ctx->resp_len = (uint16_t)(ctx->resp_len + copy_len);
    }
    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void update_err_cb(void *arg, err_t err)
{
    (void)err;
    sidetnfs_update_ctx_t *ctx = (sidetnfs_update_ctx_t *)arg;
    // lwIP has already freed the pcb by the time this fires -- nothing
    // else to clean up here, just record it.
    ctx->error = true;
    ctx->closed = true;
}

static err_t update_connected_cb(void *arg, struct altcp_pcb *pcb, err_t err)
{
    sidetnfs_update_ctx_t *ctx = (sidetnfs_update_ctx_t *)arg;
    if (err != ERR_OK)
    {
        ctx->error = true;
        ctx->closed = true;
        return ERR_OK;
    }

    char req[192];
    int req_len = snprintf(req, sizeof(req),
                            "GET %s HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "User-Agent: SideTNFS-Firmware\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            SIDETNFS_UPDATE_CHECK_PATH, SIDETNFS_UPDATE_CHECK_HOST);
    if (req_len <= 0 || (size_t)req_len >= sizeof(req))
    {
        ctx->error = true;
        ctx->closed = true;
        return ERR_OK;
    }

    err_t write_err = altcp_write(pcb, req, (u16_t)req_len, TCP_WRITE_FLAG_COPY);
    if (write_err != ERR_OK)
    {
        ctx->error = true;
        ctx->closed = true;
        return ERR_OK;
    }
    altcp_output(pcb);
    ctx->request_sent = true;
    return ERR_OK;
}

// Finds the blank line separating HTTP headers from the body, and
// returns a pointer to the body's first byte (NUL-terminated, since
// ctx->resp is always NUL-padded within its bounds -- see the caller).
// NULL if the headers were never fully received within the response
// budget.
static const char *update_find_body(const char *resp, uint16_t resp_len)
{
    for (uint16_t i = 0; i + 3 < resp_len; i++)
    {
        if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n')
        {
            return resp + i + 4;
        }
    }
    return NULL;
}

// Parses a "vMAJOR.MINOR.PATCH" (or "MAJOR.MINOR.PATCH") string into
// three integers for a numeric compare -- a plain strcmp() would sort
// "v1.0.9" after "v1.0.10", which is wrong. Missing components default
// to 0. Returns false if `s` has no leading digit anywhere (empty/
// garbage input).
static bool update_parse_version(const char *s, long *out_major, long *out_minor, long *out_patch)
{
    if (s == NULL)
    {
        return false;
    }
    if (*s == 'v' || *s == 'V')
    {
        s++;
    }
    char *end = NULL;
    *out_major = strtol(s, &end, 10);
    if (end == s)
    {
        return false; // no digits at all -- not a version string
    }
    *out_minor = 0;
    *out_patch = 0;
    if (*end == '.')
    {
        s = end + 1;
        *out_minor = strtol(s, &end, 10);
    }
    if (*end == '.')
    {
        s = end + 1;
        *out_patch = strtol(s, &end, 10);
    }
    return true;
}

// True if `remote` is strictly newer than `local`.
static bool update_is_newer(const char *remote, const char *local)
{
    long r_major, r_minor, r_patch;
    long l_major, l_minor, l_patch;
    if (!update_parse_version(remote, &r_major, &r_minor, &r_patch) ||
        !update_parse_version(local, &l_major, &l_minor, &l_patch))
    {
        return false; // can't parse -- never claim an update over unparseable input
    }
    if (r_major != l_major)
    {
        return r_major > l_major;
    }
    if (r_minor != l_minor)
    {
        return r_minor > l_minor;
    }
    return r_patch > l_patch;
}

uint32_t sidetnfs_update_check_run(char *out_latest_version, size_t out_size)
{
    if (out_latest_version != NULL && out_size > 0)
    {
        out_latest_version[0] = '\0';
    }

    sidetnfs_update_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    absolute_time_t deadline = make_timeout_time_ms(SIDETNFS_UPDATE_CHECK_TIMEOUT_MS);

    // --- DNS resolve ---
    ip_addr_t resolved;
    err_t dns_err = dns_gethostbyname(SIDETNFS_UPDATE_CHECK_HOST, &resolved, update_dns_found_cb, &ctx);
    if (dns_err == ERR_OK)
    {
        // Already resolved (lwIP DNS cache) -- the callback is never
        // invoked in this case, so adopt its result directly, same
        // convention the boot-time NTP resolve already uses.
        ctx.dns_addr = resolved;
        ctx.dns_ok = true;
        ctx.dns_done = true;
    }
    else if (dns_err != ERR_INPROGRESS)
    {
        DPRINTF("Update check: DNS lookup failed synchronously (%d)\n", (int)dns_err);
        return SIDETNFS_UPDATE_STATUS_ERROR;
    }
    while (!ctx.dns_done && !time_reached(deadline))
    {
        network_safe_poll();
        sleep_us(SIDETNFS_UPDATE_CHECK_POLL_STEP_US);
    }
    if (!ctx.dns_ok)
    {
        DPRINTF("Update check: DNS lookup did not resolve\n");
        return SIDETNFS_UPDATE_STATUS_ERROR;
    }

    // --- TLS connect ---
    struct altcp_tls_config *tls_config = altcp_tls_create_config_client(NULL, 0);
    if (tls_config == NULL)
    {
        DPRINTF("Update check: could not create TLS client config\n");
        return SIDETNFS_UPDATE_STATUS_ERROR;
    }
    struct altcp_pcb *pcb = altcp_tls_new(tls_config, IPADDR_TYPE_V4);
    if (pcb == NULL)
    {
        DPRINTF("Update check: could not allocate TLS pcb\n");
        altcp_tls_free_config(tls_config);
        return SIDETNFS_UPDATE_STATUS_ERROR;
    }
    altcp_arg(pcb, &ctx);
    altcp_recv(pcb, update_recv_cb);
    altcp_err(pcb, update_err_cb);

    if (altcp_connect(pcb, &ctx.dns_addr, SIDETNFS_UPDATE_CHECK_PORT, update_connected_cb) != ERR_OK)
    {
        DPRINTF("Update check: altcp_connect() failed\n");
        altcp_close(pcb);
        altcp_tls_free_config(tls_config);
        return SIDETNFS_UPDATE_STATUS_ERROR;
    }

    while (!ctx.closed && !time_reached(deadline))
    {
        network_safe_poll();
        sleep_us(SIDETNFS_UPDATE_CHECK_POLL_STEP_US);
    }
    if (!ctx.closed)
    {
        // Timed out mid-flight -- close what we opened, best-effort,
        // and report the check as failed (never partially, silently
        // "successful" on a timeout).
        DPRINTF("Update check: timed out waiting for a response\n");
        altcp_close(pcb);
        altcp_tls_free_config(tls_config);
        return SIDETNFS_UPDATE_STATUS_ERROR;
    }
    altcp_close(pcb);
    altcp_tls_free_config(tls_config);

    if (ctx.error || !ctx.request_sent || ctx.resp_len == 0)
    {
        DPRINTF("Update check: connection closed with no usable response\n");
        return SIDETNFS_UPDATE_STATUS_ERROR;
    }

    // ctx.resp is a fixed static-sized buffer, always at least one byte
    // shorter than sizeof(ctx.resp) was ever written to (see
    // update_recv_cb's clamp) -- NUL-terminate right after the last byte
    // actually received.
    ctx.resp[ctx.resp_len] = '\0';

    const char *body = update_find_body(ctx.resp, ctx.resp_len);
    if (body == NULL)
    {
        DPRINTF("Update check: response headers never completed\n");
        return SIDETNFS_UPDATE_STATUS_ERROR;
    }

    // version.txt is a single line ("v1.0.2\n") -- trim any trailing
    // whitespace/newline, bounded to the same small buffer.
    char version[SIDETNFS_UPDATE_VERSION_LEN];
    size_t i = 0;
    while (i < sizeof(version) - 1 && body[i] != '\0' && body[i] != '\r' && body[i] != '\n' && body[i] != ' ')
    {
        version[i] = body[i];
        i++;
    }
    version[i] = '\0';

    if (i == 0)
    {
        DPRINTF("Update check: empty version string in response\n");
        return SIDETNFS_UPDATE_STATUS_ERROR;
    }

    if (out_latest_version != NULL && out_size > 0)
    {
        strncpy(out_latest_version, version, out_size - 1);
        out_latest_version[out_size - 1] = '\0';
    }

    DPRINTF("Update check: remote=%s local=%s\n", version, RELEASE_VERSION);

    if (update_is_newer(version, RELEASE_VERSION))
    {
        return SIDETNFS_UPDATE_STATUS_AVAILABLE;
    }
    return SIDETNFS_UPDATE_STATUS_UP_TO_DATE;
}

/**
 * File: test_fase9e_dns_err_ok.c
 * Fase 9E: the dns_gethostbyname() ERR_OK bugfix in gemdrvemul.c's boot
 * NTP loop.
 *
 * gemdrvemul.c is not host-compilable as a whole (lwIP/cyw43/PIO/DMA/
 * shared-memory dependencies throughout) -- the same documented limitation
 * test_fase5_net_err_root.c/test_fase6_sd_service.c/test_fase7_sd_backend.c
 * already carry. This file is therefore a faithful MIRROR of just the
 * resolve/send decision logic of that loop, transcribed statement for
 * statement from the real code, with lwIP's dns_gethostbyname() and the
 * set_internal_rtc() UDP send replaced by scriptable stand-ins so each
 * DNS outcome can be driven deterministically and the number of UDP
 * requests actually counted.
 *
 * The bug being fixed: lwIP invokes the DNS callback ONLY for the
 * ERR_INPROGRESS case. On ERR_OK (entry already in the lwIP DNS cache, or
 * the configured "hostname" is a numeric IP literal) it fills in the
 * address and returns immediately WITHOUT ever calling the callback, so
 * ntp_server_found stayed false, set_internal_rtc() was never reached, no
 * UDP request went out, and the attempt always ran into the timeout.
 *
 * Run:
 *   gcc -std=gnu11 -Wall -Wextra -o /tmp/test_fase9e_dns_err_ok \
 *       test_fase9e_dns_err_ok.c && /tmp/test_fase9e_dns_err_ok
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                             \
    do                                                               \
    {                                                                \
        g_checks++;                                                  \
        if (!(cond))                                                 \
        {                                                            \
            g_failures++;                                            \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

/* ---- verbatim lwIP error codes (lwip/err.h) ---- */
typedef int err_t;
#define ERR_OK 0
#define ERR_ARG (-16)
#define ERR_VAL (-6)
#define ERR_INPROGRESS (-5)

/* ---- mirror of rtcemul.c's NTP_TIME ---- */
typedef struct
{
    unsigned long ntp_ipaddr;
    bool ntp_server_found;
    bool ntp_error;
} NTP_TIME;

static NTP_TIME g_net_time;
static NTP_TIME *get_net_time(void) { return &g_net_time; }

/* ---- scriptable DNS stand-in ---- */
typedef enum
{
    DNS_IMMEDIATE_OK,     /* cached entry or numeric IP literal -> ERR_OK, no callback */
    DNS_ASYNC_SUCCESS,    /* ERR_INPROGRESS, callback later with an address */
    DNS_ASYNC_FAILURE,    /* ERR_INPROGRESS, callback later with NULL (bad hostname) */
    DNS_HARD_ERROR,       /* ERR_ARG/ERR_VAL, no callback ever */
} dns_behaviour_t;

static dns_behaviour_t g_dns_behaviour;
static int g_dns_calls;
static bool g_callback_pending;   /* an async callback is still owed to us */
static bool g_stray_callback_after_send; /* test 5: fire a late duplicate callback */

/* mirror of rtcemul.c's host_found_callback(), including its
 * "!ntime->ntp_server_found" guard */
static void host_found_callback(const char *name, const unsigned long *ipaddr, NTP_TIME *ntime)
{
    (void)name;
    if (ipaddr != NULL && !ntime->ntp_server_found)
    {
        ntime->ntp_server_found = true;
        ntime->ntp_ipaddr = *ipaddr;
    }
    else if (ipaddr == NULL)
    {
        ntime->ntp_error = true;
    }
}

static err_t dns_gethostbyname_stub(const char *host, unsigned long *addr)
{
    (void)host;
    g_dns_calls++;
    switch (g_dns_behaviour)
    {
    case DNS_IMMEDIATE_OK:
        *addr = 0xC0A80001UL; /* lwIP fills the address in itself */
        return ERR_OK;        /* and does NOT call the callback */
    case DNS_ASYNC_SUCCESS:
    case DNS_ASYNC_FAILURE:
        g_callback_pending = true;
        return ERR_INPROGRESS;
    case DNS_HARD_ERROR:
    default:
        return ERR_ARG;
    }
}

/* deliver whatever async callback the stub owes us, once */
static void pump_async_callback(void)
{
    if (!g_callback_pending)
    {
        return;
    }
    g_callback_pending = false;
    if (g_dns_behaviour == DNS_ASYNC_SUCCESS)
    {
        unsigned long ip = 0x0A000001UL;
        host_found_callback("host", &ip, get_net_time());
    }
    else
    {
        host_found_callback("host", NULL, get_net_time());
    }
}

/* ---- mirror of the boot loop, transcribed from gemdrvemul.c ---- */
typedef struct
{
    int udp_requests_sent;
    int dns_queries;
    bool rtc_set;
} loop_result_t;

#define MAX_ITERATIONS 40 /* stands in for the ~3s bounded deadline */

static loop_result_t run_boot_ntp_loop(dns_behaviour_t behaviour, bool stray_callback)
{
    memset(&g_net_time, 0, sizeof(g_net_time));
    g_dns_behaviour = behaviour;
    g_dns_calls = 0;
    g_callback_pending = false;
    g_stray_callback_after_send = stray_callback;

    loop_result_t r = {0};

    bool dns_query_done = false;
    bool ntp_request_sent = false;

    for (int i = 0; i < MAX_ITERATIONS && !r.rtc_set; i++)
    {
        pump_async_callback(); /* stands in for network_safe_poll() delivering callbacks */

        if (g_net_time.ntp_server_found && dns_query_done && !ntp_request_sent)
        {
            g_net_time.ntp_server_found = false;
            ntp_request_sent = true;
            r.udp_requests_sent++; /* set_internal_rtc() */
            r.rtc_set = true;      /* assume the server answers */

            if (g_stray_callback_after_send)
            {
                /* a late/duplicate callback arriving after the send must not
                 * be able to trigger a second request */
                unsigned long ip = 0x0A000001UL;
                host_found_callback("host", &ip, get_net_time());
            }
            continue;
        }

        if (!dns_query_done)
        {
            g_net_time.ntp_server_found = false;
            g_net_time.ntp_error = false;
            ntp_request_sent = false;

            err_t dns_ret = dns_gethostbyname_stub("host", &g_net_time.ntp_ipaddr);
            dns_query_done = true;

            if (dns_ret == ERR_OK)
            {
                g_net_time.ntp_server_found = true;
            }
        }

        if (g_net_time.ntp_error)
        {
            dns_query_done = false;
            g_net_time.ntp_error = false;
            g_net_time.ntp_server_found = false;
        }
    }

    r.dns_queries = g_dns_calls;
    return r;
}

int main(void)
{
    printf("Fase 9E -- dns_gethostbyname() ERR_OK bugfix\n");
    printf("=============================================\n");

    printf("Test 1: numeric IP literal (ERR_OK) -> request is sent\n");
    loop_result_t t1 = run_boot_ntp_loop(DNS_IMMEDIATE_OK, false);
    CHECK(t1.udp_requests_sent == 1, "1: exactly one UDP request sent");
    CHECK(t1.rtc_set, "1: attempt reaches the NTP send instead of timing out");

    printf("Test 2: cached hostname (ERR_OK) -> request is sent\n");
    loop_result_t t2 = run_boot_ntp_loop(DNS_IMMEDIATE_OK, false);
    CHECK(t2.udp_requests_sent == 1, "2: exactly one UDP request sent");
    CHECK(t2.dns_queries == 1, "2: only a single DNS query needed");

    printf("Test 3: async DNS (ERR_INPROGRESS) -> callback sends request\n");
    loop_result_t t3 = run_boot_ntp_loop(DNS_ASYNC_SUCCESS, false);
    CHECK(t3.udp_requests_sent == 1, "3: exactly one UDP request sent via the callback path");
    CHECK(t3.rtc_set, "3: async path still works unchanged");

    printf("Test 4: invalid hostname (callback with NULL) -> no request\n");
    loop_result_t t4 = run_boot_ntp_loop(DNS_ASYNC_FAILURE, false);
    CHECK(t4.udp_requests_sent == 0, "4: no UDP request is ever sent");
    CHECK(!t4.rtc_set, "4: attempt ends without setting the RTC");

    printf("Test 5: at most one UDP request per attempt (stray late callback)\n");
    loop_result_t t5 = run_boot_ntp_loop(DNS_IMMEDIATE_OK, true);
    CHECK(t5.udp_requests_sent == 1, "5: a duplicate callback cannot trigger a second request");

    printf("Test 6: hard synchronous DNS error (ERR_ARG) -> no request, no query storm\n");
    loop_result_t t6 = run_boot_ntp_loop(DNS_HARD_ERROR, false);
    CHECK(t6.udp_requests_sent == 0, "6: no UDP request is sent");
    CHECK(t6.dns_queries == 1, "6: the failure is not retried in a tight loop");

    printf("=============================================\n");
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

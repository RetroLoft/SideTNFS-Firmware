/**
 * File: test_tnfs_dns_resolve.c
 * TNFS server address resolution: IPv4 literals, DNS names, failures,
 * timeouts, retry and per-slot isolation.
 *
 * Compiles the real sidetnfs_resolve.c. lwIP itself is not host-buildable,
 * so dns_gethostbyname() is modelled by a small stand-in that reproduces
 * its three outcomes -- ERR_OK with the address filled in and no callback,
 * ERR_INPROGRESS followed by a callback, and an immediate error. The state
 * machine under test is the production one.
 *
 * Run:
 *   gcc -std=gnu11 -Wall -Wextra -Isandbox -Isandbox/include \
 *       test_tnfs_dns_resolve.c sandbox/sidetnfs_resolve.c \
 *       -o /tmp/test_tnfs_dns_resolve && /tmp/test_tnfs_dns_resolve
 */
#include <stdio.h>
#include <string.h>
#include "include/sidetnfs_resolve.h"

static int g_failures = 0, g_checks = 0;
#define CHECK(c, m) do { g_checks++; if(!(c)){ g_failures++; \
    printf("  FAIL: %s (%s:%d)\n", m, __FILE__, __LINE__);} } while(0)

/* ---- lwIP dns_gethostbyname() stand-in ---------------------------- */
#define STUB_ERR_OK          0
#define STUB_ERR_INPROGRESS -5
#define STUB_ERR_VAL        -6

typedef enum { DNS_CACHE_HIT, DNS_ASYNC_OK, DNS_ASYNC_FAIL, DNS_ASYNC_SILENT, DNS_IMMEDIATE_ERR } dns_mode_t;

static dns_mode_t g_mode;
static uint32_t   g_answer;
static int        g_query_count;
static uint32_t   g_next_id = 1;

/* Outstanding lookups lwIP still owes us an answer for. Mirrors production:
   the callback carries only the request id, never a pointer. */
#define MAX_INFLIGHT 8
static struct { uint32_t id; bool live; dns_mode_t mode; uint32_t answer; } g_inflight[MAX_INFLIGHT];

/* the slots a callback may resolve, mirroring the production scan */
static sidetnfs_resolve_t *g_slots[4];
static int g_slot_count;

static void deliver_id(uint32_t id, dns_mode_t mode, uint32_t answer)
{
    for (int i = 0; i < g_slot_count; i++) {
        if (!sidetnfs_resolve_accepts(g_slots[i], id)) continue;
        if (mode == DNS_ASYNC_OK) sidetnfs_resolve_complete(g_slots[i], id, &answer);
        else                      sidetnfs_resolve_complete(g_slots[i], id, NULL);
        return;
    }
}

/* mirrors ensure_slot_address()'s use of the lwIP entry point */
static int stub_dns_gethostbyname(const char *host, uint32_t *out, uint32_t id)
{
    (void)host;
    g_query_count++;
    switch (g_mode) {
    case DNS_CACHE_HIT:     *out = g_answer; return STUB_ERR_OK;
    case DNS_IMMEDIATE_ERR: return STUB_ERR_VAL;
    default:
        for (int i = 0; i < MAX_INFLIGHT; i++)
            if (!g_inflight[i].live) {
                g_inflight[i] = (typeof(g_inflight[0])){ id, true, g_mode, g_answer };
                break;
            }
        return STUB_ERR_INPROGRESS;
    }
}
/* deliver the oldest outstanding answer */
static void stub_deliver_callback(void)
{
    for (int i = 0; i < MAX_INFLIGHT; i++)
        if (g_inflight[i].live) {
            g_inflight[i].live = false;
            if (g_inflight[i].mode != DNS_ASYNC_SILENT)
                deliver_id(g_inflight[i].id, g_inflight[i].mode, g_inflight[i].answer);
            return;
        }
}
/* deliver a specific, possibly long-superseded, lookup */
static void stub_deliver_id(uint32_t id, dns_mode_t mode, uint32_t answer)
{ deliver_id(id, mode, answer); }

/* faithful transcription of ensure_slot_address()'s decision flow */
static uint32_t g_last_request_id;   /* id issued by the most recent lookup */

static bool resolve_slot(sidetnfs_resolve_t *r, int slot, const char *host, int deliver_after_ticks)
{
    if (r->state == SIDETNFS_RESOLVE_DONE) return true;

    uint32_t literal = 0;
    if (sidetnfs_resolve_parse_ipv4(host, &literal)) {
        uint32_t id = g_next_id++;
        sidetnfs_resolve_reset(r, slot);
        sidetnfs_resolve_begin(r, id);
        sidetnfs_resolve_complete(r, id, &literal);
        return true;
    }
    if (host[0] == '\0') return false;

    uint32_t id = g_next_id++;
    g_last_request_id = id;
    sidetnfs_resolve_reset(r, slot);
    sidetnfs_resolve_begin(r, id);

    uint32_t got = 0;
    int err = stub_dns_gethostbyname(host, &got, id);
    if (err == STUB_ERR_OK)            sidetnfs_resolve_complete(r, id, &got);
    else if (err != STUB_ERR_INPROGRESS) sidetnfs_resolve_complete(r, id, NULL);

    int ticks = 0;
    while (r->state == SIDETNFS_RESOLVE_PENDING) {
        if (deliver_after_ticks >= 0 && ticks == deliver_after_ticks) stub_deliver_callback();
        ticks++;
        sidetnfs_resolve_tick(r, SIDETNFS_RESOLVE_STEP_MS, SIDETNFS_RESOLVE_TIMEOUT_MS);
    }
    return r->state == SIDETNFS_RESOLVE_DONE;
}

static void reset_stub(dns_mode_t m, uint32_t answer)
{ g_mode = m; g_answer = answer; g_query_count = 0;
  for (int i = 0; i < MAX_INFLIGHT; i++) g_inflight[i].live = false; }
static void register_slots(sidetnfs_resolve_t *a, sidetnfs_resolve_t *b)
{ g_slot_count = 0; if (a) g_slots[g_slot_count++] = a; if (b) g_slots[g_slot_count++] = b; }

/* 10.0.0.5 in ip_addr_t.addr order (first octet in the low byte) */
#define IP_10_0_0_5 (10u | (0u<<8) | (0u<<16) | (5u<<24))

int main(void)
{
    printf("TNFS DNS resolution\n===================\n");

    printf("Test 1: numeric IPv4 -- resolved without any DNS query\n");
    { sidetnfs_resolve_t r; sidetnfs_resolve_reset(&r, 0); register_slots(&r, NULL); reset_stub(DNS_ASYNC_OK, 0);
      CHECK(resolve_slot(&r, 0, "10.0.0.5", 0), "1: literal resolves");
      CHECK(r.state == SIDETNFS_RESOLVE_DONE, "1: state DONE");
      CHECK(r.addr == IP_10_0_0_5, "1: correct address");
      CHECK(g_query_count == 0, "1: no DNS query issued for a literal"); }

    printf("Test 2: hostname via cache hit (ERR_OK, no callback)\n");
    { sidetnfs_resolve_t r; sidetnfs_resolve_reset(&r, 0); register_slots(&r, NULL); reset_stub(DNS_CACHE_HIT, IP_10_0_0_5);
      CHECK(resolve_slot(&r, 0, "tnfs.local", -1), "2: cache hit resolves");
      CHECK(r.addr == IP_10_0_0_5, "2: address from the synchronous answer");
      CHECK(g_query_count == 1, "2: exactly one query"); }

    printf("Test 3: hostname via async callback (ERR_INPROGRESS)\n");
    { sidetnfs_resolve_t r; sidetnfs_resolve_reset(&r, 0); register_slots(&r, NULL); reset_stub(DNS_ASYNC_OK, IP_10_0_0_5);
      CHECK(resolve_slot(&r, 0, "tnfs.example.org", 3), "3: async answer resolves");
      CHECK(r.state == SIDETNFS_RESOLVE_DONE, "3: state DONE");
      CHECK(r.addr == IP_10_0_0_5, "3: address from the callback"); }

    printf("Test 4: non-existent hostname -- clean failure\n");
    { sidetnfs_resolve_t r; sidetnfs_resolve_reset(&r, 0); register_slots(&r, NULL); reset_stub(DNS_ASYNC_FAIL, 0);
      CHECK(!resolve_slot(&r, 0, "nope.invalid", 2), "4: reports failure");
      CHECK(r.state == SIDETNFS_RESOLVE_FAILED, "4: state FAILED, never stuck PENDING"); }

    printf("Test 5: DNS timeout -- bounded, never blocks forever\n");
    { sidetnfs_resolve_t r; sidetnfs_resolve_reset(&r, 0); register_slots(&r, NULL); reset_stub(DNS_ASYNC_SILENT, 0);
      CHECK(!resolve_slot(&r, 0, "blackhole.invalid", -1), "5: reports failure");
      CHECK(r.state == SIDETNFS_RESOLVE_FAILED, "5: state FAILED");
      CHECK(r.elapsed_ms >= SIDETNFS_RESOLVE_TIMEOUT_MS, "5: waited the full budget");
      CHECK(r.elapsed_ms <= SIDETNFS_RESOLVE_TIMEOUT_MS + SIDETNFS_RESOLVE_STEP_MS, "5: and no longer"); }

    printf("Test 6: late callback after timeout cannot revive the lookup\n");
    { sidetnfs_resolve_t r; sidetnfs_resolve_reset(&r, 0); register_slots(&r, NULL); reset_stub(DNS_ASYNC_SILENT, IP_10_0_0_5);
      resolve_slot(&r, 0, "slow.invalid", -1);
      uint32_t stale = g_last_request_id;
      CHECK(r.state == SIDETNFS_RESOLVE_FAILED, "6: timed out first");
      stub_deliver_id(stale, DNS_ASYNC_OK, IP_10_0_0_5);
      CHECK(r.state == SIDETNFS_RESOLVE_FAILED, "6: stale answer ignored");
      CHECK(r.addr == 0, "6: address not overwritten"); }

    printf("Test 7: retry after a DNS failure succeeds\n");
    { sidetnfs_resolve_t r; sidetnfs_resolve_reset(&r, 0); register_slots(&r, NULL);
      reset_stub(DNS_ASYNC_FAIL, 0);
      CHECK(!resolve_slot(&r, 0, "tnfs.example.org", 1), "7: first attempt fails");
      reset_stub(DNS_ASYNC_OK, IP_10_0_0_5);
      CHECK(resolve_slot(&r, 0, "tnfs.example.org", 1), "7: retry succeeds");
      CHECK(r.state == SIDETNFS_RESOLVE_DONE, "7: failure was not sticky"); }

    printf("Test 8: two slots -- one hostname, one literal, no crosstalk\n");
    { sidetnfs_resolve_t a, b;
      sidetnfs_resolve_reset(&a, 0); sidetnfs_resolve_reset(&b, 1); register_slots(&a, &b);
      reset_stub(DNS_ASYNC_OK, IP_10_0_0_5);
      CHECK(resolve_slot(&a, 0, "tnfs.example.org", 1), "8: slot 0 hostname resolves");
      CHECK(resolve_slot(&b, 1, "192.168.1.9", -1), "8: slot 1 literal resolves");
      CHECK(a.addr == IP_10_0_0_5, "8: slot 0 kept its own address");
      CHECK(b.addr == (192u|(168u<<8)|(1u<<16)|(9u<<24)), "8: slot 1 kept its own address");
      CHECK(a.slot == 0 && b.slot == 1, "8: each state records its own slot"); }

    printf("Test 9: two slots with different hostnames resolve independently\n");
    { sidetnfs_resolve_t a, b;
      sidetnfs_resolve_reset(&a, 0); sidetnfs_resolve_reset(&b, 1); register_slots(&a, &b);
      reset_stub(DNS_ASYNC_OK, IP_10_0_0_5);
      CHECK(resolve_slot(&a, 0, "one.example.org", 1), "9: slot 0 resolves");
      reset_stub(DNS_ASYNC_FAIL, 0);
      CHECK(!resolve_slot(&b, 1, "two.example.org", 1), "9: slot 1 fails");
      CHECK(a.state == SIDETNFS_RESOLVE_DONE && a.addr == IP_10_0_0_5,
            "9: slot 1's failure did not disturb slot 0"); }

    printf("Test 10: literal parser rejects non-literals\n");
    { uint32_t a;
      CHECK(!sidetnfs_resolve_parse_ipv4("tnfs.local", &a), "10: hostname rejected");
      CHECK(!sidetnfs_resolve_parse_ipv4("10.0.0", &a), "10: partial rejected");
      CHECK(!sidetnfs_resolve_parse_ipv4("10.0.0.256", &a), "10: out-of-range octet rejected");
      CHECK(!sidetnfs_resolve_parse_ipv4("10.0.0.5 ", &a), "10: trailing space rejected");
      CHECK(!sidetnfs_resolve_parse_ipv4("", &a), "10: empty rejected");
      CHECK(!sidetnfs_resolve_parse_ipv4(NULL, &a), "10: NULL rejected");
      CHECK(sidetnfs_resolve_parse_ipv4("255.255.255.255", &a), "10: broadcast accepted");
      CHECK(sidetnfs_resolve_parse_ipv4("0.0.0.0", &a), "10: zero accepted"); }


    printf("Test 11: late callback of A cannot complete retry B (the race)\n");
    { sidetnfs_resolve_t r; sidetnfs_resolve_reset(&r, 0); register_slots(&r, NULL);
      /* A: times out with no answer */
      reset_stub(DNS_ASYNC_SILENT, 0);
      CHECK(!resolve_slot(&r, 0, "server.example.org", -1), "11: A times out");
      uint32_t id_a = g_last_request_id;
      /* B: retry for the same slot, still waiting */
      reset_stub(DNS_ASYNC_SILENT, 0);
      uint32_t id_b = g_next_id++;
      sidetnfs_resolve_reset(&r, 0);
      sidetnfs_resolve_begin(&r, id_b);
      CHECK(r.state == SIDETNFS_RESOLVE_PENDING, "11: B is pending");
      /* A's answer finally shows up while B is in flight */
      stub_deliver_id(id_a, DNS_ASYNC_OK, IP_10_0_0_5);
      CHECK(r.state == SIDETNFS_RESOLVE_PENDING, "11: B still pending -- A's answer rejected");
      CHECK(r.addr == 0, "11: B did not take A's address");
      CHECK(id_a != id_b, "11: the two lookups carry different ids");
      /* Second line of defence: even if an answer reaches complete()
         directly, bypassing the id scan in the callback, the id mismatch
         must still reject it. This is the check the old state-only guard
         failed. */
      uint32_t bogus = IP_10_0_0_5;
      sidetnfs_resolve_complete(&r, id_a, &bogus);
      CHECK(r.state == SIDETNFS_RESOLVE_PENDING, "11: complete() rejects A's id while B pends");
      CHECK(r.addr == 0, "11: complete() left B's address untouched");
      /* B's own answer is still accepted */
      stub_deliver_id(id_b, DNS_ASYNC_OK, IP_10_0_0_5);
      CHECK(r.state == SIDETNFS_RESOLVE_DONE, "11: B completes on its own answer"); }

    printf("Test 12: callback B, then a still-later callback A\n");
    { sidetnfs_resolve_t r; sidetnfs_resolve_reset(&r, 0); register_slots(&r, NULL);
      reset_stub(DNS_ASYNC_SILENT, 0);
      resolve_slot(&r, 0, "a.example.org", -1);
      uint32_t id_a = g_last_request_id;
      uint32_t id_b = g_next_id++;
      sidetnfs_resolve_reset(&r, 0); sidetnfs_resolve_begin(&r, id_b);
      stub_deliver_id(id_b, DNS_ASYNC_OK, IP_10_0_0_5);
      CHECK(r.state == SIDETNFS_RESOLVE_DONE && r.addr == IP_10_0_0_5, "12: B applied");
      stub_deliver_id(id_a, DNS_ASYNC_OK, 0xDEADBEEFu);
      CHECK(r.addr == IP_10_0_0_5, "12: later A answer does not overwrite B"); }

    printf("Test 13: hostname changed between A and B\n");
    { sidetnfs_resolve_t r; sidetnfs_resolve_reset(&r, 0); register_slots(&r, NULL);
      reset_stub(DNS_ASYNC_SILENT, 0);
      resolve_slot(&r, 0, "old.example.org", -1);          /* A: times out */
      uint32_t id_a = g_last_request_id;
      reset_stub(DNS_ASYNC_OK, IP_10_0_0_5);
      CHECK(resolve_slot(&r, 0, "new.example.org", 1), "13: B resolves the new host");
      uint32_t new_addr = r.addr;
      stub_deliver_id(id_a, DNS_ASYNC_OK, 0x0A0A0A0Au);    /* old host answers late */
      CHECK(r.addr == new_addr, "13: stale answer for the old hostname is ignored"); }

    printf("Test 14: two slots in flight, answers land on the right slot\n");
    { sidetnfs_resolve_t a, b;
      sidetnfs_resolve_reset(&a, 0); sidetnfs_resolve_reset(&b, 1); register_slots(&a, &b);
      reset_stub(DNS_ASYNC_SILENT, 0);
      uint32_t id_a = g_next_id++, id_b = g_next_id++;
      sidetnfs_resolve_begin(&a, id_a);
      sidetnfs_resolve_begin(&b, id_b);
      stub_deliver_id(id_b, DNS_ASYNC_OK, IP_10_0_0_5);
      CHECK(b.state == SIDETNFS_RESOLVE_DONE && b.addr == IP_10_0_0_5, "14: slot 1 got its answer");
      CHECK(a.state == SIDETNFS_RESOLVE_PENDING, "14: slot 0 untouched");
      stub_deliver_id(id_a, DNS_ASYNC_OK, 0x0100007Fu);
      CHECK(a.addr == 0x0100007Fu, "14: slot 0 got its own answer");
      CHECK(b.addr == IP_10_0_0_5, "14: slot 1 unchanged"); }

    printf("Test 15: an unknown id matches no slot at all\n");
    { sidetnfs_resolve_t a, b;
      sidetnfs_resolve_reset(&a, 0); sidetnfs_resolve_reset(&b, 1); register_slots(&a, &b);
      uint32_t id_a = g_next_id++, id_b = g_next_id++;
      sidetnfs_resolve_begin(&a, id_a); sidetnfs_resolve_begin(&b, id_b);
      stub_deliver_id(999999u, DNS_ASYNC_OK, IP_10_0_0_5);
      CHECK(a.state == SIDETNFS_RESOLVE_PENDING && b.state == SIDETNFS_RESOLVE_PENDING,
            "15: neither slot accepts a foreign id"); }

    printf("Test 16: empty/unconfigured host costs no query and no wait\n");
    { sidetnfs_resolve_t r; sidetnfs_resolve_reset(&r, 0); register_slots(&r, NULL);
      reset_stub(DNS_ASYNC_SILENT, 0);
      CHECK(!resolve_slot(&r, 0, "", -1), "16: empty host reports failure");
      CHECK(g_query_count == 0, "16: no DNS query issued");
      CHECK(r.elapsed_ms == 0, "16: no time spent waiting"); }

    printf("===================\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

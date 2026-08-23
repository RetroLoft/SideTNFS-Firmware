/**
 * File: test_tnfs_seq_wrap.c
 * SideTNFS v1.0.3 reliability fix: regression/stress test for the
 * fs-listing (TNFS OPEN/READ/WRITE/CLOSE/...) response-correlation bug
 * fixed in romemul/sidetnfs_probe.c (fslisting_wait_for() /
 * tnfs_fslisting_recv_callback()).
 *
 * Bug: every fslisting_send_*() shared one 8-bit s_readdirx_seq counter,
 * and fslisting_wait_for() matched an incoming response purely on
 * (cmd, seq), with source address/port and the TNFS session id ignored.
 * A response for a request that had already timed out (client gave up,
 * but the response was only late, not lost) could survive to be
 * misattributed to a LATER, completely unrelated request that reused
 * the same (cmd, seq) pair after the 256-value sequence space wrapped
 * around -- silently splicing stale bytes into whatever was being read
 * at that point. Large .PRG files loaded over TNFS need 600-800+ READ
 * round trips (200-byte chunks), wrapping the sequence space 2-3+ times,
 * which is what let this surface as "almost always" hitting an illegal
 * instruction when the Atari later executed into the corrupted region
 * of a loaded program.
 *
 * romemul/sidetnfs_probe.c itself is NOT host-compilable (lwIP/cyw43/
 * Pico SDK throughout, same reason tests/host_netconfig and
 * tests/host_configdrive don't link the real TNFS-facing .c files
 * either -- see those tests' own header comments). This test instead
 * validates the response-correlation ALGORITHM directly: OLD_* functions
 * below are a faithful, byte-for-byte translation of the pre-fix logic
 * (cmd+seq only, no drain-on-timeout); NEW_* functions are the same
 * translation of the actual fix now in fslisting_wait_for()/
 * tnfs_fslisting_recv_callback() (addr+port+session-id validation, plus
 * the post-timeout drain that closes the wraparound gap). A small mock
 * "network" (a scheduled-delivery packet queue, see MockPacket/
 * mock_poll() below) lets the test place responses -- including a
 * deliberately late straggler -- at an exact simulated point in time,
 * which is what actually reproducing this over a real UDP socket would
 * require minutes of flaky, timing-dependent hardware testing to hit
 * reliably.
 *
 * Run:
 *   gcc -std=c11 -Wall -Wextra test_tnfs_seq_wrap.c \
 *       -o /tmp/test_tnfs_seq_wrap && /tmp/test_tnfs_seq_wrap
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- Mock network: a scheduled-delivery packet queue --------------- */

typedef struct
{
    uint32_t addr;
    uint16_t port;
    uint16_t sid;
    uint8_t cmd;
    uint8_t seq;
    uint8_t payload; /* stand-in for the actual data bytes a real READ response carries */
    int arrival_tick;
    bool delivered;
    bool in_use;
} MockPacket;

#define MOCK_MAX_PACKETS 16
static MockPacket s_queue[MOCK_MAX_PACKETS];
static int s_tick = 0;

static void mock_reset(void)
{
    memset(s_queue, 0, sizeof(s_queue));
    s_tick = 0;
}

static void mock_schedule(uint32_t addr, uint16_t port, uint16_t sid, uint8_t cmd, uint8_t seq, uint8_t payload,
                           int arrival_tick)
{
    for (int i = 0; i < MOCK_MAX_PACKETS; i++)
    {
        if (!s_queue[i].in_use)
        {
            s_queue[i] = (MockPacket){.addr = addr,
                                       .port = port,
                                       .sid = sid,
                                       .cmd = cmd,
                                       .seq = seq,
                                       .payload = payload,
                                       .arrival_tick = arrival_tick,
                                       .delivered = false,
                                       .in_use = true};
            return;
        }
    }
    fprintf(stderr, "mock_schedule: queue full\n");
}

/* Advances the simulated clock by one tick and returns the next
 * not-yet-delivered packet whose arrival_tick has come due, or NULL.
 * Mirrors cyw43_arch_poll() handing exactly one datagram to the UDP
 * receive callback when one is ready. Frees the slot immediately on
 * delivery (in_use=false) -- safe, since the caller only reads the
 * returned pointer synchronously, before any later mock_schedule()/
 * mock_poll() call could reuse the slot -- so a long scenario (e.g.
 * scenario E's 600 rounds) never runs out of queue slots. */
static const MockPacket *mock_poll(void)
{
    static MockPacket delivered_copy;
    s_tick++;
    for (int i = 0; i < MOCK_MAX_PACKETS; i++)
    {
        if (s_queue[i].in_use && !s_queue[i].delivered && s_queue[i].arrival_tick <= s_tick)
        {
            s_queue[i].delivered = true;
            delivered_copy = s_queue[i];
            s_queue[i].in_use = false;
            return &delivered_copy;
        }
    }
    return NULL;
}

#define WAIT_MAX_ITER 20 /* stand-in for SIDETNFS_FS_WAIT_MAX_ITER, scaled down for a fast test */

/* ---- OLD (pre-fix) algorithm: cmd+seq only, no drain ---------------- */

static bool s_old_waiting = false;
static bool s_old_resp_ready = false;
static uint8_t s_old_resp_cmd = 0;
static uint8_t s_old_resp_seq = 0;
static uint8_t s_old_resp_payload = 0;

static void old_recv_callback(const MockPacket *pkt)
{
    if (!s_old_waiting)
    {
        return; /* nobody waiting -- discarded, matches the pre-fix guard that WAS already present */
    }
    s_old_resp_cmd = pkt->cmd;
    s_old_resp_seq = pkt->seq;
    s_old_resp_payload = pkt->payload;
    s_old_resp_ready = true;
}

static bool old_wait_for(uint8_t expect_cmd, uint8_t expect_seq, uint8_t *out_payload)
{
    s_old_waiting = true;
    for (int i = 0; i < WAIT_MAX_ITER; i++)
    {
        const MockPacket *pkt = mock_poll();
        if (pkt)
        {
            old_recv_callback(pkt);
        }
        if (s_old_resp_ready)
        {
            if (s_old_resp_cmd == expect_cmd && s_old_resp_seq == expect_seq)
            {
                s_old_waiting = false;
                if (out_payload)
                {
                    *out_payload = s_old_resp_payload;
                }
                return true;
            }
            s_old_resp_ready = false; /* stray/late response -- discard */
        }
    }
    s_old_waiting = false;
    return false; /* timeout -- NO drain in the pre-fix algorithm */
}

/* ---- NEW (fixed) algorithm: addr+port+sid validation, plus drain ---- */

static bool s_new_waiting = false;
static bool s_new_resp_ready = false;
static uint8_t s_new_resp_cmd = 0;
static uint8_t s_new_resp_seq = 0;
static uint8_t s_new_resp_payload = 0;
static uint32_t s_new_expected_addr = 0;
static uint16_t s_new_expected_port = 0;
static uint16_t s_new_expected_sid = 0;

static void new_recv_callback(const MockPacket *pkt)
{
    if (!s_new_waiting)
    {
        return;
    }
    if (pkt->addr != s_new_expected_addr || pkt->port != s_new_expected_port || pkt->sid != s_new_expected_sid)
    {
        return; /* not from the expected server/session -- never even stored as a candidate */
    }
    s_new_resp_cmd = pkt->cmd;
    s_new_resp_seq = pkt->seq;
    s_new_resp_payload = pkt->payload;
    s_new_resp_ready = true;
}

static bool new_wait_for(uint32_t addr, uint16_t port, uint16_t sid, uint8_t expect_cmd, uint8_t expect_seq,
                          uint8_t *out_payload)
{
    s_new_expected_addr = addr;
    s_new_expected_port = port;
    s_new_expected_sid = sid;

    s_new_waiting = true;
    for (int i = 0; i < WAIT_MAX_ITER; i++)
    {
        const MockPacket *pkt = mock_poll();
        if (pkt)
        {
            new_recv_callback(pkt);
        }
        if (s_new_resp_ready)
        {
            if (s_new_resp_cmd == expect_cmd && s_new_resp_seq == expect_seq)
            {
                s_new_waiting = false;
                if (out_payload)
                {
                    *out_payload = s_new_resp_payload;
                }
                return true;
            }
            s_new_resp_ready = false;
        }
    }
    /* TIMEOUT: drain one more full window, unconditionally discarding
     * anything that arrives, exactly like fslisting_wait_for()'s fix. */
    for (int i = 0; i < WAIT_MAX_ITER; i++)
    {
        const MockPacket *pkt = mock_poll();
        if (pkt)
        {
            new_recv_callback(pkt);
        }
        s_new_resp_ready = false;
    }
    s_new_waiting = false;
    return false;
}

/* ---- Test scaffolding ------------------------------------------------ */

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const char *what)
{
    g_checks++;
    if (!cond)
    {
        g_failures++;
        printf("FAIL: %s\n", what);
    }
}

#define SERVER_ADDR 0x0A00A8C0u /* stand-in for some LAN IPv4 address */
#define SERVER_PORT 16384u
#define SESSION_A 0x1234u
#define SESSION_B 0x5678u /* a different session id, e.g. after a remount */
#define CMD_READ 4u       /* arbitrary stand-in for TNFS_CMD_READ */

/* Scenario A: reproduce the pre-fix bug. Round R (seq=5) is sent, and
 * its real response is scheduled to arrive at tick 25 -- past round R's
 * own WAIT_MAX_ITER(=20)-tick primary window, so old_wait_for() times
 * out without ever seeing it (ticks 1..20). The OLD algorithm has no
 * drain step, so the response is left sitting in the mock queue,
 * undelivered. Round R+256 (seq=5 again, after the 8-bit wraparound)
 * then starts its own wait immediately afterwards (ticks 21..40) --
 * squarely inside that window, so its poll loop is the one that finally
 * delivers round R's straggler, and (cmd, seq) alone is enough for it to
 * be wrongly accepted as round R+256's own response. */
static void scenario_a_old_algorithm_is_vulnerable(void)
{
    mock_reset(); /* s_tick == 0 */

    mock_schedule(SERVER_ADDR, SERVER_PORT, SESSION_A, CMD_READ, 5, 0xAA, 25);
    bool round_r_ok = old_wait_for(CMD_READ, 5, NULL); /* consumes ticks 1..20, times out */
    check(!round_r_ok, "scenario A: round R (seq=5) times out as expected");
    check(s_tick == 20, "scenario A: primary wait consumed exactly WAIT_MAX_ITER ticks");

    /* Round R+256's own, correct response -- scheduled late enough that
     * it's only reached if the stale 0xAA one is NOT wrongly matched
     * first. */
    mock_schedule(SERVER_ADDR, SERVER_PORT, SESSION_A, CMD_READ, 5, 0xBB, 100);

    uint8_t payload = 0;
    bool round_r_plus_256_ok = old_wait_for(CMD_READ, 5, &payload); /* ticks 21..40 */

    check(round_r_plus_256_ok, "scenario A: OLD algorithm accepts the stale response as a match (the bug)");
    check(payload == 0xAA, "scenario A: OLD algorithm hands back the STALE payload, not the real one");
}

/* Scenario B: the same exact byte-for-byte scenario against the FIXED
 * algorithm. This time round R's own wait_for() call spans BOTH its
 * primary window (ticks 1..20) AND its post-timeout drain (ticks
 * 21..40) before returning -- so the straggler (still arriving at tick
 * 25) is delivered and discarded there, under round R's own identity,
 * before round R+256 ever starts waiting. */
static void scenario_b_new_algorithm_closes_the_gap(void)
{
    mock_reset(); /* s_tick == 0 */

    mock_schedule(SERVER_ADDR, SERVER_PORT, SESSION_A, CMD_READ, 5, 0xAA, 25);
    bool round_r_ok = new_wait_for(SERVER_ADDR, SERVER_PORT, SESSION_A, CMD_READ, 5, NULL); /* ticks 1..40 */
    check(!round_r_ok, "scenario B: round R (seq=5) times out as expected");
    check(s_tick == 40, "scenario B: primary wait + drain together consumed 2*WAIT_MAX_ITER ticks");

    /* Round R+256 (seq=5 again, wrapped) gets its own correct response,
     * scheduled inside ITS OWN primary window (ticks 41..60). */
    mock_schedule(SERVER_ADDR, SERVER_PORT, SESSION_A, CMD_READ, 5, 0xBB, 45);

    uint8_t payload = 0;
    bool round_r_plus_256_ok = new_wait_for(SERVER_ADDR, SERVER_PORT, SESSION_A, CMD_READ, 5, &payload);

    check(round_r_plus_256_ok, "scenario B: round R+256 still succeeds against its own, fresh response");
    check(payload == 0xBB, "scenario B: round R+256 gets the CORRECT payload, never the drained stale one");
}

/* Scenario C: even if a straggler somehow survived any drain window
 * (e.g. an even longer delay than the drain covers), a session change
 * (simulating an Atari reset + remount between round R and round
 * R+256) makes the fixed algorithm reject it on the session-id check
 * alone -- independent of, and in addition to, the drain in scenario B. */
static void scenario_c_session_id_is_independent_layer(void)
{
    mock_reset(); /* s_tick == 0 */

    /* Straggler from the OLD session (A), delivered well inside round
     * R+256's own wait window (tick 5) -- deliberately NOT drained
     * first, to isolate this layer from scenario B's drain. */
    mock_schedule(SERVER_ADDR, SERVER_PORT, SESSION_A, CMD_READ, 5, 0xAA, 5);
    /* Round R+256 now runs under a NEW session (B), e.g. after a
     * remount, and gets its own, correct, timely response. */
    mock_schedule(SERVER_ADDR, SERVER_PORT, SESSION_B, CMD_READ, 5, 0xBB, 10);

    uint8_t payload = 0;
    bool ok = new_wait_for(SERVER_ADDR, SERVER_PORT, SESSION_B, CMD_READ, 5, &payload);
    check(ok, "scenario C: round under session B succeeds");
    check(payload == 0xBB, "scenario C: session-id mismatch alone keeps the old-session straggler out");
}

/* Scenario D: a genuine duplicate delayed response (two copies of the
 * same stale packet, both landing inside round R's own drain window)
 * must not confuse or hang the drain, and the following legitimate
 * round must still succeed normally. */
static void scenario_d_duplicate_stale_response(void)
{
    mock_reset(); /* s_tick == 0 */

    mock_schedule(SERVER_ADDR, SERVER_PORT, SESSION_A, CMD_READ, 9, 0xAA, 25); /* straggler, in drain window */
    mock_schedule(SERVER_ADDR, SERVER_PORT, SESSION_A, CMD_READ, 9, 0xAA, 27); /* duplicate, also in drain window */

    bool round_r_ok = new_wait_for(SERVER_ADDR, SERVER_PORT, SESSION_A, CMD_READ, 9, NULL); /* ticks 1..40 */
    check(!round_r_ok, "scenario D: round R (seq=9) times out as expected");

    mock_schedule(SERVER_ADDR, SERVER_PORT, SESSION_A, CMD_READ, 9, 0xCC, 45); /* inside round R+256's own window */

    uint8_t payload = 0;
    bool ok = new_wait_for(SERVER_ADDR, SERVER_PORT, SESSION_A, CMD_READ, 9, &payload);
    check(ok, "scenario D: legitimate round after a duplicate straggler still succeeds");
    check(payload == 0xCC, "scenario D: payload is the fresh one, unaffected by the duplicate");
}

/* Scenario E: a large, entirely healthy transfer -- 600 sequential
 * rounds (comfortably more than 2x the 256-value wraparound, matching
 * a ~120 KB file at the real 200-byte chunk size) with every response
 * arriving in time and correctly addressed. Every round must succeed,
 * with no false failures introduced by the fix on the normal path. */
static void scenario_e_large_healthy_transfer_unaffected(void)
{
    mock_reset();
    int successes = 0;
    for (int round = 0; round < 600; round++)
    {
        uint8_t seq = (uint8_t)(2 + (round % 254)); /* mirrors s_readdirx_seq's 2..255 cycle */
        mock_schedule(SERVER_ADDR, SERVER_PORT, SESSION_A, CMD_READ, seq, (uint8_t)round, s_tick + 1);
        uint8_t payload = 0xFF;
        if (new_wait_for(SERVER_ADDR, SERVER_PORT, SESSION_A, CMD_READ, seq, &payload) &&
            payload == (uint8_t)round)
        {
            successes++;
        }
    }
    check(successes == 600, "scenario E: all 600 rounds of a healthy large transfer succeed with correct data");
}

int main(void)
{
    scenario_a_old_algorithm_is_vulnerable();
    scenario_b_new_algorithm_closes_the_gap();
    scenario_c_session_id_is_independent_layer();
    scenario_d_duplicate_stale_response();
    scenario_e_large_healthy_transfer_unaffected();

    printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}

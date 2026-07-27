/**
 * File: sidetnfs_resolve.h
 * Description: TNFS server address resolution state, kept per runtime slot.
 *
 * The decision logic here is pure: no lwIP, no sockets, no timers, so it
 * can be unit tested on a host. The lwIP glue (dns_gethostbyname() and the
 * cyw43 poll loop) lives in sidetnfs_probe.c and drives this state.
 *
 * A configured TNFS host may be either a dotted-quad IPv4 literal or a
 * DNS name. A literal resolves immediately and never causes a DNS query;
 * a name goes through lwIP's resolver, which can answer either straight
 * away from its cache or later via a callback.
 */
#ifndef SIDETNFS_RESOLVE_H
#define SIDETNFS_RESOLVE_H

#include <stdbool.h>
#include <stdint.h>

/** Bounded wait applied to a single DNS lookup, in milliseconds. */
#define SIDETNFS_RESOLVE_TIMEOUT_MS 4000u
/** Poll interval used while waiting for an asynchronous answer. */
#define SIDETNFS_RESOLVE_STEP_MS 10u

typedef enum
{
    SIDETNFS_RESOLVE_IDLE = 0,  ///< Nothing attempted yet for this slot.
    SIDETNFS_RESOLVE_PENDING,   ///< DNS query issued, waiting for an answer.
    SIDETNFS_RESOLVE_DONE,      ///< addr holds a usable IPv4 address.
    SIDETNFS_RESOLVE_FAILED,    ///< Lookup failed or timed out; addr is meaningless.
} sidetnfs_resolve_state_t;

/**
 * Per-slot resolution state.
 *
 * Every lookup carries a request_id drawn from a monotonically increasing
 * counter. lwIP may deliver a callback for a lookup that already timed out,
 * and such a lookup cannot be cancelled, so a state check alone is not
 * enough: a retry for the same slot puts the state back to PENDING and a
 * stale answer would otherwise be accepted as the retry's result. Matching
 * on request_id closes that window, because an id is never reused.
 */
typedef struct
{
    sidetnfs_resolve_state_t state;
    uint32_t addr;       ///< IPv4 address in lwIP ip_addr_t.addr form; valid only when state == DONE.
    uint32_t elapsed_ms; ///< Time spent waiting on the current lookup.
    uint32_t request_id; ///< Identifies the in-flight lookup; meaningful only while PENDING.
    int slot;            ///< Owning runtime slot, for diagnostics.
} sidetnfs_resolve_t;

/**
 * Parses a strict dotted-quad IPv4 literal.
 *
 * Accepts exactly four decimal octets in 0..255 separated by single dots,
 * with no leading/trailing characters. Rejects host names, partial
 * addresses and out-of-range octets.
 *
 * @param host     NUL-terminated candidate string; may be NULL.
 * @param out_addr Receives the address in lwIP ip_addr_t.addr byte order
 *                 (first octet in the least significant byte) on success.
 *
 * @return true if host was a dotted-quad literal, false otherwise.
 */
bool sidetnfs_resolve_parse_ipv4(const char *host, uint32_t *out_addr);

/**
 * Returns the state to IDLE and records the owning slot. Any address from
 * an earlier attempt is discarded, so a previously failed slot can be
 * retried.
 *
 * @param r    State to reset; NULL is ignored.
 * @param slot Owning runtime slot index.
 */
void sidetnfs_resolve_reset(sidetnfs_resolve_t *r, int slot);

/**
 * Marks a lookup as started and waiting for an answer.
 *
 * @param r          State to move to PENDING; NULL is ignored.
 * @param request_id Identifier for this lookup. Must never be reused, so
 *                   that a late answer to an earlier lookup can be told
 *                   apart from the answer to this one.
 */
void sidetnfs_resolve_begin(sidetnfs_resolve_t *r, uint32_t request_id);

/**
 * Reports whether an answer carrying request_id belongs to the lookup this
 * state is currently waiting for.
 *
 * @param r          State to test; NULL yields false.
 * @param request_id Identifier carried by the incoming answer.
 *
 * @return true only when r is PENDING on exactly that request.
 */
bool sidetnfs_resolve_accepts(const sidetnfs_resolve_t *r, uint32_t request_id);

/**
 * Applies a resolver answer, from either the synchronous cache hit or the
 * asynchronous callback.
 *
 * The answer is applied only when r is PENDING on exactly request_id. An
 * answer to a lookup that timed out is therefore ignored, and so is an
 * answer to a superseded lookup even when a retry has since put the same
 * state back into PENDING.
 *
 * @param r          State to complete; NULL is ignored.
 * @param request_id Identifier of the lookup being answered.
 * @param addr       Resolved address in ip_addr_t.addr form, or NULL for failure.
 */
void sidetnfs_resolve_complete(sidetnfs_resolve_t *r, uint32_t request_id, const uint32_t *addr);

/**
 * Advances the bounded wait by one poll interval.
 *
 * @param r          State being waited on; NULL yields FAILED.
 * @param step_ms    Milliseconds elapsed since the previous tick.
 * @param timeout_ms Total budget for this lookup.
 *
 * @return The resulting state. PENDING means keep polling; the state
 *         becomes FAILED once elapsed time reaches timeout_ms.
 */
sidetnfs_resolve_state_t sidetnfs_resolve_tick(sidetnfs_resolve_t *r, uint32_t step_ms, uint32_t timeout_ms);

#endif // SIDETNFS_RESOLVE_H

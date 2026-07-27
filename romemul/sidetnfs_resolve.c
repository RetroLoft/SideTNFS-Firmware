#include "include/sidetnfs_resolve.h"

#include <stddef.h>

bool sidetnfs_resolve_parse_ipv4(const char *host, uint32_t *out_addr)
{
    if (host == NULL || out_addr == NULL)
    {
        return false;
    }

    uint32_t octets[4];
    const char *p = host;
    for (int i = 0; i < 4; i++)
    {
        if (*p < '0' || *p > '9')
        {
            return false; // every octet needs at least one digit
        }
        uint32_t v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9')
        {
            v = (v * 10u) + (uint32_t)(*p - '0');
            p++;
            if (++digits > 3 || v > 255u)
            {
                return false;
            }
        }
        octets[i] = v;
        if (i < 3)
        {
            if (*p != '.')
            {
                return false;
            }
            p++;
        }
    }
    if (*p != '\0')
    {
        return false; // trailing characters -- not a bare literal
    }

    // ip_addr_t.addr keeps the first octet in the least significant byte.
    *out_addr = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return true;
}

void sidetnfs_resolve_reset(sidetnfs_resolve_t *r, int slot)
{
    if (r == NULL)
    {
        return;
    }
    r->state = SIDETNFS_RESOLVE_IDLE;
    r->addr = 0;
    r->elapsed_ms = 0;
    r->request_id = 0;
    r->slot = slot;
}

void sidetnfs_resolve_begin(sidetnfs_resolve_t *r, uint32_t request_id)
{
    if (r == NULL)
    {
        return;
    }
    r->state = SIDETNFS_RESOLVE_PENDING;
    r->addr = 0;
    r->elapsed_ms = 0;
    r->request_id = request_id;
}

bool sidetnfs_resolve_accepts(const sidetnfs_resolve_t *r, uint32_t request_id)
{
    return r != NULL && r->state == SIDETNFS_RESOLVE_PENDING && r->request_id == request_id;
}

void sidetnfs_resolve_complete(sidetnfs_resolve_t *r, uint32_t request_id, const uint32_t *addr)
{
    if (!sidetnfs_resolve_accepts(r, request_id))
    {
        // Either nothing is outstanding, or this answer belongs to an
        // earlier lookup that has since timed out and been superseded.
        return;
    }
    if (addr != NULL)
    {
        r->addr = *addr;
        r->state = SIDETNFS_RESOLVE_DONE;
    }
    else
    {
        r->addr = 0;
        r->state = SIDETNFS_RESOLVE_FAILED;
    }
}

sidetnfs_resolve_state_t sidetnfs_resolve_tick(sidetnfs_resolve_t *r, uint32_t step_ms, uint32_t timeout_ms)
{
    if (r == NULL)
    {
        return SIDETNFS_RESOLVE_FAILED;
    }
    if (r->state != SIDETNFS_RESOLVE_PENDING)
    {
        return r->state;
    }
    r->elapsed_ms += step_ms;
    if (r->elapsed_ms >= timeout_ms)
    {
        r->state = SIDETNFS_RESOLVE_FAILED;
    }
    return r->state;
}

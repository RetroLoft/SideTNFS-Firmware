#include "include/fs_backend.h"
#include "include/fs_tnfs.h"
#include "include/tnfs_client.h"
#include "include/gemdrvemul.h"
#include "include/debug.h"
#include "wifi_config.h"

#include <string.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/ip_addr.h"

// ─── Configuration ────────────────────────────────────────────────────────────

// TNFS path that drive N:\ maps to.
#define TNFS_ROOT_PATH              "/Atari.ST"

// Max entries requested per single READDIRX packet.
// Keep small so each UDP response fits in TNFS_MTU (512 bytes).
// 4 × (13-byte fixed + ~14-byte name) ≈ 108 bytes + 9 header = well within 512.
#define TNFS_READDIRX_MAX_ENTRIES   4u

// Max data bytes requested per single TNFS READ call.
// Response: 4-byte header + 1 rc + 2 len + data  →  512 - 7 = 505 bytes max.
// We use 504 so the response is 511 bytes = TNFS_MTU-1, which is what
// tnfs_client_recv() accepts without triggering the rlen-clamping guard.
#define TNFS_READ_CHUNK             504u

// Max data per single TNFS WRITE call (same cap for symmetry).
#define TNFS_WRITE_CHUNK            504u

#define CACHE_MAX           256     // total cached entries per directory
#define CACHE_TTL_MS        30000   // directory cache lifetime in ms
#define FETCH_TIMEOUT_MS    2000    // per-packet wait timeout
#define FETCH_RETRIES       3       // retransmits before giving up

#define FS_MAX_HANDLES      8       // concurrent open files (static pool)

// ─── Directory cache ──────────────────────────────────────────────────────────
// Each entry stores the uppercased name shown to GEMDOS plus the original
// server-side name used for TNFS OPEN/STAT to preserve case on Linux servers.

typedef struct {
    FsEntry base;       // what GEMDOS sees (name is uppercased)
    char    orig[14];   // original server case for TNFS path construction
} CacheItem;

static CacheItem s_cache[CACHE_MAX];
static int       s_cache_count = 0;
static char      s_cache_dir[MAX_FOLDER_LENGTH] = "";
static uint32_t  s_cache_ms    = 0;
static bool      s_cache_valid = false;

// ─── TNFS session ─────────────────────────────────────────────────────────────

static bool     s_mounted    = false;
static uint16_t s_session    = 0;
static uint8_t  s_req_id     = 0;
static uint8_t  s_tx_buf[TNFS_MTU];
static uint8_t  s_rx_buf[TNFS_MTU];

// ─── Open-file handle pool ────────────────────────────────────────────────────

struct FsHandle {
    bool     in_use;
    uint8_t  tnfs_fd;   // TNFS server-side file handle
    uint32_t pos;       // current byte offset
    uint32_t size;      // file size in bytes (from STAT at open time)
    bool     writable;  // true when opened for writing
};

static struct FsHandle s_fh_pool[FS_MAX_HANDLES];
static int             s_file_count = 0;  // open files; controls socket lifetime

// ─── Helpers ──────────────────────────────────────────────────────────────────

static uint16_t unix_to_fat_date(uint32_t t)
{
    uint32_t days = t / 86400u;
    if (days < 3652u) days = 0u; else days -= 3652u;
    uint32_t year = days / 365u;
    uint32_t rem  = days - year * 365u;
    static const uint8_t md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint8_t month = 0;
    while (month < 11u && rem >= md[month]) { rem -= md[month]; month++; }
    return (uint16_t)(((year & 0x7Fu) << 9) | (((month+1u) & 0x0Fu) << 5) | ((rem+1u) & 0x1Fu));
}

static uint16_t unix_to_fat_time(uint32_t t)
{
    uint32_t sod = t % 86400u;
    uint8_t h = (uint8_t)(sod / 3600u);
    uint8_t m = (uint8_t)((sod % 3600u) / 60u);
    uint8_t s = (uint8_t)(sod % 60u);
    return (uint16_t)(((h & 0x1Fu) << 11) | ((m & 0x3Fu) << 5) | ((s / 2u) & 0x1Fu));
}

// '*.*' and '*' mean "match everything" in Atari TOS wildcard convention.
static bool wildmatch(const char *pat, const char *str)
{
    if (pat[0]=='*' && pat[1]=='.' && pat[2]=='*' && pat[3]=='\0') return true;
    if (pat[0]=='*' && pat[1]=='\0')                                return true;
    if (*pat=='\0' && *str=='\0')                                   return true;
    if (*pat=='\0' || *str=='\0')                                   return false;
    if (*pat=='?' || *pat==*str) return wildmatch(pat+1, str+1);
    if (*pat=='*') return wildmatch(pat+1, str) || wildmatch(pat, str+1);
    return false;
}

// Convert a GEMDOS path (with leading backslash, e.g. "\" or "\GAMES") to a
// TNFS absolute path rooted at TNFS_ROOT_PATH.
static void gemdos_to_tnfs(const char *gemdos, char *tnfs, int max)
{
    int j = 0;
    const char *r = TNFS_ROOT_PATH;
    while (*r && j < max-1) tnfs[j++] = *r++;

    const char *p = gemdos;
    while (*p == '\\' || *p == '/') p++;

    if (*p && j < max-1) {
        tnfs[j++] = '/';
        for (; *p && j < max-1; p++)
            tnfs[j++] = (*p == '\\') ? '/' : *p;
    }

    int root_len = (int)strlen(TNFS_ROOT_PATH);
    while (j > root_len+1 && tnfs[j-1] == '/') j--;
    tnfs[j] = '\0';
}

// Convert a GEMDOS relative file path (no leading backslash, already uppercase,
// e.g. "README.TXT" or "GAMES\RPG.PRG") to a TNFS absolute path.
// Tries to restore original server case from the directory cache.
static void relpath_to_tnfs(const char *upper_rel, char *out, int max)
{
    // Split into directory part + filename part
    const char *sep = NULL;
    for (const char *p = upper_rel; *p; p++)
        if (*p == '\\' || *p == '/') sep = p;

    const char *fname_upper = sep ? sep + 1 : upper_rel;

    // Build TNFS directory path from the directory component
    char tnfs_dir[MAX_FOLDER_LENGTH];
    if (sep) {
        char dir_part[MAX_FOLDER_LENGTH];
        size_t dlen = (size_t)(sep - upper_rel);
        if (dlen >= MAX_FOLDER_LENGTH) dlen = MAX_FOLDER_LENGTH - 1;
        memcpy(dir_part, upper_rel, dlen);
        dir_part[dlen] = '\0';
        gemdos_to_tnfs(dir_part, tnfs_dir, sizeof(tnfs_dir));
    } else {
        strncpy(tnfs_dir, TNFS_ROOT_PATH, sizeof(tnfs_dir) - 1);
        tnfs_dir[sizeof(tnfs_dir) - 1] = '\0';
    }

    // Look up original server-case filename in cache for this directory
    const char *fname_orig = fname_upper;
    if (s_cache_valid && strcmp(s_cache_dir, tnfs_dir) == 0) {
        for (int i = 0; i < s_cache_count; i++) {
            if (strcmp(s_cache[i].base.name, fname_upper) == 0) {
                fname_orig = s_cache[i].orig;
                break;
            }
        }
    }

    snprintf(out, (size_t)max, "%s/%s", tnfs_dir, fname_orig);
}

// Build TNFS header in s_tx_buf; advance s_req_id (retransmits re-send unchanged).
static uint8_t *prep(uint16_t session, uint8_t cmd)
{
    s_tx_buf[0] = (uint8_t)(session & 0xFFu);
    s_tx_buf[1] = (uint8_t)(session >> 8);
    s_tx_buf[2] = s_req_id++;
    s_tx_buf[3] = cmd;
    return s_tx_buf + 4;
}

static bool do_send(uint16_t len)
{
    cyw43_arch_lwip_begin();
    bool ok = tnfs_client_send(s_tx_buf, len);
    cyw43_arch_lwip_end();
    return ok;
}

static bool wait_match(uint16_t *out_len, uint8_t expect_cmd,
                       uint16_t session, uint8_t seq)
{
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while ((to_ms_since_boot(get_absolute_time()) - start) < FETCH_TIMEOUT_MS) {
        cyw43_arch_lwip_begin();
        uint16_t n = tnfs_client_recv(s_rx_buf, TNFS_MTU - 1u);
        cyw43_arch_lwip_end();
        if (n < 5u) continue;
        s_rx_buf[n] = '\0';
        if (s_rx_buf[3] != expect_cmd) continue;
        if (s_rx_buf[2] != seq)        continue;
        if (expect_cmd != TNFS_CMD_MOUNT) {
            uint16_t got = (uint16_t)s_rx_buf[0] | ((uint16_t)s_rx_buf[1] << 8);
            if (got != session) continue;
        }
        *out_len = n;
        return true;
    }
    return false;
}

static bool send_recv(uint16_t tx_len, uint16_t *rx_len,
                      uint8_t expect_cmd, uint16_t session)
{
    uint8_t seq = s_tx_buf[2];
    for (int r = 0; r < FETCH_RETRIES; r++) {
        if (!do_send(tx_len)) continue;
        if (wait_match(rx_len, expect_cmd, session, seq)) return true;
        DPRINTF("[TNFS FS] cmd=%02x timeout (retry %d/%d)\n",
                expect_cmd, r+1, FETCH_RETRIES);
    }
    return false;
}

static bool is_path_error(uint8_t rc)
{
    return rc == 0x01u || // EPERM
           rc == 0x02u || // ENOENT
           rc == 0x09u || // EACCES
           rc == 0x0Cu || // ENOTDIR
           rc == 0x14u;   // EROFS
}

// ─── Socket lifecycle ─────────────────────────────────────────────────────────
// The socket is kept open for the lifetime of any open FsHandle (s_file_count>0).
// Directory operations borrow the open socket instead of close/reopening it.

static bool socket_open_raw(void)
{
    ip_addr_t ip;
    ipaddr_aton(TNFS_SERVER, &ip);
    cyw43_arch_lwip_begin();
    tnfs_client_close();
    bool ok = tnfs_client_open(&ip, TNFS_PORT);
    cyw43_arch_lwip_end();
    return ok;
}

static void socket_close_raw(void)
{
    cyw43_arch_lwip_begin();
    tnfs_client_close();
    cyw43_arch_lwip_end();
}

// Ensure socket is open. Returns true if socket is now ready.
// *we_opened is set to true only if this call opened it (caller must close it
// after use if no open files are holding it).
static bool socket_ensure(bool *we_opened)
{
    if (s_file_count > 0) { *we_opened = false; return true; } // already held open
    bool ok = socket_open_raw();
    *we_opened = ok;
    return ok;
}

static void socket_release(bool we_opened)
{
    if (we_opened) socket_close_raw();
}

// ─── TNFS MOUNT ───────────────────────────────────────────────────────────────

static bool do_mount(void)
{
    uint8_t *p = prep(0x0000u, TNFS_CMD_MOUNT);
    *p++ = TNFS_PROTO_VER_MINOR;
    *p++ = TNFS_PROTO_VER_MAJOR;
    *p++ = '/'; *p++ = '\0';
    *p++ = '\0';
    *p++ = '\0';
    uint16_t len = (uint16_t)(p - s_tx_buf);
    uint16_t rlen;
    if (!send_recv(len, &rlen, TNFS_CMD_MOUNT, 0u) || s_rx_buf[4] != TNFS_OK) {
        LOG("[TNFS FS] MOUNT failed\n");
        s_mounted = false;
        return false;
    }
    s_session = (uint16_t)s_rx_buf[0] | ((uint16_t)s_rx_buf[1] << 8);
    s_mounted  = true;
    LOG("[TNFS FS] mounted sid=%04x\n", s_session);
    return true;
}

// ─── Directory load (OPENDIRX / READDIRX) ────────────────────────────────────

static bool load_dir(const char *tnfs_path)
{
    s_cache_count = 0;
    s_cache_valid = false;

    bool we_opened;
    if (!socket_ensure(&we_opened)) {
        LOG("[TNFS FS] socket open failed\n");
        return false;
    }
    if (!s_mounted && !do_mount()) {
        socket_release(we_opened);
        return false;
    }

    uint32_t t0 = to_ms_since_boot(get_absolute_time());

    // ── OPENDIRX ───────────────────────────────────────────────────────────────
    // Payload: diropts(1) sortopts(1) max_count(2) pattern(null) path(null)
    uint8_t dir_handle;
    {
        uint8_t *p    = prep(s_session, TNFS_CMD_OPENDIRX);
        *p++ = 0x00u;           // diropts: default (dirs first, skip hidden/special)
        *p++ = 0x00u;           // sortopts: default (ascending by name)
        *p++ = 0x00u;           // max_count lo (0 = return total count in response)
        *p++ = 0x00u;           // max_count hi
        *p++ = '*'; *p++ = '\0'; // pattern: match all files
        size_t plen = strlen(tnfs_path);
        memcpy(p, tnfs_path, plen + 1u);
        p += plen + 1u;
        uint16_t tx_len = (uint16_t)(p - s_tx_buf);
        uint16_t rlen   = 0;

        LOG("[TNFS FS] OPENDIRX %s\n", tnfs_path);

        bool got = send_recv(tx_len, &rlen, TNFS_CMD_OPENDIRX, s_session);
        uint32_t t_opendirx = to_ms_since_boot(get_absolute_time());
        LOG("[TNFS FS] OPENDIRX done in %ums\n", t_opendirx - t0);

        if (!got) {
            LOG("[TNFS FS] OPENDIRX no response — clearing session\n");
            s_mounted = false;
            socket_release(we_opened);
            return false;
        }

        uint8_t rc = s_rx_buf[4];
        if (rc != TNFS_OK) {
            if (is_path_error(rc)) {
                LOG("[TNFS FS] OPENDIRX %s: not found (rc=%02x), empty dir\n",
                    tnfs_path, rc);
            } else {
                LOG("[TNFS FS] OPENDIRX rc=%02x — remounting\n", rc);
                s_mounted = false;
                if (!do_mount()) { socket_release(we_opened); return false; }
                // One retry after remount
                p    = prep(s_session, TNFS_CMD_OPENDIRX);
                *p++ = 0x00u; *p++ = 0x00u; *p++ = 0x00u; *p++ = 0x00u;
                *p++ = '*'; *p++ = '\0';
                memcpy(p, tnfs_path, plen + 1u); p += plen + 1u;
                tx_len = (uint16_t)(p - s_tx_buf);
                if (!send_recv(tx_len, &rlen, TNFS_CMD_OPENDIRX, s_session) ||
                    s_rx_buf[4] != TNFS_OK) {
                    LOG("[TNFS FS] OPENDIRX retry failed — empty dir\n");
                    goto done;
                }
                rc = s_rx_buf[4];
            }
            if (rc != TNFS_OK) goto done;
        }

        if (rlen < 8u) { LOG("[TNFS FS] OPENDIRX short response\n"); goto done; }
        dir_handle = s_rx_buf[5];
        uint16_t total;
        memcpy(&total, &s_rx_buf[6], 2u);
        LOG("[TNFS FS] OPENDIRX handle=%u total=%u\n", dir_handle, total);
    }

    // ── READDIRX loop ──────────────────────────────────────────────────────────
    {
        bool done_reading = false;
        int  batch_num    = 0;
        while (!done_reading && s_cache_count < CACHE_MAX) {
            uint8_t *p = prep(s_session, TNFS_CMD_READDIRX);
            *p++ = dir_handle;
            *p++ = TNFS_READDIRX_MAX_ENTRIES;
            uint16_t tx_len = (uint16_t)(p - s_tx_buf);
            uint16_t rlen   = 0;
            uint32_t t_b0   = to_ms_since_boot(get_absolute_time());

            LOG("[TNFS FS] READDIRX batch %d req=%u\n", batch_num, TNFS_READDIRX_MAX_ENTRIES);

            if (!send_recv(tx_len, &rlen, TNFS_CMD_READDIRX, s_session)) {
                LOG("[TNFS FS] READDIRX timeout after %d entries\n", s_cache_count);
                done_reading = true; break;
            }

            uint32_t t_b1 = to_ms_since_boot(get_absolute_time());

            if (rlen < 5u) { done_reading = true; break; }
            uint8_t rc = s_rx_buf[4];
            if (rc != TNFS_OK) {
                if (rc != TNFS_EOF) DPRINTF("[TNFS FS] READDIRX rc=%02x\n", rc);
                done_reading = true; break;
            }
            if (rlen <= 8u) { done_reading = true; break; }

            uint8_t batch  = s_rx_buf[5];
            uint8_t status = s_rx_buf[6];
            LOG("[TNFS FS] READDIRX batch %d: %u entries in %ums\n",
                batch_num, batch, t_b1 - t_b0);
            batch_num++;

            if (batch == 0u) { done_reading = true; break; }

            // Parse entries: flags(1)+size(4)+mtime(4)+ctime(4)+name(null-term)
            // needle advances by strlen(name)+14  (matches tnfscmdr tnfs_nextdirx)
            uint16_t needle = 9u;
            for (uint8_t i = 0u; i < batch && s_cache_count < CACHE_MAX; i++) {
                if ((uint32_t)needle + 13u >= rlen) {
                    LOG("[TNFS FS] READDIRX truncated at entry %u (needle=%u rlen=%u)\n",
                        i, needle, rlen);
                    done_reading = true; break;
                }
                uint8_t  flags = s_rx_buf[needle];
                uint32_t size, mtime;
                memcpy(&size,  &s_rx_buf[needle + 1u], 4u);
                memcpy(&mtime, &s_rx_buf[needle + 5u], 4u);

                const char *name  = (const char *)&s_rx_buf[needle + 13u];
                uint16_t    avail = rlen - (needle + 13u);
                size_t      nlen  = strnlen(name, avail);
                if (nlen >= avail) {
                    LOG("[TNFS FS] READDIRX unterminated name at entry %u\n", i);
                    done_reading = true; break;
                }
                needle = (uint16_t)(needle + 14u + nlen);

                // Skip hidden / special entries
                if (flags & (TNFS_DIRENTRY_HIDDEN | TNFS_DIRENTRY_SPECIAL)) continue;
                // Skip dot entries
                if (nlen == 0u || (name[0]=='.' && (nlen==1u || (name[1]=='.' && nlen==2u)))) continue;
                // Skip names that don't fit FsEntry.name[14] (max 13 chars + NUL)
                if (nlen > 13u) { DPRINTF("[TNFS FS] skip long name \"%s\"\n", name); continue; }

                CacheItem *e = &s_cache[s_cache_count++];
                memset(e, 0, sizeof(*e));
                for (size_t j = 0u; j < nlen; j++) {
                    e->base.name[j] = (char)toupper((unsigned char)name[j]);
                    e->orig[j]      = name[j];
                }
                e->base.name[nlen] = '\0';
                e->orig[nlen]      = '\0';
                e->base.size = size;
                e->base.attr = (flags & TNFS_DIRENTRY_DIR) ? 0x10u : 0x20u;
                e->base.date = unix_to_fat_date(mtime);
                e->base.time = unix_to_fat_time(mtime);

                LOG("[TNFS FS]   %s \"%s\" %lu bytes\n",
                    (flags & TNFS_DIRENTRY_DIR) ? "DIR " : "FILE",
                    e->base.name, (unsigned long)size);
            }

            if (status & TNFS_DIRSTATUS_EOF) {
                LOG("[TNFS FS] READDIRX EOF\n");
                done_reading = true;
            }
        }
        LOG("[TNFS FS] loaded %d entries in %d batches, total %ums\n",
            s_cache_count, batch_num,
            to_ms_since_boot(get_absolute_time()) - t0);
    }

    // ── CLOSEDIR (best-effort) ─────────────────────────────────────────────────
    {
        uint8_t *p = prep(s_session, TNFS_CMD_CLOSEDIR);
        *p++ = dir_handle;
        uint16_t len = (uint16_t)(p - s_tx_buf);
        uint16_t rlen;
        send_recv(len, &rlen, TNFS_CMD_CLOSEDIR, s_session);
    }

done:
    socket_release(we_opened);
    s_cache_valid = true;
    s_cache_ms    = to_ms_since_boot(get_absolute_time());
    strncpy(s_cache_dir, tnfs_path, sizeof(s_cache_dir) - 1u);
    s_cache_dir[sizeof(s_cache_dir) - 1u] = '\0';
    return true;
}

// ─── fs_backend.h API ─────────────────────────────────────────────────────────

void fs_init(void)
{
    s_cache_valid = false;
    s_cache_count = 0;
    s_mounted     = false;
    s_session     = 0u;
    s_req_id      = 0u;
    s_file_count  = 0;
    memset(s_fh_pool, 0, sizeof(s_fh_pool));
}

bool fs_list_dir(const char *dir, const char *pat, int index, FsEntry *out)
{
    char tnfs_path[MAX_FOLDER_LENGTH];
    gemdos_to_tnfs(dir, tnfs_path, sizeof(tnfs_path));

    uint32_t now     = to_ms_since_boot(get_absolute_time());
    bool     expired = (now - s_cache_ms) >= CACHE_TTL_MS;
    bool     same    = s_cache_valid && (strcmp(tnfs_path, s_cache_dir) == 0);

    if (same && !expired) {
        DPRINTF("[TNFS FS] cache hit %s\n", tnfs_path);
    } else {
        LOG("[TNFS FS] cache miss %s%s\n",
            tnfs_path, (same && expired) ? " (expired)" : "");
        if (!load_dir(tnfs_path)) {
            s_cache_valid = false;
            s_cache_count = 0;
            return false;
        }
    }

    int match = 0;
    for (int i = 0; i < s_cache_count; i++) {
        if (wildmatch(pat, s_cache[i].base.name)) {
            if (match == index) { *out = s_cache[i].base; return true; }
            match++;
        }
    }
    return false;
}

bool fs_stat(const char *path, FsEntry *out)
{
    char tnfs_path[MAX_FOLDER_LENGTH];
    relpath_to_tnfs(path, tnfs_path, sizeof(tnfs_path));
    DPRINTF("[TNFS FS] STAT %s\n", tnfs_path);

    bool we_opened;
    if (!socket_ensure(&we_opened)) return false;
    if (!s_mounted && !do_mount()) { socket_release(we_opened); return false; }

    uint8_t *p = prep(s_session, TNFS_CMD_STAT);
    size_t plen = strlen(tnfs_path);
    memcpy(p, tnfs_path, plen + 1u);
    p += plen + 1u;
    uint16_t tx_len = (uint16_t)(p - s_tx_buf);
    uint16_t rlen;

    if (!send_recv(tx_len, &rlen, TNFS_CMD_STAT, s_session) ||
        s_rx_buf[4] != TNFS_OK || rlen < 27u) {
        DPRINTF("[TNFS FS] STAT %s failed (rc=%02x)\n", tnfs_path,
                rlen >= 5u ? s_rx_buf[4] : 0xFF);
        socket_release(we_opened);
        return false;
    }

    // STAT response: buf[5-6]=mode, buf[7-8]=uid, buf[9-10]=gid,
    //                buf[11-14]=size LE, buf[15-18]=atime, buf[19-22]=mtime
    uint16_t mode; uint32_t size, mtime;
    memcpy(&mode,  &s_rx_buf[5],  2u);
    memcpy(&size,  &s_rx_buf[11], 4u);
    memcpy(&mtime, &s_rx_buf[19], 4u);

    // Unix mode S_IFDIR = 0x4000, S_IFREG = 0x8000
    bool is_dir = (mode & 0xF000u) == 0x4000u;

    const char *basename = path;
    for (const char *q = path; *q; q++)
        if (*q == '\\' || *q == '/') basename = q + 1;

    memset(out, 0, sizeof(*out));
    size_t nlen = strlen(basename);
    if (nlen > 13u) nlen = 13u;
    for (size_t j = 0u; j < nlen; j++)
        out->name[j] = (char)toupper((unsigned char)basename[j]);
    out->name[nlen] = '\0';
    out->size = size;
    out->attr = is_dir ? 0x10u : 0x20u;
    out->date = unix_to_fat_date(mtime);
    out->time = unix_to_fat_time(mtime);

    socket_release(we_opened);
    return true;
}

// Internal helper: open/create a TNFS file with explicit flags.
static FsHandle *open_with_flags(const char *tnfs_path, uint16_t tnfs_flags,
                                 bool writable, int16_t *gemdos_err)
{
    if (s_file_count == 0 && !socket_open_raw()) {
        *gemdos_err = GEMDOS_EDRVNR;
        return NULL;
    }
    if (!s_mounted && !do_mount()) {
        if (s_file_count == 0) socket_close_raw();
        *gemdos_err = GEMDOS_EDRVNR;
        return NULL;
    }

    struct FsHandle *h = NULL;
    for (int i = 0; i < FS_MAX_HANDLES; i++) {
        if (!s_fh_pool[i].in_use) { h = &s_fh_pool[i]; break; }
    }
    if (!h) {
        if (s_file_count == 0) socket_close_raw();
        *gemdos_err = GEMDOS_ENHNDL;
        return NULL;
    }

    // TNFS OPEN: flags LE(2) + mode LE(2) + path(null)
    {
        uint8_t *p = prep(s_session, TNFS_CMD_OPEN);
        *p++ = (uint8_t)(tnfs_flags & 0xFFu);
        *p++ = (uint8_t)(tnfs_flags >> 8);
        *p++ = (uint8_t)(TNFS_MODE_0644 & 0xFFu);
        *p++ = (uint8_t)(TNFS_MODE_0644 >> 8);
        size_t plen = strlen(tnfs_path);
        memcpy(p, tnfs_path, plen + 1u);
        p += plen + 1u;
        uint16_t tx_len = (uint16_t)(p - s_tx_buf);
        uint16_t rlen;
        if (!send_recv(tx_len, &rlen, TNFS_CMD_OPEN, s_session) ||
            s_rx_buf[4] != TNFS_OK) {
            uint8_t rc = (rlen >= 5u) ? s_rx_buf[4] : 0xFFu;
            if (s_file_count == 0) socket_close_raw();
            switch (rc) {
                case 0x02u: *gemdos_err = GEMDOS_EFILNF;  break;
                case 0x14u: *gemdos_err = GEMDOS_EACCDN;  break;
                default:    *gemdos_err = GEMDOS_EACCDN;  break;
            }
            return NULL;
        }
        h->tnfs_fd = s_rx_buf[5];
    }

    // Get accurate file size via LSEEK(SEEK_END, 0) then LSEEK(SEEK_SET, 0).
    // STAT offsets differ between tnfsd implementations (uint16 vs uint32 uid/gid),
    // so we avoid STAT for size and use LSEEK which has an unambiguous format.
    // Only do this for read-only opens — writable files start at size 0 (newly
    // created or opened for overwrite) and fs_write tracks growth via h->size.
    uint32_t file_size = 0;
    if (!writable) {
        uint32_t zero = 0u;
        uint16_t rlen;

        uint8_t *p = prep(s_session, TNFS_CMD_LSEEK);
        *p++ = h->tnfs_fd;
        *p++ = 2u;  // SEEK_END
        memcpy(p, &zero, 4u); p += 4u;
        uint16_t tx_len = (uint16_t)(p - s_tx_buf);
        if (send_recv(tx_len, &rlen, TNFS_CMD_LSEEK, s_session) &&
            s_rx_buf[4] == TNFS_OK && rlen >= 9u)
            memcpy(&file_size, &s_rx_buf[5], 4u);

        // Seek back to start regardless of whether the seek-to-end succeeded
        p = prep(s_session, TNFS_CMD_LSEEK);
        *p++ = h->tnfs_fd;
        *p++ = 0u;  // SEEK_SET
        memcpy(p, &zero, 4u); p += 4u;
        tx_len = (uint16_t)(p - s_tx_buf);
        send_recv(tx_len, &rlen, TNFS_CMD_LSEEK, s_session);
    }

    h->in_use   = true;
    h->pos      = 0u;
    h->size     = file_size;
    h->writable = writable;
    s_file_count++;
    return h;
}

FsHandle *fs_open(const char *path, uint16_t mode, int16_t *gemdos_err)
{
    char tnfs_path[MAX_FOLDER_LENGTH];
    relpath_to_tnfs(path, tnfs_path, sizeof(tnfs_path));

    uint16_t tnfs_flags;
    switch (mode) {
        case 1:  tnfs_flags = TNFS_O_WRONLY; break;
        case 2:  tnfs_flags = TNFS_O_RDWR;   break;
        default: tnfs_flags = TNFS_O_RDONLY;  break;
    }
    return open_with_flags(tnfs_path, tnfs_flags, (mode != 0), gemdos_err);
}

FsHandle *fs_create(const char *path, int16_t *gemdos_err)
{
    char tnfs_path[MAX_FOLDER_LENGTH];
    relpath_to_tnfs(path, tnfs_path, sizeof(tnfs_path));

    // Invalidate directory cache so the new file appears in listings
    s_cache_valid = false;

    uint16_t flags = TNFS_O_WRONLY | TNFS_O_CREAT | TNFS_O_TRUNC;
    return open_with_flags(tnfs_path, flags, true, gemdos_err);
}

uint32_t fs_read(FsHandle *h, void *buf, uint32_t len)
{
    if (!h || !h->in_use || len == 0u) return 0u;

    // Clamp read to remaining file size; return early if already at or past EOF.
    if (h->size > 0u) {
        if (h->pos >= h->size) return 0u;
        if (len > h->size - h->pos) len = h->size - h->pos;
    }

    uint32_t total = 0u;
    uint8_t *dst   = (uint8_t *)buf;

    while (total < len) {
        uint16_t chunk = (uint16_t)((len - total) < TNFS_READ_CHUNK
                                    ? (len - total) : TNFS_READ_CHUNK);
        uint8_t *p = prep(s_session, TNFS_CMD_READ);
        *p++ = h->tnfs_fd;
        *p++ = (uint8_t)(chunk & 0xFFu);
        *p++ = (uint8_t)(chunk >> 8);
        uint16_t tx_len = (uint16_t)(p - s_tx_buf);
        uint16_t rlen;

        if (!send_recv(tx_len, &rlen, TNFS_CMD_READ, s_session)) {
            DPRINTF("[TNFS FS] READ timeout after %lu bytes\n", (unsigned long)total);
            break;
        }
        if (s_rx_buf[4] == TNFS_EOF || rlen < 7u) break;
        if (s_rx_buf[4] != TNFS_OK) {
            DPRINTF("[TNFS FS] READ rc=%02x\n", s_rx_buf[4]);
            break;
        }

        uint16_t actual;
        memcpy(&actual, &s_rx_buf[5], 2u);
        if (actual == 0u) break;

        // Data starts at s_rx_buf[7]; validate it fits in the response
        if ((uint32_t)actual + 7u > rlen) actual = (uint16_t)(rlen - 7u);

        memcpy(dst + total, &s_rx_buf[7], actual);
        total  += actual;
        h->pos += actual;
        // Do NOT break on short read — TNFS spec allows server to return fewer
        // bytes than requested without signalling EOF. Keep fetching until the
        // server returns 0 bytes (caught by `actual == 0` above) or TNFS_EOF.
    }

    DPRINTF("[TNFS FS] read %lu bytes\n", (unsigned long)total);
    return total;
}

int32_t fs_seek(FsHandle *h, int32_t offset, int whence)
{
    if (!h || !h->in_use) return GEMDOS_EINTRN;

    int32_t new_pos;
    switch (whence) {
        case 0:  new_pos = offset; break;                            // SEEK_SET
        case 1:  new_pos = (int32_t)h->pos + offset; break;         // SEEK_CUR
        case 2:  new_pos = (int32_t)h->size + offset; break;        // SEEK_END
        default: return GEMDOS_EINTRN;
    }
    if (new_pos < 0) new_pos = 0;

    // TNFS LSEEK: handle(1) + seektype(1) + position(4 LE)
    uint8_t seek_type = (uint8_t)whence;  // 0=SET, 1=CUR, 2=END match TNFS
    uint32_t pos_u    = (uint32_t)new_pos;
    uint8_t *p = prep(s_session, TNFS_CMD_LSEEK);
    *p++ = h->tnfs_fd;
    *p++ = seek_type;
    memcpy(p, &pos_u, 4u); p += 4u;
    uint16_t tx_len = (uint16_t)(p - s_tx_buf);
    uint16_t rlen;

    if (!send_recv(tx_len, &rlen, TNFS_CMD_LSEEK, s_session) ||
        s_rx_buf[4] != TNFS_OK) {
        DPRINTF("[TNFS FS] LSEEK failed rc=%02x\n",
                rlen >= 5u ? s_rx_buf[4] : 0xFFu);
        return GEMDOS_EINTRN;
    }

    h->pos = (uint32_t)new_pos;
    DPRINTF("[TNFS FS] seek → %ld\n", (long)new_pos);
    return new_pos;
}

uint32_t fs_write(FsHandle *h, const void *buf, uint32_t len)
{
    if (!h || !h->in_use || !h->writable || len == 0u) return 0u;

    uint32_t total = 0u;
    const uint8_t *src = (const uint8_t *)buf;

    while (total < len) {
        uint16_t chunk = (uint16_t)((len - total) < TNFS_WRITE_CHUNK
                                    ? (len - total) : TNFS_WRITE_CHUNK);
        uint8_t *p = prep(s_session, TNFS_CMD_WRITE);
        *p++ = h->tnfs_fd;
        *p++ = (uint8_t)(chunk & 0xFFu);
        *p++ = (uint8_t)(chunk >> 8);
        memcpy(p, src + total, chunk);
        p += chunk;
        uint16_t tx_len = (uint16_t)(p - s_tx_buf);
        uint16_t rlen;

        if (!send_recv(tx_len, &rlen, TNFS_CMD_WRITE, s_session)) break;
        if (s_rx_buf[4] != TNFS_OK || rlen < 7u) break;

        uint16_t actual;
        memcpy(&actual, &s_rx_buf[5], 2u);
        total  += actual;
        h->pos += actual;
        if (actual < chunk) break;
    }

    if (h->pos > h->size) h->size = h->pos;
    return total;
}

bool fs_handle_writable(const FsHandle *h)
{
    return h && h->in_use && h->writable;
}

void fs_close(FsHandle *h)
{
    if (!h || !h->in_use) return;

    if (h->writable) s_cache_valid = false;  // size/presence may have changed

    uint8_t *p = prep(s_session, TNFS_CMD_CLOSE);
    *p++ = h->tnfs_fd;
    uint16_t tx_len = (uint16_t)(p - s_tx_buf);
    uint16_t rlen;
    send_recv(tx_len, &rlen, TNFS_CMD_CLOSE, s_session); // best-effort

    h->in_use = false;
    s_file_count--;
    if (s_file_count == 0) socket_close_raw();
}

// ─── File/directory mutation ──────────────────────────────────────────────────

static int16_t tnfs_err_to_gemdos(uint8_t rc)
{
    switch (rc) {
        case 0x02u: return GEMDOS_EFILNF;
        case 0x14u: return GEMDOS_EACCDN;
        case 0x01u: return GEMDOS_EACCDN;
        default:    return GEMDOS_ERROR;
    }
}

int16_t fs_unlink(const char *path)
{
    char tnfs_path[MAX_FOLDER_LENGTH];
    relpath_to_tnfs(path, tnfs_path, sizeof(tnfs_path));

    bool we_opened;
    if (!socket_ensure(&we_opened)) return GEMDOS_EDRVNR;
    if (!s_mounted && !do_mount()) { socket_release(we_opened); return GEMDOS_EDRVNR; }

    uint8_t *p = prep(s_session, TNFS_CMD_UNLINK);
    size_t plen = strlen(tnfs_path);
    memcpy(p, tnfs_path, plen + 1u);
    p += plen + 1u;
    uint16_t tx_len = (uint16_t)(p - s_tx_buf);
    uint16_t rlen;
    bool ok = send_recv(tx_len, &rlen, TNFS_CMD_UNLINK, s_session);
    socket_release(we_opened);

    if (!ok || s_rx_buf[4] != TNFS_OK)
        return tnfs_err_to_gemdos(rlen >= 5u ? s_rx_buf[4] : 0xFFu);

    s_cache_valid = false;  // entry gone — invalidate dir cache
    return GEMDOS_EOK;
}

int16_t fs_mkdir(const char *path)
{
    char tnfs_path[MAX_FOLDER_LENGTH];
    // path has no leading backslash — build TNFS path via gemdos_to_tnfs
    // We re-use gemdos_to_tnfs which expects a GEMDOS path with backslashes.
    gemdos_to_tnfs(path, tnfs_path, sizeof(tnfs_path));

    bool we_opened;
    if (!socket_ensure(&we_opened)) return GEMDOS_EDRVNR;
    if (!s_mounted && !do_mount()) { socket_release(we_opened); return GEMDOS_EDRVNR; }

    uint8_t *p = prep(s_session, TNFS_CMD_MKDIR);
    size_t plen = strlen(tnfs_path);
    memcpy(p, tnfs_path, plen + 1u);
    p += plen + 1u;
    uint16_t tx_len = (uint16_t)(p - s_tx_buf);
    uint16_t rlen;
    bool ok = send_recv(tx_len, &rlen, TNFS_CMD_MKDIR, s_session);
    socket_release(we_opened);

    if (!ok || s_rx_buf[4] != TNFS_OK)
        return tnfs_err_to_gemdos(rlen >= 5u ? s_rx_buf[4] : 0xFFu);

    s_cache_valid = false;
    return GEMDOS_EOK;
}

int16_t fs_rmdir(const char *path)
{
    char tnfs_path[MAX_FOLDER_LENGTH];
    gemdos_to_tnfs(path, tnfs_path, sizeof(tnfs_path));

    bool we_opened;
    if (!socket_ensure(&we_opened)) return GEMDOS_EDRVNR;
    if (!s_mounted && !do_mount()) { socket_release(we_opened); return GEMDOS_EDRVNR; }

    uint8_t *p = prep(s_session, TNFS_CMD_RMDIR);
    size_t plen = strlen(tnfs_path);
    memcpy(p, tnfs_path, plen + 1u);
    p += plen + 1u;
    uint16_t tx_len = (uint16_t)(p - s_tx_buf);
    uint16_t rlen;
    bool ok = send_recv(tx_len, &rlen, TNFS_CMD_RMDIR, s_session);
    socket_release(we_opened);

    if (!ok || s_rx_buf[4] != TNFS_OK)
        return tnfs_err_to_gemdos(rlen >= 5u ? s_rx_buf[4] : 0xFFu);

    s_cache_valid = false;
    return GEMDOS_EOK;
}

int16_t fs_rename(const char *old_path, const char *new_path)
{
    char old_tnfs[MAX_FOLDER_LENGTH];
    char new_tnfs[MAX_FOLDER_LENGTH];
    relpath_to_tnfs(old_path, old_tnfs, sizeof(old_tnfs));
    relpath_to_tnfs(new_path, new_tnfs, sizeof(new_tnfs));

    bool we_opened;
    if (!socket_ensure(&we_opened)) return GEMDOS_EDRVNR;
    if (!s_mounted && !do_mount()) { socket_release(we_opened); return GEMDOS_EDRVNR; }

    uint8_t *p = prep(s_session, TNFS_CMD_RENAME);
    size_t olen = strlen(old_tnfs);
    memcpy(p, old_tnfs, olen + 1u);
    p += olen + 1u;
    size_t nlen = strlen(new_tnfs);
    memcpy(p, new_tnfs, nlen + 1u);
    p += nlen + 1u;
    uint16_t tx_len = (uint16_t)(p - s_tx_buf);
    uint16_t rlen;
    bool ok2 = send_recv(tx_len, &rlen, TNFS_CMD_RENAME, s_session);
    socket_release(we_opened);

    if (!ok2 || s_rx_buf[4] != TNFS_OK)
        return tnfs_err_to_gemdos(rlen >= 5u ? s_rx_buf[4] : 0xFFu);

    s_cache_valid = false;
    return GEMDOS_EOK;
}

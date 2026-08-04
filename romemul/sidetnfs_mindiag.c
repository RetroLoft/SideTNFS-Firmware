/**
 * File: sidetnfs_mindiag.c
 * Description: See include/sidetnfs_mindiag.h. Minimal, fixed-size
 * diagnostic state for the XELITE.IT copy-failure investigation --
 * plain static variables only, updated by simple assignments/increments,
 * dumped to COPYTRACE.TXT only from the SELECT-button edge-handler.
 */
#include "include/sidetnfs_mindiag.h"

#if SIDETNFS_ENABLE_MINDIAG

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "f_util.h"

typedef struct
{
    uint32_t dispatch_fopen;
    uint32_t dispatch_fread;
    uint32_t dispatch_fclose;
    uint32_t handler_fopen;
    uint32_t handler_fread;
    uint32_t handler_fclose;
    uint8_t last_op;
    uint16_t last_gemdos_handle;
    uint8_t last_tnfs_handle;
    uint32_t last_requested_bytes;
    uint32_t last_actual_bytes;
    uint8_t last_tnfs_rc;
    int32_t last_pico_result;
    int32_t active_bindings;
    bool xelite_it_seen;
    uint32_t xelite_it_dta_size;
} SidetnfsMindiagState;

static SidetnfsMindiagState s_mindiag = {0};

void sidetnfs_mindiag_note_dispatch(uint8_t op)
{
    switch (op)
    {
    case SIDETNFS_MINDIAG_OP_FOPEN:
        s_mindiag.dispatch_fopen++;
        break;
    case SIDETNFS_MINDIAG_OP_FREAD:
        s_mindiag.dispatch_fread++;
        break;
    case SIDETNFS_MINDIAG_OP_FCLOSE:
        s_mindiag.dispatch_fclose++;
        break;
    default:
        break;
    }
}

void sidetnfs_mindiag_note_fopen(uint16_t gemdos_handle, uint8_t tnfs_handle, uint8_t tnfs_rc, int32_t pico_result)
{
    s_mindiag.handler_fopen++;
    s_mindiag.last_op = SIDETNFS_MINDIAG_OP_FOPEN;
    s_mindiag.last_gemdos_handle = gemdos_handle;
    s_mindiag.last_tnfs_handle = tnfs_handle;
    s_mindiag.last_requested_bytes = 0;
    s_mindiag.last_actual_bytes = 0;
    s_mindiag.last_tnfs_rc = tnfs_rc;
    s_mindiag.last_pico_result = pico_result;
    if (pico_result == 0)
    {
        s_mindiag.active_bindings++;
    }
}

void sidetnfs_mindiag_note_fread(uint16_t gemdos_handle, uint8_t tnfs_handle, uint32_t requested_bytes,
                                    uint32_t actual_bytes, uint8_t tnfs_rc, int32_t pico_result)
{
    s_mindiag.handler_fread++;
    s_mindiag.last_op = SIDETNFS_MINDIAG_OP_FREAD;
    s_mindiag.last_gemdos_handle = gemdos_handle;
    s_mindiag.last_tnfs_handle = tnfs_handle;
    s_mindiag.last_requested_bytes = requested_bytes;
    s_mindiag.last_actual_bytes = actual_bytes;
    s_mindiag.last_tnfs_rc = tnfs_rc;
    s_mindiag.last_pico_result = pico_result;
}

void sidetnfs_mindiag_note_fclose(uint16_t gemdos_handle, uint8_t tnfs_handle, int32_t pico_result,
                                     bool binding_existed)
{
    s_mindiag.handler_fclose++;
    s_mindiag.last_op = SIDETNFS_MINDIAG_OP_FCLOSE;
    s_mindiag.last_gemdos_handle = gemdos_handle;
    s_mindiag.last_tnfs_handle = tnfs_handle;
    s_mindiag.last_requested_bytes = 0;
    s_mindiag.last_actual_bytes = 0;
    s_mindiag.last_tnfs_rc = 0;
    s_mindiag.last_pico_result = pico_result;
    if (pico_result == 0 && binding_existed)
    {
        s_mindiag.active_bindings--;
    }
}

void sidetnfs_mindiag_note_dta_entry(const char *name, uint32_t size)
{
    if (s_mindiag.xelite_it_seen || name == NULL)
    {
        return;
    }
    // One bounded comparison, fixed length (sizeof "XELITE.IT" includes the
    // terminating NUL) -- never a loop, never a runtime-computed length.
    if (strncmp(name, "XELITE.IT", sizeof("XELITE.IT")) == 0)
    {
        s_mindiag.xelite_it_seen = true;
        s_mindiag.xelite_it_dta_size = size;
    }
}

void sidetnfs_mindiag_dump_to_file(const char *hd_folder)
{
    if (hd_folder == NULL)
    {
        return;
    }
    char path[160];
    int n = snprintf(path, sizeof(path), "%s/COPYTRACE.TXT", hd_folder);
    if (n <= 0 || (size_t)n >= sizeof(path))
    {
        return;
    }

    FIL file;
    FRESULT fr = f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK)
    {
        return; // stay silent, no crash
    }

    // Every format string below has a proven worst case (all fields at
    // their maximum width) well under this buffer's size -- the clamp in
    // the write helper is a hard backstop, not something any of these
    // lines is expected to ever hit.
    char line[128];
    UINT written;
    int len;

    len = snprintf(line, sizeof(line), "SIDETNFS MINDIAG\r\n");
    if (len > 0)
    {
        size_t to_write = (size_t)len;
        if (to_write >= sizeof(line))
        {
            to_write = sizeof(line) - 1; // never write more than the buffer holds
        }
        f_write(&file, line, (UINT)to_write, &written);
    }

    len = snprintf(line, sizeof(line), "dispatch fopen=%lu fread=%lu fclose=%lu\r\n",
                    (unsigned long)s_mindiag.dispatch_fopen, (unsigned long)s_mindiag.dispatch_fread,
                    (unsigned long)s_mindiag.dispatch_fclose);
    if (len > 0)
    {
        size_t to_write = (size_t)len;
        if (to_write >= sizeof(line))
        {
            to_write = sizeof(line) - 1;
        }
        f_write(&file, line, (UINT)to_write, &written);
    }

    len = snprintf(line, sizeof(line), "handler fopen=%lu fread=%lu fclose=%lu\r\n",
                    (unsigned long)s_mindiag.handler_fopen, (unsigned long)s_mindiag.handler_fread,
                    (unsigned long)s_mindiag.handler_fclose);
    if (len > 0)
    {
        size_t to_write = (size_t)len;
        if (to_write >= sizeof(line))
        {
            to_write = sizeof(line) - 1;
        }
        f_write(&file, line, (UINT)to_write, &written);
    }

    len = snprintf(line, sizeof(line), "last op=%u gemdos_handle=%u tnfs_handle=%u\r\n", (unsigned)s_mindiag.last_op,
                    (unsigned)s_mindiag.last_gemdos_handle, (unsigned)s_mindiag.last_tnfs_handle);
    if (len > 0)
    {
        size_t to_write = (size_t)len;
        if (to_write >= sizeof(line))
        {
            to_write = sizeof(line) - 1;
        }
        f_write(&file, line, (UINT)to_write, &written);
    }

    len = snprintf(line, sizeof(line), "last requested=%lu actual=%lu tnfs_rc=%u pico_result=%ld\r\n",
                    (unsigned long)s_mindiag.last_requested_bytes, (unsigned long)s_mindiag.last_actual_bytes,
                    (unsigned)s_mindiag.last_tnfs_rc, (long)s_mindiag.last_pico_result);
    if (len > 0)
    {
        size_t to_write = (size_t)len;
        if (to_write >= sizeof(line))
        {
            to_write = sizeof(line) - 1;
        }
        f_write(&file, line, (UINT)to_write, &written);
    }

    len = snprintf(line, sizeof(line), "active_bindings=%ld\r\n", (long)s_mindiag.active_bindings);
    if (len > 0)
    {
        size_t to_write = (size_t)len;
        if (to_write >= sizeof(line))
        {
            to_write = sizeof(line) - 1;
        }
        f_write(&file, line, (UINT)to_write, &written);
    }

    if (s_mindiag.xelite_it_seen)
    {
        len = snprintf(line, sizeof(line), "xelite_it_dta_size=%lu\r\n", (unsigned long)s_mindiag.xelite_it_dta_size);
    }
    else
    {
        len = snprintf(line, sizeof(line), "xelite_it_dta_size=NOT_SEEN\r\n");
    }
    if (len > 0)
    {
        size_t to_write = (size_t)len;
        if (to_write >= sizeof(line))
        {
            to_write = sizeof(line) - 1;
        }
        f_write(&file, line, (UINT)to_write, &written);
    }

    f_close(&file);
}

#endif // SIDETNFS_ENABLE_MINDIAG
// When SIDETNFS_ENABLE_MINDIAG is 0, every function declared in
// sidetnfs_mindiag.h is a static inline no-op right there in the header --
// nothing to define in this .c file for that case, and no risk of a
// conflicting definition with this file's own real bodies above.

#include "include/gemdrvemul.h"
#include "include/fs_backend.h"
#include "include/net_wifi.h"
#include "include/net_test.h"
#include "include/net_udp_test.h"
#include "include/tnfs_test.h"
#include "include/log_event.h"

// ─── Atari GEMDRIVE protocol layer ───────────────────────────────────────────
//   Handles the ROM3 command/response protocol with the Atari 68k firmware.
//   All filesystem access goes through the fs_backend.h interface below.
//
//   Atari GEMDRIVE protocol layer  (this file)
//       → filesystem backend interface  (fs_backend.h)
//           → memory backend            (fs_memory.c)   ← active now
//           → TNFS backend              (fs_tnfs.c)     ← future

// IRQ-visible state: command pending + payload pointer
static volatile uint16_t active_command_id = 0xFFFF;
static uint16_t *payloadPtr = NULL;
static uint16_t payload_size_received = 0;
static uint32_t random_token;

// Current directory path (backslash-separated, Atari style)
static char dpath_string[MAX_FOLDER_LENGTH] = {'\\', '\0'};

// Drive letter assigned to this GEMDRIVE
#define DRIVE_LETTER 'N'
#define DRIVE_NUMBER 13  // A=0, B=1, C=2, D=3, ... N=13

// ── Open file descriptors ─────────────────────────────────────────────────────

#define MAX_OPEN_FILES 16
typedef struct {
    bool      in_use;
    FsHandle *handle;
} OpenFile;
static OpenFile open_files[MAX_OPEN_FILES];

// Static write buffer and pending state for the two-phase FWRITE protocol.
static uint8_t  s_write_buf[DEFAULT_FWRITE_BUFFER_SIZE];
static uint16_t s_write_pending_fd    = 0;
static uint32_t s_write_pending_count = 0;

// ── DTA search slots ──────────────────────────────────────────────────────────

#define MAX_DTA_SLOTS 16
typedef struct {
    uint32_t key;       // ndta Atari address (0 = empty slot)
    int      dir_index; // next index to pass to fs_list_dir()
    uint32_t attribs;   // requested search attributes
    char     path[MAX_FOLDER_LENGTH];    // search directory (backslash-separated)
    char     pattern[MAX_FOLDER_LENGTH]; // filename wildcard pattern (uppercase)
} DTASlot;
static DTASlot dta_slots[MAX_DTA_SLOTS];

// ── Helpers ───────────────────────────────────────────────────────────────────

static inline void generate_random_token_seed(const TransmissionProtocol *protocol)
{
    random_token = ((*((uint32_t *)protocol->payload) & 0xFFFF0000) >> 16) |
                   ((*((uint32_t *)protocol->payload) & 0x0000FFFF) << 16);
}

static void __not_in_flash_func(write_random_token)(uint32_t mem)
{
    *((volatile uint32_t *)(mem + GEMDRVEMUL_RANDOM_TOKEN)) = random_token;
}

static void set_shared_var(uint32_t idx, uint32_t val, uint32_t mem)
{
    *((volatile uint16_t *)(mem + GEMDRVEMUL_SHARED_VARIABLES + (idx * 4) + 2)) = val & 0xFFFF;
    *((volatile uint16_t *)(mem + GEMDRVEMUL_SHARED_VARIABLES + (idx * 4)))     = val >> 16;
}

// Convert ASCII string to uppercase in-place (dst may equal src).
static void str_upper(const char *src, char *dst, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = (src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i];
    dst[i] = '\0';
}

// Split "C:\DIR\PATTERN" → dir="\DIR\", pattern="PATTERN" (uppercase).
static void split_fspec(const char *fspec, char *dir, char *pattern)
{
    const char *last_slash = NULL;
    for (const char *p = fspec; *p; p++)
        if (*p == '\\' || *p == '/') last_slash = p;

    if (last_slash) {
        size_t dlen = (size_t)(last_slash - fspec) + 1;
        if (dlen == 0) dlen = 1;
        if (dlen >= MAX_FOLDER_LENGTH) dlen = MAX_FOLDER_LENGTH - 1;
        memcpy(dir, fspec, dlen);
        dir[dlen] = '\0';
        str_upper(last_slash + 1, pattern, MAX_FOLDER_LENGTH);
    } else {
        dir[0] = '\\'; dir[1] = '\0';
        str_upper(fspec, pattern, MAX_FOLDER_LENGTH);
    }
}

// ── Atari string extraction ───────────────────────────────────────────────────

// The 68k sends strings big-endian over a 16-bit bus; each uint16_t in the Pico
// SRAM has its bytes byte-swapped relative to ASCII order.  This helper unpacks
// word by word, stops at the first NUL byte, and always NUL-terminates dst.
// max_words: hard upper bound on source words to read (payload safety bound).
static void copy_atari_str(const uint16_t *src, int max_words,
                            char *dst, int dst_size)
{
    int out = 0;
    for (int w = 0; w < max_words; w++) {
        char hi = (char)((src[w] >> 8) & 0xFF);
        char lo = (char)(src[w] & 0xFF);
        if (hi == '\0' || out >= dst_size - 1) break;
        dst[out++] = hi;
        if (lo == '\0' || out >= dst_size - 1) break;
        dst[out++] = lo;
    }
    dst[out] = '\0';
}

// Words consumed before the string argument in every string-bearing command:
// handle_protocol_command skips 2 words (random token), then each handler
// advances payloadPtr by 6 words of header fields.  Total = 8.
#define CMD_STR_OFFSET_WORDS 8

static inline int str_max_words(uint16_t psize)
{
    int avail = (int)(psize / 2) - CMD_STR_OFFSET_WORDS;
    if (avail <= 0) return 0;
    return avail < MAX_FOLDER_LENGTH / 2 ? avail : MAX_FOLDER_LENGTH / 2;
}

// ── DTA slot management ───────────────────────────────────────────────────────

static DTASlot *find_dta(uint32_t key)
{
    if (key == 0) return NULL;  // 0 is the empty-slot sentinel
    for (int i = 0; i < MAX_DTA_SLOTS; i++)
        if (dta_slots[i].key == key) return &dta_slots[i];
    return NULL;
}

static DTASlot *alloc_dta(uint32_t key)
{
    if (key == 0) return NULL;  // refuse to allocate the sentinel value
    DTASlot *s = find_dta(key);
    if (s) return s;
    for (int i = 0; i < MAX_DTA_SLOTS; i++) {
        if (!dta_slots[i].key) {
            memset(&dta_slots[i], 0, sizeof(DTASlot));
            dta_slots[i].key = key;
            return &dta_slots[i];
        }
    }
    return NULL;
}

static void release_dta(uint32_t key)
{
    if (key == 0) return;  // nothing to release for the sentinel
    DTASlot *s = find_dta(key);
    if (s) memset(s, 0, sizeof(DTASlot));
}

static int count_dta(void)
{
    int n = 0;
    for (int i = 0; i < MAX_DTA_SLOTS; i++)
        if (dta_slots[i].key) n++;
    return n;
}

// ── Open-file management ──────────────────────────────────────────────────────

static int alloc_fd(FsHandle *h)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_files[i].in_use) {
            open_files[i].in_use  = true;
            open_files[i].handle  = h;
            return FIRST_FILE_DESCRIPTOR + i;
        }
    }
    return -1;
}

static OpenFile *get_fd(uint16_t fd)
{
    int slot = (int)fd - FIRST_FILE_DESCRIPTOR;
    if (slot < 0 || slot >= MAX_OPEN_FILES || !open_files[slot].in_use)
        return NULL;
    return &open_files[slot];
}

static void free_fd(uint16_t fd)
{
    int slot = (int)fd - FIRST_FILE_DESCRIPTOR;
    if (slot >= 0 && slot < MAX_OPEN_FILES)
        open_files[slot].in_use = false;
}

// ── DTA transfer area fill ────────────────────────────────────────────────────
// Writes a filesystem entry into the 44-byte DTA transfer area in ROM3 shared
// memory so the 68k ROM driver can copy it to the Atari's DTA buffer.

static void write_dta_entry(uint32_t mem, const FsEntry *e)
{
    memset((void *)(mem + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);

    // Internal name field (bytes 0-11)
    for (int i = 0; i < 12 && e->name[i]; i++)
        *((volatile uint8_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + i)) = (uint8_t)e->name[i];

    // d_attr (byte 20) + d_attrib (byte 21) — write then swap word to match Atari byte order
    *((volatile uint8_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 20)) = e->attr;
    *((volatile uint8_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 21)) = e->attr;
    CHANGE_ENDIANESS_BLOCK16(mem + GEMDRVEMUL_DTA_TRANSFER + 20, 2);

    // d_time (bytes 22-23), d_date (bytes 24-25) — raw FAT uint16
    *((volatile uint16_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 22)) = e->time;
    *((volatile uint16_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 24)) = e->date;

    // d_length (bytes 26-29) — swapped longword
    uint32_t swapped_len = ((e->size << 16) & 0xFFFF0000) | ((e->size >> 16) & 0xFFFF);
    uint16_t *lptr = (uint16_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 26);
    lptr[0] = swapped_len & 0xFFFF;
    lptr[1] = (swapped_len >> 16) & 0xFFFF;

    // d_fname (bytes 30-43) — write then endian-swap each word pair
    for (int i = 0; i < 14 && e->name[i]; i++)
        *((volatile uint8_t *)(mem + GEMDRVEMUL_DTA_TRANSFER + 30 + i)) = (uint8_t)e->name[i];
    CHANGE_ENDIANESS_BLOCK16(mem + GEMDRVEMUL_DTA_TRANSFER + 30, 14);
}

// ── IRQ handler (called from DMA IRQ context) ─────────────────────────────────

static inline void __not_in_flash_func(handle_protocol_command)(const TransmissionProtocol *protocol)
{
    if (active_command_id == 0xFFFF)
    {
        // Skip 4-byte random token at start of payload
        payloadPtr = (uint16_t *)protocol->payload + 2;
        payload_size_received = protocol->payload_size;
        generate_random_token_seed(protocol);
        active_command_id = protocol->command_id;
    }
}

void __not_in_flash_func(gemdrvemul_dma_irq_handler_lookup_callback)(void)
{
    uint32_t addr = (uint32_t)dma_hw->ch[lookup_data_rom_dma_channel].al3_read_addr_trig;
    if (addr >= ROM3_START_ADDRESS)
        parse_protocol((uint16_t)(addr & 0xFFFF), handle_protocol_command);
    dma_hw->ints1 = 1u << lookup_data_rom_dma_channel;
}

// ── Main command loop ─────────────────────────────────────────────────────────

void init_gemdrvemul(void)
{
    LOG("SIDETNFS GEMDRIVE emulator starting (M2 backend interface)\n");

    dpath_string[0] = '\\';
    dpath_string[1] = '\0';

    active_command_id = 0xFFFF;
    memset(open_files, 0, sizeof(open_files));
    memset(dta_slots,  0, sizeof(dta_slots));

    // ── Filesystem backend initialisation ─────────────────────────────────────
    fs_init();

    uint32_t mem  = ROM3_START_ADDRESS;  // shared memory (ROM3 bank)
    uint32_t code = ROM4_START_ADDRESS;  // firmware code  (ROM4 bank)

    // Zero shared memory
    memset((void *)mem, 0, 0x5000);

    // Pre-populate status fields expected by the 68k driver
    *((volatile uint32_t *)(mem + GEMDRVEMUL_RTC_STATUS))     = 0x0;
    *((volatile uint32_t *)(mem + GEMDRVEMUL_NETWORK_STATUS)) = 0x0;
    *((volatile uint32_t *)(mem + GEMDRVEMUL_RTC_ENABLED))    = 0x0;
    *((volatile uint32_t *)(mem + GEMDRVEMUL_RTC_Y2K_PATCH))  = 0x0;
    WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_TIMEOUT_SEC, 0);

    // Shared variables expected by the 68k driver.
    //
    // DRIVE_LETTER: 68k CMP.B at the ODD address (slot+3) reads the LOW byte of the
    // uint16_t stored at slot+2. Low byte of 0x0044 = 0x44 = 'D'. Write val as-is.
    //
    // DRIVE_NUMBER: proven via disassembly of firmware/firmware_gemdrvemul.c that TWO
    // independent 68k consumers read this slot at different widths/addresses:
    //   - 13 call sites (the GEMDOS function dispatcher's "is this request for my
    //     drive?" check, used by every intercepted function) do CMP.B at slot+2
    //     (the EVEN address) = HIGH byte of the uint16_t at slot+2. Requires
    //     DRIVE_NUMBER << 8 written to slot+2.
    //   - 2 call sites in the hard-disk init routine — confirmed to be exactly
    //     "_drvbits |= (1 << DRIVE_NUMBER); Dsetdrv(DRIVE_NUMBER);" (TRAP #1 with
    //     GEMDOS selector 0x0E = Dsetdrv right after the word push) — did a MOVE.W
    //     of the SAME slot+2 address, needing the plain value DRIVE_NUMBER.
    // A byte read returning the high byte of a word AND a word read returning that
    // same word's full value cannot both equal DRIVE_NUMBER for any DRIVE_NUMBER > 0
    // — mathematically impossible from one 16-bit slot. This is why C:/D: only
    // "worked": they're already registered via the real harddisk partition table,
    // independent of this mechanism, while N: (and any drive without that table
    // entry) never actually got a _drvbits bit or a Dsetdrv call.
    //
    // Fix: firmware/firmware_gemdrvemul.c line 64 was binary-patched (operand-only,
    // no opcode/length change, no branch displacement touched) to redirect just the
    // 2 word-read instructions from slot+2 to slot+0 — the high half of the same
    // 4-byte shared-var slot, which no other 68k code reads. slot+0 now carries the
    // plain DRIVE_NUMBER value for the word reads; slot+2 keeps the existing <<8
    // byte-routing trick for the 13 dispatcher checks. Both consumers now get a
    // correct, independent view of DRIVE_NUMBER from a single Pico-side write.
    uint32_t drive_number_val = ((uint32_t)DRIVE_NUMBER << 16) | ((uint32_t)DRIVE_NUMBER << 8);
    set_shared_var(SHARED_VARIABLE_FIRST_FILE_DESCRIPTOR, FIRST_FILE_DESCRIPTOR, mem);
    set_shared_var(SHARED_VARIABLE_DRIVE_LETTER, (uint32_t)DRIVE_LETTER, mem);
    set_shared_var(SHARED_VARIABLE_DRIVE_NUMBER, drive_number_val, mem);
    set_shared_var(SHARED_VARIABLE_BUFFER_TYPE, 0, mem);
    set_shared_var(SHARED_VARIABLE_FAKE_FLOPPY,  0, mem);
#ifdef SIDETNFS_DEBUG
    {
        // Byte layout: ARM LE stores uint16_t V at slot+2 as byte[slot+2]=V_low, byte[slot+3]=V_high.
        // 68k byte at EVEN addr slot+2 = V_high = Pico byte[slot+3].
        // 68k byte at ODD  addr slot+3 = V_low  = Pico byte[slot+2].
        uint8_t *dl = (uint8_t *)(mem + GEMDRVEMUL_SHARED_VARIABLES + SHARED_VARIABLE_DRIVE_LETTER * 4);
        uint8_t *dn = (uint8_t *)(mem + GEMDRVEMUL_SHARED_VARIABLES + SHARED_VARIABLE_DRIVE_NUMBER * 4);
        uint16_t dn_word_slot0 = *((volatile uint16_t *)(mem + GEMDRVEMUL_SHARED_VARIABLES + SHARED_VARIABLE_DRIVE_NUMBER * 4));
        LOG("[INIT] DRIVE_LETTER slot raw bytes: +0=0x%02x +1=0x%02x +2=0x%02x +3=0x%02x\n",
            dl[0], dl[1], dl[2], dl[3]);
        LOG("[INIT]   68k CMP.B at slot+3(odd) reads 0x%02x, expect 0x%02x='%c'\n",
            dl[2], (uint8_t)DRIVE_LETTER, DRIVE_LETTER);
        LOG("[INIT] DRIVE_NUMBER slot raw bytes: +0=0x%02x +1=0x%02x +2=0x%02x +3=0x%02x\n",
            dn[0], dn[1], dn[2], dn[3]);
        LOG("[INIT]   68k CMP.B at slot+2(even, dispatcher x13) reads 0x%02x, expect %d\n",
            dn[3], DRIVE_NUMBER);
        LOG("[INIT]   68k MOVE.W at slot+0(patched, hard-disk init x2: _drvbits/Dsetdrv) reads 0x%04x, expect %d\n",
            dn_word_slot0, DRIVE_NUMBER);
    }
#endif

    DPRINTF("Entering GEMDRIVE command loop...\n");

#ifdef DIAGNOSTIC_PASSTHROUGH
    *((volatile uint16_t *)(code + 0x0806)) = 0x4E71;
    *((volatile uint16_t *)(code + 0x0828)) = 0x4E71;
    LOG("DIAGNOSTIC: gemdrive_trap patched → pure pass-through (both variants)\n");
#endif

    static uint32_t seed_counter = 1;

    while (true)
    {
        *((volatile uint32_t *)(mem + GEMDRVEMUL_RANDOM_TOKEN_SEED)) = ++seed_counter;
        tight_loop_contents();

#ifdef SIDETNFS_DEBUG
        // Print a status banner every time USB serial connects or reconnects.
        // Checked once per second to avoid overhead; fires on every DTR rising
        // edge so the user sees current state whenever they open the terminal,
        // regardless of how late in the Atari boot sequence that happens.
        {
            static bool     prev_usb      = false;
            static uint32_t usb_check_ms  = 0;
            uint32_t now_ms = to_ms_since_boot(get_absolute_time());
            if (now_ms - usb_check_ms >= 1000) {
                usb_check_ms = now_ms;
                bool cur_usb = stdio_usb_connected();
                if (cur_usb && !prev_usb) {
                    LOG("--- SIDETNFS GEMDRIVE ready, D: active ---\n");
                    net_wifi_log_status();
                    net_test_log_result();
                    net_udp_test_log_result();
                    tnfs_test_log_result();
                }
                prev_usb = cur_usb;
            }
        }
#endif

        // Check WiFi status once per second; logs connect/fail/timeout.
        // No-op after WiFi is resolved (connected or failed).
        net_wifi_poll();
#ifdef SIDETNFS_DEBUG
        net_udp_test_poll();
        tnfs_test_poll();
#endif

        uint16_t cmd = active_command_id;
        if (cmd == 0xFFFF) {
            log_flush();
            continue;
        }

        log_event(EVT_DISPATCH, cmd, 0, 0);

        // Guard: all valid commands are APP_GEMDRVEMUL (0x04xx).  Anything
        // else is a protocol-framing artifact (e.g. a payload word such as
        // the DTA address that slipped into the command slot).  Writing a
        // fake token here would ack a command the 68k never intended to
        // send and corrupt its state machine.  Instead, clear the slot and
        // let the 68k's internal polling timeout trigger a natural resync.
        if ((cmd & 0xFF00) != (APP_GEMDRVEMUL << 8)) {
            log_event(EVT_FRAMING, cmd, payload_size_received, 0);
            active_command_id = 0xFFFF;
            continue;
        }

        switch (cmd)
        {
        // ── Handshake ──────────────────────────────────────────────────────
        case GEMDRVEMUL_PING:
        {
            *((volatile uint16_t *)(mem + GEMDRVEMUL_PING_STATUS)) = 0x1;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_CANCEL:
        {
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Vector patching ────────────────────────────────────────────────
        case GEMDRVEMUL_SAVE_VECTORS:
        {
            uint32_t old_addr  = ((uint32_t)payloadPtr[0] << 16) | payloadPtr[1];
            payloadPtr += 2;
            uint32_t xbra_addr = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            uint32_t xbra_offset = xbra_addr - ATARI_ROM4_START_ADDRESS;
            if (xbra_offset < 0x10000) {
                *((volatile uint16_t *)(code + xbra_offset))     = old_addr & 0xFFFF;
                *((volatile uint16_t *)(code + xbra_offset + 2)) = old_addr >> 16;
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_SAVE_XBIOS_VECTOR:
        {
            uint32_t xbios_old = ((uint32_t)payloadPtr[0] << 16) | payloadPtr[1];
            *((volatile uint16_t *)(mem + GEMDRVEMUL_OLD_XBIOS_TRAP))     = xbios_old & 0xFFFF;
            *((volatile uint16_t *)(mem + GEMDRVEMUL_OLD_XBIOS_TRAP + 2)) = xbios_old >> 16;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_SHOW_VECTOR_CALL:
        {
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Reentry guards ─────────────────────────────────────────────────
        case GEMDRVEMUL_REENTRY_LOCK:
        {
            *((volatile uint16_t *)(mem + GEMDRVEMUL_REENTRY_TRAP)) = 0xFFFF;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_REENTRY_UNLOCK:
        {
            *((volatile uint16_t *)(mem + GEMDRVEMUL_REENTRY_TRAP)) = 0x0;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_REENTRY_XBIOS_LOCK:
        {
            *((volatile uint16_t *)(mem + GEMDRVEMUL_RTC_XBIOS_REENTRY_TRAP)) = 0xFFFF;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_REENTRY_XBIOS_UNLOCK:
        {
            *((volatile uint16_t *)(mem + GEMDRVEMUL_RTC_XBIOS_REENTRY_TRAP)) = 0x0;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Shared variables ───────────────────────────────────────────────
        case GEMDRVEMUL_SET_SHARED_VAR:
        {
            uint32_t idx = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            payloadPtr += 2;
            uint32_t val = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            set_shared_var(idx, val, mem);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Drive info ─────────────────────────────────────────────────────
        case GEMDRVEMUL_DGETDRV_CALL:
        {
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_DFREE_CALL:
        {
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DFREE_STRUCT,      256);
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DFREE_STRUCT + 4,  512);
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DFREE_STRUCT + 8,  512);
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DFREE_STRUCT + 12, 1);
            *((volatile uint32_t *)(mem + GEMDRVEMUL_DFREE_STATUS)) = GEMDOS_EOK;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Directory path ─────────────────────────────────────────────────
        case GEMDRVEMUL_DGETPATH_CALL:
        {
            // TOS Dgetpath returns the current path WITHOUT the leading backslash.
            // Root ("\\") becomes "" (empty string). "\\CONFIG" becomes "CONFIG".
            const char *p = dpath_string;
            if (*p == '\\') p++;
            char tmp[MAX_FOLDER_LENGTH];
            strncpy(tmp, p, MAX_FOLDER_LENGTH - 1);
            tmp[MAX_FOLDER_LENGTH - 1] = '\0';
            log_event(EVT_DGETPATH, log_pack4(tmp), 0, 0);
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(tmp, mem + GEMDRVEMUL_DEFAULT_PATH, MAX_FOLDER_LENGTH);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_DSETPATH_CALL:
        {
            payloadPtr += 6;
            char new_path[MAX_FOLDER_LENGTH] = {0};
            copy_atari_str(payloadPtr, str_max_words(payload_size_received),
                           new_path, sizeof(new_path));
            log_event(EVT_DSETPATH_RX, log_pack4(new_path), 0, 0);

            // Strip drive letter if present ("D:\CONFIG" → "\CONFIG")
            if (new_path[1] == ':')
                memmove(new_path, new_path + 2, strlen(new_path) - 1);

            if (new_path[0] == '\0') {
                dpath_string[0] = '\\'; dpath_string[1] = '\0';
            } else {
                strncpy(dpath_string, new_path, MAX_FOLDER_LENGTH - 1);
                dpath_string[MAX_FOLDER_LENGTH - 1] = '\0';
            }
            log_event(EVT_DSETPATH_OK, log_pack4(dpath_string), 0, 0);
            *((volatile uint16_t *)(mem + GEMDRVEMUL_SET_DPATH_STATUS)) = GEMDOS_EOK;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── DTA management ─────────────────────────────────────────────────
        case GEMDRVEMUL_FSETDTA_CALL:
        {
            uint32_t ndta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            if (!find_dta(ndta)) alloc_dta(ndta);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_DTA_EXIST_CALL:
        {
            uint32_t ndta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            if (ndta == 0) {
                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DTA_EXIST, 0);
                write_random_token(mem);
                active_command_id = 0xFFFF;
                break;
            }
            bool exists = find_dta(ndta) != NULL;
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DTA_EXIST, exists ? ndta : 0);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_DTA_RELEASE_CALL:
        {
            uint32_t ndta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            if (ndta == 0) {
                write_random_token(mem);
                active_command_id = 0xFFFF;
                break;
            }
            release_dta(ndta);
            memset((void *)(mem + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_DTA_RELEASE, (uint32_t)count_dta());
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Directory search ───────────────────────────────────────────────
        case GEMDRVEMUL_FSFIRST_CALL:
        {
            uint32_t ndta    = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            payloadPtr += 2;
            uint32_t attribs = payloadPtr[0];
            payloadPtr += 2;
            payloadPtr += 2;  // fspec Atari address — skip
            char raw_fspec[MAX_FOLDER_LENGTH] = {0};
            copy_atari_str(payloadPtr, str_max_words(payload_size_received),
                           raw_fspec, sizeof(raw_fspec));
            log_event(EVT_FSFIRST, log_pack4(raw_fspec), attribs, 0);

            char fspec[MAX_FOLDER_LENGTH];
            if (raw_fspec[1] == ':') strncpy(fspec, raw_fspec + 2, MAX_FOLDER_LENGTH - 1);
            else                     strncpy(fspec, raw_fspec,     MAX_FOLDER_LENGTH - 1);
            fspec[MAX_FOLDER_LENGTH - 1] = '\0';

            char dir[MAX_FOLDER_LENGTH], pat[MAX_FOLDER_LENGTH];
            split_fspec(fspec, dir, pat);

            DTASlot *slot = alloc_dta(ndta);
            if (!slot) {
                *((volatile int16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = GEMDOS_EINTRN;
                write_random_token(mem);
                active_command_id = 0xFFFF;
                break;
            }
            slot->attribs   = attribs;
            slot->dir_index = 0;
            strncpy(slot->path,    dir, MAX_FOLDER_LENGTH - 1);
            slot->path[MAX_FOLDER_LENGTH - 1] = '\0';
            strncpy(slot->pattern, pat, MAX_FOLDER_LENGTH - 1);
            slot->pattern[MAX_FOLDER_LENGTH - 1] = '\0';

            FsEntry entry;
            if (fs_list_dir(dir, pat, 0, &entry)) {
                slot->dir_index = 1;
                *((volatile uint16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = 0;
                write_dta_entry(mem, &entry);
            } else {
                release_dta(ndta);
                memset((void *)(mem + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);
                *((volatile int16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = GEMDOS_EFILNF;
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FSNEXT_CALL:
        {
            uint32_t ndta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            if (ndta == 0) {
                log_event(EVT_FSNEXT_NDTA0, 0, 0, 0);
                memset((void *)(mem + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);
                *((volatile int16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = GEMDOS_ENMFIL;
                write_random_token(mem);
                active_command_id = 0xFFFF;
                break;
            }
            DTASlot *slot = find_dta(ndta);
            if (!slot) {
                log_event(EVT_FSNEXT, ndta, 0xFFFFFFFFu, 0);
                memset((void *)(mem + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);
                *((volatile int16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = GEMDOS_ENMFIL;
                write_random_token(mem);
                active_command_id = 0xFFFF;
                break;
            }
            log_event(EVT_FSNEXT, ndta, (uint32_t)slot->dir_index, 0);
            FsEntry entry;
            if (fs_list_dir(slot->path, slot->pattern, slot->dir_index, &entry)) {
                slot->dir_index++;
                *((volatile uint16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = 0;
                write_dta_entry(mem, &entry);
            } else {
                release_dta(ndta);
                memset((void *)(mem + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);
                *((volatile int16_t *)(mem + GEMDRVEMUL_DTA_F_FOUND)) = GEMDOS_ENMFIL;
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── File open/close/seek/read ──────────────────────────────────────
        case GEMDRVEMUL_FOPEN_CALL:
        {
            uint16_t mode = payloadPtr[0];
            payloadPtr += 6;  // skip mode + 5 address/padding words
            char filename[MAX_FOLDER_LENGTH] = {0};
            copy_atari_str(payloadPtr, str_max_words(payload_size_received),
                           filename, sizeof(filename));
            log_event(EVT_FOPEN, log_pack4(filename), mode, 0);

            // Strip drive letter and leading path separator
            char *fptr = filename;
            if (fptr[1] == ':') fptr += 2;
            if (*fptr == '\\' || *fptr == '/') fptr++;
            char upper[MAX_FOLDER_LENGTH];
            str_upper(fptr, upper, MAX_FOLDER_LENGTH);

            {
                int16_t err = GEMDOS_EFILNF;
                FsHandle *h = fs_open(upper, mode, &err);
                if (!h) {
                    WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FOPEN_HANDLE, (uint32_t)(int32_t)err);
                } else {
                    int fd = alloc_fd(h);
                    if (fd < 0) {
                        fs_close(h);
                        WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FOPEN_HANDLE, (uint32_t)(int32_t)GEMDOS_ENHNDL);
                    } else {
                        WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FOPEN_HANDLE, (uint32_t)fd);
                    }
                }
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FCLOSE_CALL:
        {
            uint16_t fd = payloadPtr[0];
            OpenFile *f = get_fd(fd);
            if (!f) {
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FCLOSE_STATUS)) = GEMDOS_EIHNDL;
            } else {
                fs_close(f->handle);
                free_fd(fd);
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FCLOSE_STATUS)) = GEMDOS_EOK;
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // GEMDRVEMUL_FWRITE_CALL (0x0440) is never sent by the ROM — no case needed.

        case GEMDRVEMUL_FWRITE_BUFF_CALL:
        {
            // Phase 1 of the two-phase write protocol.
            // Payload layout (after token skip):
            //   [0..1]  fd              (word + padding word)
            //   [2..3]  bytes_to_write  (total for this Fwrite call — informational)
            //   [4..5]  pending_count   (bytes valid in THIS chunk, ≤ DEFAULT_FWRITE_BUFFER_SIZE)
            //   [6..]   data            (always DEFAULT_FWRITE_BUFFER_SIZE bytes sent by 68k)
            uint16_t fd = payloadPtr[0];
            payloadPtr += 2;
            payloadPtr += 2;  // skip bytes_to_write (total, informational)
            uint32_t count = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            payloadPtr += 2;
            // payloadPtr now points to the data region (1024 words = 2048 bytes)

            // Checksum: 16-bit additive sum of ALL 1024 data words, exactly as the 68k computes
            // in send_sync_write_command_to_sidecart (add.w d0, d7 for each word, 1024 iterations).
            uint16_t chk = 0;
            for (uint32_t w = 0; w < DEFAULT_FWRITE_BUFFER_SIZE / 2; w++)
                chk += payloadPtr[w];

            // Cache the valid bytes for the ACK handler to write to TNFS.
            if (count > DEFAULT_FWRITE_BUFFER_SIZE) count = DEFAULT_FWRITE_BUFFER_SIZE;
            for (uint32_t i = 0, w = 0; i < count; w++) {
                s_write_buf[i++] = (uint8_t)(payloadPtr[w] >> 8);
                if (i < count) s_write_buf[i++] = (uint8_t)(payloadPtr[w] & 0xFF);
            }
            s_write_pending_fd    = fd;
            s_write_pending_count = count;

            log_event(EVT_FWRITE, fd, count, (uint32_t)chk);

            // Signal the 68k: data received, here is our checksum.
            // WRITE_BYTES = count so the 68k's ADDA.L D2,A4 advances the data pointer correctly.
            // WRITE_CHK   = checksum so the 68k's CMP WRITE_CHK,D7 integrity check passes.
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_WRITE_BYTES, count);
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_WRITE_CHK, (uint32_t)chk);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FWRITE_ACK_CALL:
        {
            // Phase 2 of the two-phase write protocol.
            // The 68k has verified the checksum and now asks us to commit the cached chunk.
            // Payload: fd (2 words), forward_bytes (2 words) = WRITE_BYTES from BUFF response.
            uint32_t result;
            OpenFile *f = get_fd(s_write_pending_fd);
            if (!f) {
                result = (uint32_t)(int32_t)GEMDOS_EIHNDL;
            } else if (s_write_pending_count == 0) {
                result = 0;
            } else {
                result = fs_write(f->handle, s_write_buf, s_write_pending_count);
            }

            log_event(EVT_FWRITE_ACK, s_write_pending_fd, s_write_pending_count, result);
            s_write_pending_count = 0;

            // WRITE_BYTES = actual bytes written (68k reads this to accumulate total).
            // WRITE_CHK   = 0 (ready for next chunk).
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_WRITE_BYTES, result);
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_WRITE_CHK, 0);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FSEEK_CALL:
        {
            uint16_t fd     = payloadPtr[0];
            payloadPtr += 2;
            uint32_t offset = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            payloadPtr += 2;
            uint16_t whence = payloadPtr[0];
            OpenFile *f = get_fd(fd);
            if (!f) {
                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FSEEK_STATUS, (uint32_t)(int32_t)GEMDOS_EIHNDL);
            } else {
                int32_t new_pos = fs_seek(f->handle, (int32_t)offset, whence);
                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FSEEK_STATUS, (uint32_t)new_pos);
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_READ_BUFF_CALL:
        {
            // Payload layout (after token skip), matching original SidecarT:
            //   [0..1]  fd              (word + padding word)
            //   [2..3]  bytes_to_read   (total for this Fread call — informational)
            //   [4..5]  pending_bytes   (bytes to read in THIS chunk)
            uint16_t fd = payloadPtr[0];
            payloadPtr += 2;
            payloadPtr += 2;  // skip bytes_to_read (total, informational)
            uint32_t req_bytes = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            payloadPtr += 2;

            OpenFile *f = get_fd(fd);
            if (!f) {
                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_READ_BYTES, (uint32_t)(int32_t)GEMDOS_EIHNDL);
            } else {
                if (req_bytes > DEFAULT_FOPEN_READ_BUFFER_SIZE)
                    req_bytes = DEFAULT_FOPEN_READ_BUFFER_SIZE;

                uint8_t *dst = (uint8_t *)(mem + GEMDRVEMUL_READ_BUFF);

                // Zero the region before reading so padding bytes beyond n are
                // clean when CHANGE_ENDIANESS_BLOCK16 operates over the full
                // req_bytes range, matching original SidecarT behaviour.
                uint32_t swap = req_bytes + (req_bytes & 1u);
                if (req_bytes < DEFAULT_FOPEN_READ_BUFFER_SIZE)
                    memset(dst, 0, swap);

                uint32_t n = fs_read(f->handle, dst, req_bytes);

                // Pad the last byte if req_bytes is odd, then swap the full
                // requested region (not just n) so the 68k sees correctly
                // byte-ordered words across the entire chunk.
                if (req_bytes & 1u) dst[req_bytes] = 0;
                CHANGE_ENDIANESS_BLOCK16(mem + GEMDRVEMUL_READ_BUFF, swap);

                WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_READ_BYTES, n);
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── File attributes / date-time ────────────────────────────────────
        case GEMDRVEMUL_FATTRIB_CALL:
        {
            uint16_t flag = payloadPtr[0];
            payloadPtr += 6;
            char filename[MAX_FOLDER_LENGTH] = {0};
            copy_atari_str(payloadPtr, str_max_words(payload_size_received),
                           filename, sizeof(filename));
            char *fptr = filename;
            if (fptr[1] == ':') fptr += 2;
            if (*fptr == '\\' || *fptr == '/') fptr++;
            char upper[MAX_FOLDER_LENGTH];
            str_upper(fptr, upper, MAX_FOLDER_LENGTH);
            FsEntry entry;
            if (fs_stat(upper, &entry))
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FATTRIB_STATUS)) = entry.attr;
            else
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FATTRIB_STATUS)) = (uint16_t)(int16_t)GEMDOS_EFILNF;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FDATETIME_CALL:
        {
            uint16_t flag = payloadPtr[0];
            payloadPtr += 6;
            char filename[MAX_FOLDER_LENGTH] = {0};
            copy_atari_str(payloadPtr, str_max_words(payload_size_received),
                           filename, sizeof(filename));
            char *fptr = filename;
            if (fptr[1] == ':') fptr += 2;
            if (*fptr == '\\' || *fptr == '/') fptr++;
            char upper[MAX_FOLDER_LENGTH];
            str_upper(fptr, upper, MAX_FOLDER_LENGTH);
            FsEntry entry;
            if (fs_stat(upper, &entry)) {
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FDATETIME_DATE))   = entry.date;
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FDATETIME_TIME))   = entry.time;
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FDATETIME_STATUS)) = GEMDOS_EOK;
            } else {
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FDATETIME_STATUS)) = (uint16_t)(int16_t)GEMDOS_EFILNF;
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        // ── Write/create/delete ────────────────────────────────────────────
        case GEMDRVEMUL_FCREATE_CALL:
        {
            payloadPtr += 6;
            char filename[MAX_FOLDER_LENGTH] = {0};
            copy_atari_str(payloadPtr, str_max_words(payload_size_received),
                           filename, sizeof(filename));
            log_event(EVT_FCREATE, log_pack4(filename), 0, 0);
            char *fptr = filename;
            if (fptr[1] == ':') fptr += 2;
            if (*fptr == '\\' || *fptr == '/') fptr++;
            char upper[MAX_FOLDER_LENGTH];
            str_upper(fptr, upper, MAX_FOLDER_LENGTH);
            int16_t err = GEMDOS_ERROR;
            FsHandle *h = fs_create(upper, &err);
            if (!h) {
                *((volatile uint16_t *)(mem + GEMDRVEMUL_FCREATE_HANDLE)) = (uint16_t)(int16_t)err;
            } else {
                int fd = alloc_fd(h);
                if (fd < 0) {
                    fs_close(h);
                    *((volatile uint16_t *)(mem + GEMDRVEMUL_FCREATE_HANDLE)) = (uint16_t)(int16_t)GEMDOS_ENHNDL;
                } else {
                    *((volatile uint16_t *)(mem + GEMDRVEMUL_FCREATE_HANDLE)) = (uint16_t)fd;
                }
            }
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_DCREATE_CALL:
        {
            payloadPtr += 6;
            char dirname[MAX_FOLDER_LENGTH] = {0};
            copy_atari_str(payloadPtr, str_max_words(payload_size_received),
                           dirname, sizeof(dirname));
            log_event(EVT_DCREATE_DO, log_pack4(dirname), 0, 0);
            char *dptr = dirname;
            if (dptr[1] == ':') dptr += 2;
            if (*dptr == '\\' || *dptr == '/') dptr++;
            char upper[MAX_FOLDER_LENGTH];
            str_upper(dptr, upper, MAX_FOLDER_LENGTH);
            int16_t err = fs_mkdir(upper);
            *((volatile uint16_t *)(mem + GEMDRVEMUL_DCREATE_STATUS)) = (uint16_t)(int16_t)err;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_DDELETE_CALL:
        {
            payloadPtr += 6;
            char dirname[MAX_FOLDER_LENGTH] = {0};
            copy_atari_str(payloadPtr, str_max_words(payload_size_received),
                           dirname, sizeof(dirname));
            log_event(EVT_DDELETE_DO, log_pack4(dirname), 0, 0);
            char *dptr = dirname;
            if (dptr[1] == ':') dptr += 2;
            if (*dptr == '\\' || *dptr == '/') dptr++;
            char upper[MAX_FOLDER_LENGTH];
            str_upper(dptr, upper, MAX_FOLDER_LENGTH);
            int16_t err = fs_rmdir(upper);
            *((volatile uint16_t *)(mem + GEMDRVEMUL_DDELETE_STATUS)) = (uint16_t)(int16_t)err;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FDELETE_CALL:
        {
            payloadPtr += 6;
            char filename[MAX_FOLDER_LENGTH] = {0};
            copy_atari_str(payloadPtr, str_max_words(payload_size_received),
                           filename, sizeof(filename));
            log_event(EVT_FDELETE, log_pack4(filename), 0, 0);
            char *fptr = filename;
            if (fptr[1] == ':') fptr += 2;
            if (*fptr == '\\' || *fptr == '/') fptr++;
            char upper[MAX_FOLDER_LENGTH];
            str_upper(fptr, upper, MAX_FOLDER_LENGTH);
            int16_t err = fs_unlink(upper);
            *((volatile uint16_t *)(mem + GEMDRVEMUL_FDELETE_STATUS)) = (uint16_t)(int16_t)err;
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FRENAME_CALL:
        {
            payloadPtr += 6;
            // Payload: old_path(128 bytes = 64 words) + new_path(128 bytes = 64 words)
            char old_name[MAX_FOLDER_LENGTH] = {0};
            char new_name[MAX_FOLDER_LENGTH] = {0};
            copy_atari_str(payloadPtr,      64, old_name, sizeof(old_name));
            copy_atari_str(payloadPtr + 64, 64, new_name, sizeof(new_name));
            log_event(EVT_FRENAME, log_pack4(old_name), log_pack4(new_name), 0);
            char *optr = old_name;
            if (optr[1] == ':') optr += 2;
            if (*optr == '\\' || *optr == '/') optr++;
            char oupper[MAX_FOLDER_LENGTH];
            str_upper(optr, oupper, MAX_FOLDER_LENGTH);
            char *nptr = new_name;
            if (nptr[1] == ':') nptr += 2;
            if (*nptr == '\\' || *nptr == '/') nptr++;
            char nupper[MAX_FOLDER_LENGTH];
            str_upper(nptr, nupper, MAX_FOLDER_LENGTH);
            int16_t err = fs_rename(oupper, nupper);
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_FRENAME_STATUS, (uint32_t)(int32_t)err);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_PEXEC_CALL:
        {
            log_event(EVT_PEXEC, 0, 0, 0);
            // WRITE_AND_SWAP_LONGWORD swaps the two 16-bit words before storing,
            // so the 68k reads back the original value. The ROM checks
            // CMPI.W #0, $FB4184 (reads the HIGH WORD of PEXEC_MODE).
            // Value 0x00010000 → HIGH WORD = 0x0001 → not equal → BNE → FA08E4
            //   → old_handler (original TOS Pexec) instead of crashing FOPEN path.
            WRITE_AND_SWAP_LONGWORD(mem, GEMDRVEMUL_PEXEC_MODE, 0x00010000);
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }

        default:
        {
            log_event(EVT_DISPATCH, cmd | 0x80000000u, 0, 0);  // 0x8xxx = unhandled
            write_random_token(mem);
            active_command_id = 0xFFFF;
            break;
        }
        }
    }
}

/**
 * File: test_floppy_bpb_validation.c
 * Host test for the SideTNFS floppy emulator's .ST geometry/BPB
 * validation (sidetnfs_floppy_emul_validate_geometry(), header-only, no
 * SDK dependency -- see romemul/include/sidetnfs_floppy_emul.h). Same
 * pattern as tests/host_config/test_longpress.c.
 *
 * Run:
 *   gcc -std=c11 -Wall -Wextra -I ../../romemul -o /tmp/test_floppy_bpb \
 *       test_floppy_bpb_validation.c && /tmp/test_floppy_bpb
 */
#include <stdio.h>

#include "include/sidetnfs_floppy_emul.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                             \
    do                                                               \
    {                                                                \
        g_checks++;                                                 \
        if (!(cond))                                                \
        {                                                           \
            g_failures++;                                           \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

// Test 1: a genuine 720KB double-sided 9-sector 80-track image (the
// classic Atari ST DD format) validates cleanly and reports exactly the
// geometry encoded in its own BPB.
static void test1_valid_720k_ds9(void)
{
    printf("Test 1: valid 720KB DS/9-sector/80-track image\n");
    uint32_t filesize = 80u * 2u * 9u * 512u; // 737280 bytes
    sidetnfs_floppy_geometry_t geom;
    sidetnfs_floppy_emul_status_t rc =
        sidetnfs_floppy_emul_validate_geometry(filesize, 512, (uint16_t)(filesize / 512u), 9, 2, &geom);
    CHECK(rc == SIDETNFS_FLOPPY_EMUL_OK, "720KB DS/9/80 validates OK");
    CHECK(geom.sides == 2, "sides == 2");
    CHECK(geom.sectors_per_track == 9, "sectors_per_track == 9");
    CHECK(geom.tracks == 80, "tracks == 80");
    CHECK(geom.bytes_per_sector == 512, "bytes_per_sector == 512");
    CHECK(geom.total_sectors == filesize / 512u, "total_sectors == filesize/512");
}

// Test 2: single-sided 9-sector 80-track image (the smallest MVP-legal
// image) -- boundary on sides=1.
static void test2_valid_single_sided(void)
{
    printf("Test 2: valid single-sided 9-sector/80-track image\n");
    uint32_t filesize = 80u * 1u * 9u * 512u;
    sidetnfs_floppy_geometry_t geom;
    sidetnfs_floppy_emul_status_t rc =
        sidetnfs_floppy_emul_validate_geometry(filesize, 512, (uint16_t)(filesize / 512u), 9, 1, &geom);
    CHECK(rc == SIDETNFS_FLOPPY_EMUL_OK, "single-sided 9/80 validates OK");
    CHECK(geom.sides == 1, "sides == 1");
    CHECK(geom.tracks == 80, "tracks == 80");
}

// Test 3: the two track-count boundaries (80 and 85) are both accepted;
// 79 and 86 are both rejected as unsupported geometry.
static void test3_track_count_boundaries(void)
{
    printf("Test 3: track-count boundaries (80..85 accepted, outside rejected)\n");
    sidetnfs_floppy_geometry_t geom;

    uint32_t fs80 = 80u * 2u * 10u * 512u;
    CHECK(sidetnfs_floppy_emul_validate_geometry(fs80, 512, (uint16_t)(fs80 / 512u), 10, 2, &geom) ==
              SIDETNFS_FLOPPY_EMUL_OK,
          "80 tracks accepted");

    uint32_t fs85 = 85u * 2u * 10u * 512u;
    CHECK(sidetnfs_floppy_emul_validate_geometry(fs85, 512, (uint16_t)(fs85 / 512u), 10, 2, &geom) ==
              SIDETNFS_FLOPPY_EMUL_OK,
          "85 tracks accepted");

    uint32_t fs79 = 79u * 2u * 10u * 512u;
    CHECK(sidetnfs_floppy_emul_validate_geometry(fs79, 512, (uint16_t)(fs79 / 512u), 10, 2, &geom) ==
              SIDETNFS_FLOPPY_EMUL_ERR_GEOMETRY_UNSUPPORTED,
          "79 tracks rejected (below MVP range)");

    uint32_t fs86 = 86u * 2u * 10u * 512u;
    CHECK(sidetnfs_floppy_emul_validate_geometry(fs86, 512, (uint16_t)(fs86 / 512u), 10, 2, &geom) ==
              SIDETNFS_FLOPPY_EMUL_ERR_GEOMETRY_UNSUPPORTED,
          "86 tracks rejected (above MVP range)");
}

// Test 4: sectors-per-track outside {9,10,11} is rejected -- in
// particular SPT=18 (the real 1.44MB HD format), which is explicitly out
// of this MVP's scope.
static void test4_unsupported_sectors_per_track(void)
{
    printf("Test 4: unsupported sectors-per-track values rejected\n");
    sidetnfs_floppy_geometry_t geom;

    uint32_t fs_hd = 80u * 2u * 18u * 512u; // a real 1.44MB HD image
    CHECK(sidetnfs_floppy_emul_validate_geometry(fs_hd, 512, (uint16_t)(fs_hd / 512u), 18, 2, &geom) ==
              SIDETNFS_FLOPPY_EMUL_ERR_GEOMETRY_UNSUPPORTED,
          "18 sectors/track (HD format) rejected");

    uint32_t fs_8 = 80u * 2u * 8u * 512u;
    CHECK(sidetnfs_floppy_emul_validate_geometry(fs_8, 512, (uint16_t)(fs_8 / 512u), 8, 2, &geom) ==
              SIDETNFS_FLOPPY_EMUL_ERR_GEOMETRY_UNSUPPORTED,
          "8 sectors/track rejected (below supported range)");
}

// Test 5: sides outside {1,2} is rejected.
static void test5_unsupported_sides(void)
{
    printf("Test 5: unsupported sides value rejected\n");
    sidetnfs_floppy_geometry_t geom;
    uint32_t fs = 80u * 3u * 9u * 512u;
    CHECK(sidetnfs_floppy_emul_validate_geometry(fs, 512, (uint16_t)(fs / 512u), 9, 3, &geom) ==
              SIDETNFS_FLOPPY_EMUL_ERR_GEOMETRY_UNSUPPORTED,
          "3 sides rejected");
    CHECK(sidetnfs_floppy_emul_validate_geometry(fs, 512, (uint16_t)(fs / 512u), 9, 0, &geom) ==
              SIDETNFS_FLOPPY_EMUL_ERR_GEOMETRY_UNSUPPORTED,
          "0 sides rejected");
}

// Test 6: BPB bytes-per-sector field must be exactly 512.
static void test6_bpb_bytes_per_sector_must_be_512(void)
{
    printf("Test 6: BPB bytes-per-sector != 512 rejected\n");
    sidetnfs_floppy_geometry_t geom;
    uint32_t fs = 80u * 2u * 9u * 512u;
    CHECK(sidetnfs_floppy_emul_validate_geometry(fs, 1024, (uint16_t)(fs / 512u), 9, 2, &geom) ==
              SIDETNFS_FLOPPY_EMUL_ERR_BPB_INVALID,
          "BPS=1024 rejected");
    CHECK(sidetnfs_floppy_emul_validate_geometry(fs, 256, (uint16_t)(fs / 512u), 9, 2, &geom) ==
              SIDETNFS_FLOPPY_EMUL_ERR_BPB_INVALID,
          "BPS=256 rejected");
}

// Test 7: file size that isn't a nonzero multiple of 512 is rejected
// before any geometry math runs.
static void test7_filesize_not_sector_aligned(void)
{
    printf("Test 7: file size 0 or not a multiple of 512 rejected\n");
    sidetnfs_floppy_geometry_t geom;
    CHECK(sidetnfs_floppy_emul_validate_geometry(0, 512, 0, 9, 2, &geom) == SIDETNFS_FLOPPY_EMUL_ERR_FILESIZE_INVALID,
          "filesize 0 rejected");
    CHECK(sidetnfs_floppy_emul_validate_geometry(1000, 512, 1, 9, 2, &geom) ==
              SIDETNFS_FLOPPY_EMUL_ERR_FILESIZE_INVALID,
          "filesize 1000 (not a multiple of 512) rejected");
}

// Test 8: file size that does not divide exactly by sides*sectors-per-track
// (an inexact track count) is rejected as a geometry mismatch, distinct
// from an out-of-range track count.
static void test8_inexact_track_division_rejected(void)
{
    printf("Test 8: file size not evenly divisible by sides*sectors-per-track rejected\n");
    sidetnfs_floppy_geometry_t geom;
    // 80*2*9*512 + one extra sector's worth of bytes -- 80.055... tracks.
    uint32_t fs = (80u * 2u * 9u + 1u) * 512u;
    sidetnfs_floppy_emul_status_t rc = sidetnfs_floppy_emul_validate_geometry(fs, 512, (uint16_t)(fs / 512u), 9, 2, &geom);
    CHECK(rc == SIDETNFS_FLOPPY_EMUL_ERR_GEOMETRY_MISMATCH, "inexact track division rejected as GEOMETRY_MISMATCH");
}

// Test 9: "BPB total sectors must agree with actual file size" -- even
// when the file size on its own would imply valid, in-range geometry,
// a BPB SEC field that disagrees with filesize/512 must still be
// rejected. This is the req #2 requirement most likely to be silently
// skipped by an implementation that only checks the file-size-derived
// geometry and never cross-checks the BPB's own claim.
static void test9_bpb_total_sectors_mismatch_rejected(void)
{
    printf("Test 9: BPB total-sector field disagreeing with filesize rejected\n");
    sidetnfs_floppy_geometry_t geom;
    uint32_t fs = 80u * 2u * 9u * 512u; // really 1440 sectors
    sidetnfs_floppy_emul_status_t rc = sidetnfs_floppy_emul_validate_geometry(fs, 512, 1439, 9, 2, &geom);
    CHECK(rc == SIDETNFS_FLOPPY_EMUL_ERR_GEOMETRY_MISMATCH, "BPB SEC=1439 (actual is 1440) rejected");
}

// Test 10: on any rejection, *out_geom is left fully zeroed -- a caller
// must never be able to mistake a rejected image's out_geom for a valid
// one (mirrors req #6's "never return stale data on failure" for the
// geometry result specifically, not just sector reads).
static void test10_rejected_geometry_is_zeroed(void)
{
    printf("Test 10: rejected validation leaves *out_geom zeroed\n");
    sidetnfs_floppy_geometry_t geom;
    geom.sides = 9;
    geom.sectors_per_track = 9;
    geom.tracks = 9;
    geom.bytes_per_sector = 9999;
    geom.total_sectors = 9999;
    sidetnfs_floppy_emul_status_t rc = sidetnfs_floppy_emul_validate_geometry(0, 512, 0, 9, 2, &geom);
    CHECK(rc != SIDETNFS_FLOPPY_EMUL_OK, "sanity: this call does fail");
    CHECK(geom.sides == 0 && geom.sectors_per_track == 0 && geom.tracks == 0 && geom.bytes_per_sector == 0 &&
              geom.total_sectors == 0,
          "out_geom fully zeroed after a rejection");
}

int main(void)
{
    test1_valid_720k_ds9();
    test2_valid_single_sided();
    test3_track_count_boundaries();
    test4_unsupported_sectors_per_track();
    test5_unsupported_sides();
    test6_bpb_bytes_per_sector_must_be_512();
    test7_filesize_not_sector_aligned();
    test8_inexact_track_division_rejected();
    test9_bpb_total_sectors_mismatch_rejected();
    test10_rejected_geometry_is_zeroed();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

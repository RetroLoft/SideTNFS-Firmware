/**
 * File: sidetnfs_charset.c
 * Description: The Atari ST character set from 0x80, to and from UTF-8.
 * See include/sidetnfs_charset.h. Ported from TheHulk.
 */
#include <stdint.h>
#include "include/sidetnfs_charset.h"

// Unicode code point of Atari characters 0x80..0xFF (0x00..0x7F are ASCII).
static const uint16_t s_atari_upper[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, // 80
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, // 90
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x00DF, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, // A0
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x00E3, 0x00F5, 0x00D8, 0x00F8, 0x0153, 0x0152, 0x00C0, 0x00C3, // B0
    0x00D5, 0x00A8, 0x00B4, 0x2020, 0x00B6, 0x00A9, 0x00AE, 0x2122,
    0x0133, 0x0132, 0x05D0, 0x05D1, 0x05D2, 0x05D3, 0x05D4, 0x05D5, // C0
    0x05D6, 0x05D7, 0x05D8, 0x05D9, 0x05DB, 0x05DC, 0x05DE, 0x05E0,
    0x05E1, 0x05E2, 0x05E4, 0x05E6, 0x05E7, 0x05E8, 0x05E9, 0x05EA, // D0
    0x05DF, 0x05DA, 0x05DD, 0x05E3, 0x05E5, 0x00A7, 0x2227, 0x221E,
    0x03B1, 0x03B2, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, // E0
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x222E, 0x03C6, 0x2208, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, // F0
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x00B3, 0x00AF,
};

bool sidetnfs_charset_has_high_bytes(const char *s)
{
    for (; *s; s++)
    {
        if ((uint8_t)*s >= 0x80)
        {
            return true;
        }
    }
    return false;
}

void sidetnfs_atari_to_utf8(const char *in, char *out, size_t size)
{
    size_t n = 0;
    if (size == 0)
    {
        return;
    }
    for (; *in; in++)
    {
        uint8_t c = (uint8_t)*in;
        uint16_t u = c < 0x80 ? c : s_atari_upper[c - 0x80];
        size_t len = u < 0x80 ? 1 : (u < 0x800 ? 2 : 3);
        if (n + len >= size)
        {
            break;
        }
        if (len == 1)
        {
            out[n++] = (char)u;
        }
        else if (len == 2)
        {
            out[n++] = (char)(0xC0 | (u >> 6));
            out[n++] = (char)(0x80 | (u & 0x3F));
        }
        else
        {
            out[n++] = (char)(0xE0 | (u >> 12));
            out[n++] = (char)(0x80 | ((u >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (u & 0x3F));
        }
    }
    out[n] = '\0';
}

bool sidetnfs_utf8_to_atari(const char *in, char *out, size_t size)
{
    size_t n = 0;
    const uint8_t *p = (const uint8_t *)in;
    while (*p)
    {
        uint32_t u;
        if (*p < 0x80)
        {
            u = *p++;
        }
        else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
        {
            u = (uint32_t)(p[0] & 0x1F) << 6 | (uint32_t)(p[1] & 0x3F);
            p += 2;
        }
        else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
        {
            u = (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (uint32_t)(p[2] & 0x3F);
            p += 3;
        }
        else
        {
            return false; // not UTF-8, or beyond what the Atari can show
        }
        int c = u < 0x80 ? (int)u : -1;
        for (int i = 0; c < 0 && i < 128; i++)
        {
            if (s_atari_upper[i] == u)
            {
                c = 0x80 + i;
            }
        }
        if (c < 0 || n + 1 >= size)
        {
            return false;
        }
        out[n++] = (char)c;
    }
    if (size == 0)
    {
        return false;
    }
    out[n] = '\0';
    return true;
}

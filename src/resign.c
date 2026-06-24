/*
 * resign.c  –  Non-roamable save detection, HDDKey management, re-signing
 *
 * Signing algorithm for SIGN_NOROAM games:
 *   signature = HMAC-SHA1(key=XboxHDKey, data=save_body)
 *   The 20-byte signature is stored at the start of the save file.
 *
 * HMAC-SHA1 is implemented here without external deps (self-contained).
 *
 * Sources:
 *   - consolemods.org wiki (game list + offsets)
 *   - XSavSig005 resign.ini (SigType, offsets)
 *   - feudalnate's resigner source
 */

#include "resign.h"
#include "fileops.h"
#include "util.h"
#include "../lib/ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <hal/debug.h>

/* nxdk EEPROM query */
#include <xboxkrnl/xboxkrnl.h>

/* XboxCERTKey – zelfde op elke Xbox, ingebouwd in de kernel */
static const unsigned char XBOX_CERT_KEY[16] = {
    0x5C, 0x07, 0x33, 0xAE, 0x04, 0x01, 0xF7, 0xE8,
    0xBA, 0x79, 0x93, 0xFD, 0xCD, 0x2F, 0x1F, 0xE0
};

/* ── Known non-roamable games ────────────────────────────────────────────── */
/*
 * Title IDs are the 8-digit uppercase hex values used as directory names
 * under E:\TDATA\.  Sourced from XSavSig005 resign.ini + consolemods wiki.
 *
 * sign_offset = 0 means "signature is at byte 0 of the save file".
 * sign_size   = 20 for standard HMAC-SHA1 (160-bit).
 */
const NonRoamableEntry NONROAMABLE_DB[] = {
 /* TitleID     Naam                                    Type          SigKey                                                                                    DataOff SigOff SigSz CrcOff */
 { "4C41000D", "Armed and Dangerous",                   SIGN_NOROAM, {0x2B,0x6E,0x33,0x42,0x2A,0x6A,0xEC,0x5A,0x41,0xE4,0xA7,0x7E,0x68,0x82,0x20,0x1A},  24,   0,  20, -1, NULL },
 { "45410083", "Black",                                 SIGN_NOROAM, {0x87,0xD3,0x0D,0x70,0xE3,0x61,0xDD,0x1C,0x51,0x7B,0x58,0x8E,0x01,0x18,0xC3,0xE0},   0,  -1,  20, -1, NULL },
 /*
  * Burnout 2: stap 1 = XOR32 over [0,31412) -> schrijf naar offset 31412 (4 bytes).
  *            stap 2 = HMAC-SHA1 over [0,31416) -> schrijf naar offset 31416 (20 bytes).
  * SigKey + offsets uit XSavSig005 resign.ini, geverifieerd tegen sample save.
  */
 { "41430019", "Burnout 2: Point of Impact",             SIGN_NOROAM, {0xE8,0x72,0x8E,0x71,0x0E,0x30,0x84,0x8B,0x21,0xB3,0x23,0x25,0x9E,0x78,0x3D,0xF3},   0, 31416, 20, 31412, NULL },
 { "4541005B", "Burnout 3 (PAL/NTSC-U)",                SIGN_NOROAM, {0x8F,0x4B,0xC9,0xB5,0xD6,0xB0,0x1E,0x7F,0x47,0x72,0xC5,0xAC,0xE0,0x4B,0xC9,0x22},   0,  -1,  20, -1, NULL },
 { "45410061", "Burnout 3 (NTSC-J)",                    SIGN_NOROAM, {0x8F,0x4B,0xC9,0xB5,0xD6,0xB0,0x1E,0x7F,0x47,0x72,0xC5,0xAC,0xE0,0x4B,0xC9,0x22},   0,  -1,  20, -1, NULL },
 /*
  * Dead or Alive Xtreme Beach Volleyball: één bestand (doaxuser.dat) heeft twee
  * gesigneerde regions elk met XOR64 CRC + HMAC-SHA1.
  *
  * resign_save_file() roept SIGN_DOAX op in resign_doax().
  * Playlist-region CRC-offset (8664) en sig-offset (8672) zijn bekend.
  * Main-region CRC-offset in header [0,96) is TODO: onbekend zonder sample save.
  * sig_offset bevat main sig-offset (8); data_offset bevat main data-offset (96).
  */
 { "54430007", "DOA Xtreme Beach Volleyball",           SIGN_DOAX,   {0x1E,0x1E,0x8C,0x0B,0xB7,0x44,0xEA,0x78,0x14,0xC0,0x4C,0xF1,0x09,0xA7,0x6D,0x9A},  96,   8, 20, -1, "doaxuser.dat" },
 { "54430006", "Dead or Alive Ultimate",                SIGN_NOROAM, {0x42,0xCB,0xF5,0x32,0xBF,0x1F,0xD1,0x47,0x39,0xD0,0xF7,0xF6,0x6D,0xF8,0xB7,0xB1},  20,   0,  20, -1, NULL },
 { "454D0009", "FlatOut",                               SIGN_NOROAM, {0xC8,0xD3,0xC3,0x57,0x3C,0xFA,0xD0,0x39,0xB9,0xDB,0xB5,0x4D,0x74,0x2E,0xD5,0x79},   0,  -1,  20, -1, NULL },
 { "454D0020", "FlatOut 2",                             SIGN_NOROAM, {0xEC,0x19,0x81,0x16,0x7F,0x15,0xAC,0xB9,0xFE,0xAD,0x8A,0xB6,0x7A,0x02,0xC1,0xA5},   0,  -1,  20, -1, NULL },
 { "4D53006E", "Forza Motorsport",                      SIGN_FORZA,  {0x9B,0x62,0xFF,0x6F,0xF9,0x8D,0xD4,0xF0,0x02,0xA1,0xBD,0x2B,0x72,0x60,0x8E,0xFC},   0,  -1,  20, -1, NULL },
 { "54540009", "Mafia",                                 SIGN_NOROAM, {0x7F,0x2B,0xFB,0x08,0x22,0xD1,0x25,0xB9,0x58,0xDA,0xA8,0xE9,0xC6,0x40,0xB8,0x3D},   0,  -1,  20, -1, NULL },
 { "54430003", "Ninja Gaiden",                          SIGN_NOROAM, {0xA5,0x01,0x14,0xCA,0x2B,0x7C,0x81,0x98,0xE8,0x29,0xE7,0xC9,0x37,0xD6,0xFC,0x40},  20,   0,  20, -1, NULL },
 { "5443000D", "Ninja Gaiden Black",                    SIGN_NOROAM, {0xFC,0x33,0x76,0x48,0x8B,0x3E,0x5F,0x00,0xF6,0x5A,0x6B,0xDA,0x92,0x09,0xCF,0xE8},  20,   0,  20, -1, NULL },
 { "4D530018", "Phantasy Star Online Ep I+II",          SIGN_PSO,    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},   0,   0,  20, -1, NULL },
 { "4D53001C", "Phantasy Star Online Ep III",           SIGN_PSO,    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},   0,   0, 20, -1, NULL },
 { "4D53004A", "Phantasy Star Online Ep I+II (NTSC-U)",  SIGN_PSO,    {0xE1,0xF4,0x9B,0x7D,0x45,0xAF,0xEA,0xA2,0x1D,0x57,0x18,0x70,0x98,0x28,0xA8,0xB8},   0,   0, 20, -1, NULL },
 { "5553005E", "Splinter Cell: Double Agent",           SIGN_NOROAM, {0x61,0x67,0x6F,0x08,0xB3,0x1D,0x71,0xDC,0xF8,0xE0,0x29,0x03,0xCB,0x51,0x64,0xBB},   0,  -1,  20, -1, NULL },
 { "4C410013", "Star Wars: Republic Commando",          SIGN_NOROAM, {0x88,0xC3,0xBC,0x25,0xF5,0xBB,0x1D,0xE1,0x1F,0x1F,0x6F,0xCE,0x21,0xBF,0x7F,0x74},   0,  -1,  20, -1, NULL },
 { "4C410019", "Star Wars: Republic Commando (JP)",     SIGN_NOROAM, {0x54,0x73,0x1A,0xBF,0x22,0xF0,0xEA,0x86,0x87,0xAA,0xA6,0x47,0x8F,0x4B,0xAA,0xEA},   0,  -1,  20, -1, NULL },
 { "5655000D", "The Thing (NTSC)",                      SIGN_NOROAM, {0x43,0xCB,0x90,0x4A,0x55,0x2E,0xAC,0x23,0x7D,0x62,0xD9,0xD2,0xEC,0x2E,0x7D,0x9C},  20,   0,  20, -1, "Options.dat" },
 { "56560003", "The Thing (PAL)",                       SIGN_NOROAM, {0x1C,0xE3,0x97,0x5F,0x1B,0x24,0x86,0xB4,0x26,0x4A,0xDC,0x9D,0x1D,0x84,0x4D,0x17},  20,   0,  20, -1, "Options.dat" },
};
const int NONROAMABLE_DB_COUNT =
    (int)(sizeof(NONROAMABLE_DB) / sizeof(NONROAMABLE_DB[0]));

/* ── Lookup ──────────────────────────────────────────────────────────────── */
const NonRoamableEntry *resign_lookup(const char *title_id)
{
    for (int i = 0; i < NONROAMABLE_DB_COUNT; i++)
        if (util_strcasecmp(NONROAMABLE_DB[i].title_id, title_id) == 0)
            return &NONROAMABLE_DB[i];
    return NULL;
}

/* ── Read HDDKey from EEPROM ─────────────────────────────────────────────── */
/*
 * The Xbox EEPROM layout (relevant portion):
 *   0x00  HMAC-SHA1 of encrypted section
 *   0x14  Encrypted section (confounder + factory settings)
 *   0x30  XboxHDKey  (16 bytes, plaintext after decryption by kernel)
 *
 * nxdk exposes this via NtQuerySystemInformation with the
 * SystemEncryptionInformation class, or more simply via the
 * ExQueryNonVolatileSetting / XQueryValue APIs.
 *
 * The easiest reliable method on nxdk is to read the raw EEPROM via
 * HalReadSMBusValue() at device address 0xA8, register 0x30.
 */
int resign_read_hddkey(unsigned char key_out[HDDKEY_SIZE])
{
    memcpy(key_out, XboxHDKey, HDDKEY_SIZE);

    char hex[HDDKEY_SIZE * 2 + 1];
    resign_hddkey_to_hex(key_out, hex);
    log_print("resign: XboxHDKey = %s\n", hex);

    return 0;
}

/* ── Hex helpers ─────────────────────────────────────────────────────────── */
void resign_hddkey_to_hex(const unsigned char key[HDDKEY_SIZE],
                           char hex_out[HDDKEY_SIZE * 2 + 1])
{
    for (int i = 0; i < HDDKEY_SIZE; i++)
        snprintf(hex_out + i * 2, 3, "%02X", key[i]);
    hex_out[HDDKEY_SIZE * 2] = '\0';
}

int resign_hex_to_hddkey(const char *hex, unsigned char key_out[HDDKEY_SIZE])
{
    if (strlen(hex) < HDDKEY_SIZE * 2) return -1;
    for (int i = 0; i < HDDKEY_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%02X", &byte) != 1) return -1;
        key_out[i] = (unsigned char)byte;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HMAC-SHA1 implementation (no external deps)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SHA1_DIGEST_SIZE  20
#define SHA1_BLOCK_SIZE   64

typedef struct {
    unsigned int  H[5];
    unsigned char buf[64];
    unsigned int  buf_len;
    unsigned long long total;
} SHA1Ctx;

static inline unsigned int rot32l(unsigned int x, int n)
{ return (x << n) | (x >> (32 - n)); }

static void sha1_init(SHA1Ctx *ctx)
{
    ctx->H[0] = 0x67452301; ctx->H[1] = 0xEFCDAB89;
    ctx->H[2] = 0x98BADCFE; ctx->H[3] = 0x10325476;
    ctx->H[4] = 0xC3D2E1F0;
    ctx->buf_len = 0; ctx->total = 0;
}

static void sha1_compress(SHA1Ctx *ctx, const unsigned char *block)
{
    unsigned int W[80], a, b, c, d, e, f, k, T;
    for (int i = 0; i < 16; i++)
        W[i] = ((unsigned int)block[i*4]   << 24) |
               ((unsigned int)block[i*4+1] << 16) |
               ((unsigned int)block[i*4+2] <<  8) |
               ((unsigned int)block[i*4+3]);
    for (int i = 16; i < 80; i++)
        W[i] = rot32l(W[i-3]^W[i-8]^W[i-14]^W[i-16], 1);

    a=ctx->H[0]; b=ctx->H[1]; c=ctx->H[2]; d=ctx->H[3]; e=ctx->H[4];
    for (int i = 0; i < 80; i++) {
        if      (i < 20) { f=( b&c)|(~b& d); k=0x5A827999; }
        else if (i < 40) { f=  b^c ^  d;     k=0x6ED9EBA1; }
        else if (i < 60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
        else             { f=  b^c ^  d;     k=0xCA62C1D6; }
        T = rot32l(a,5)+f+e+k+W[i];
        e=d; d=c; c=rot32l(b,30); b=a; a=T;
    }
    ctx->H[0]+=a; ctx->H[1]+=b; ctx->H[2]+=c;
    ctx->H[3]+=d; ctx->H[4]+=e;
}

static void sha1_update(SHA1Ctx *ctx, const unsigned char *data, size_t len)
{
    ctx->total += len;
    while (len > 0) {
        size_t space = SHA1_BLOCK_SIZE - ctx->buf_len;
        size_t take  = (len < space) ? len : space;
        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += (unsigned int)take;
        data += take; len -= take;
        if (ctx->buf_len == SHA1_BLOCK_SIZE) {
            sha1_compress(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

static void sha1_final(SHA1Ctx *ctx, unsigned char digest[SHA1_DIGEST_SIZE])
{
    unsigned long long bits = ctx->total * 8;
    unsigned char pad = 0x80;
    sha1_update(ctx, &pad, 1);
    pad = 0;
    while (ctx->buf_len != 56) sha1_update(ctx, &pad, 1);
    unsigned char len_block[8];
    for (int i = 7; i >= 0; i--) { len_block[i]=(unsigned char)(bits&0xFF); bits>>=8; }
    sha1_update(ctx, len_block, 8);
    for (int i = 0; i < 5; i++) {
        digest[i*4]   = (ctx->H[i]>>24)&0xFF;
        digest[i*4+1] = (ctx->H[i]>>16)&0xFF;
        digest[i*4+2] = (ctx->H[i]>> 8)&0xFF;
        digest[i*4+3] = (ctx->H[i]    )&0xFF;
    }
}

static void hmac_sha1(const unsigned char *key, size_t key_len,
                       const unsigned char *data, size_t data_len,
                       unsigned char mac_out[SHA1_DIGEST_SIZE])
{
    unsigned char k_ipad[SHA1_BLOCK_SIZE] = {0};
    unsigned char k_opad[SHA1_BLOCK_SIZE] = {0};
    unsigned char tmp_key[SHA1_DIGEST_SIZE];

    if (key_len > SHA1_BLOCK_SIZE) {
        SHA1Ctx ctx; sha1_init(&ctx);
        sha1_update(&ctx, key, key_len);
        sha1_final(&ctx, tmp_key);
        key = tmp_key; key_len = SHA1_DIGEST_SIZE;
    }
    memcpy(k_ipad, key, key_len);
    memcpy(k_opad, key, key_len);
    for (int i = 0; i < SHA1_BLOCK_SIZE; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5C;
    }
    unsigned char inner[SHA1_DIGEST_SIZE];
    SHA1Ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, k_ipad, SHA1_BLOCK_SIZE);
    sha1_update(&ctx, data, data_len);
    sha1_final(&ctx, inner);

    sha1_init(&ctx);
    sha1_update(&ctx, k_opad, SHA1_BLOCK_SIZE);
    sha1_update(&ctx, inner, SHA1_DIGEST_SIZE);
    sha1_final(&ctx, mac_out);
}

/* ── Helper: XOR64 over een aaneengesloten regio ────────────────────────── */
static void compute_xor64(const unsigned char *buf, size_t start, size_t end,
                           unsigned char out[8])
{
    uint64_t xor_val = 0;
    for (size_t k = start; k + 7 < end; k += 8) {
        uint64_t word =
            (uint64_t)buf[k]     | ((uint64_t)buf[k+1] <<  8) |
            ((uint64_t)buf[k+2] << 16) | ((uint64_t)buf[k+3] << 24) |
            ((uint64_t)buf[k+4] << 32) | ((uint64_t)buf[k+5] << 40) |
            ((uint64_t)buf[k+6] << 48) | ((uint64_t)buf[k+7] << 56);
        xor_val ^= word;
    }
    for (int i = 0; i < 8; i++)
        out[i] = (unsigned char)(xor_val >> (i * 8));
}

/* ── DOAXBV resign: twee gesigneerde regions in doaxuser.dat ─────────────
 *
 * Main profile region:
 *   XOR64  over [96, 8664) -> 8 bytes -> offset 0
 *   HMAC   over [96, 8664) -> 20 bytes -> offset 8
 *
 * Playlist region:
 *   XOR64  over [8760, 11176) -> 8 bytes -> offset 8664
 *   HMAC   over [8760, 11176) -> 20 bytes -> offset 8672
 *
 * Bestandsformaat geverifieerd tegen sample doaxuser.dat.
 * ─────────────────────────────────────────────────────────────────────── */
static int resign_doax(const char *local_path,
                       const NonRoamableEntry *entry,
                       const unsigned char old_key[HDDKEY_SIZE],
                       const unsigned char new_key[HDDKEY_SIZE])
{
    (void)old_key;  /* XOR64 is deterministisch, old_key niet nodig */

    FILE *f = fopen(local_path, "r+b");
    if (!f) {
        char msg[MAX_PATH_LEN + 32];
        snprintf(msg, sizeof(msg), "Kan bestand niet openen:\n%s", local_path);
        ui_message("Resign fout", msg);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    if (fsize < 11176) {
        fclose(f);
        ui_message("Resign fout", "doaxuser.dat te klein (< 11176 bytes)");
        return -1;
    }

    unsigned char *buf = (unsigned char *)malloc((size_t)fsize);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, (size_t)fsize, f);

    /* authkey = HMAC(XboxCERTKey, SigKey)[0..15] */
    unsigned char authkey[16];
    unsigned char temp[SHA1_DIGEST_SIZE];
    hmac_sha1(XBOX_CERT_KEY, 16, entry->title_sig_key, 16, temp);
    memcpy(authkey, temp, 16);

    /* ── Main region ── */
    compute_xor64(buf, 96, 8664, buf + 0);          /* XOR64 -> offset 0    */

    unsigned char roamable[SHA1_DIGEST_SIZE];
    hmac_sha1(authkey, 16, buf + 96, 8568, roamable);
    unsigned char main_sig[SHA1_DIGEST_SIZE];
    hmac_sha1(new_key, HDDKEY_SIZE, roamable, SHA1_DIGEST_SIZE, main_sig);
    memcpy(buf + 8, main_sig, SHA1_DIGEST_SIZE);     /* HMAC -> offset 8     */

    log_print("resign: DOAXBV main XOR64 @ 0, HMAC @ 8\n");

    /* ── Playlist region ── */
    compute_xor64(buf, 8760, 11176, buf + 8664);     /* XOR64 -> offset 8664 */

    hmac_sha1(authkey, 16, buf + 8760, 2416, roamable);
    unsigned char pl_sig[SHA1_DIGEST_SIZE];
    hmac_sha1(new_key, HDDKEY_SIZE, roamable, SHA1_DIGEST_SIZE, pl_sig);
    memcpy(buf + 8672, pl_sig, SHA1_DIGEST_SIZE);    /* HMAC -> offset 8672  */

    log_print("resign: DOAXBV playlist XOR64 @ 8664, HMAC @ 8672\n");

    /* Schrijf terug */
    rewind(f);
    fwrite(buf, 1, (size_t)fsize, f);
    fclose(f);
    free(buf);
    return 0;
}

/* ── Re-sign a single save file ──────────────────────────────────────────── */
int resign_save_file(const char *local_path,
                     const NonRoamableEntry *entry,
                     const unsigned char old_key[HDDKEY_SIZE],
                     const unsigned char new_key[HDDKEY_SIZE])
{
    if (entry->sign_type == SIGN_NONE) return 0;

    if (entry->sign_type == SIGN_FORZA) {
        char msg[MAX_PATH_LEN + 64];
        snprintf(msg, sizeof(msg), "Forza signing niet geimplementeerd.\nGebruik ForzaSign op PC:\n%s", local_path);
        ui_message("Resign", msg);
        return -1;
    }

    if (entry->sign_type == SIGN_DOAX) {
        return resign_doax(local_path, entry, old_key, new_key);
    }

    FILE *f = fopen(local_path, "r+b");
    if (!f) {   
                char msg[MAX_PATH_LEN + 32];
                snprintf(msg, sizeof(msg), "Kan bestand niet openen:\n%s", local_path);
                ui_message("Resign fout", msg);
                return -1; 
             }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    /* minimale grootte: data_offset + sig_size */
    if (fsize < entry->data_offset + entry->sig_size) {
        fclose(f); return -1;
    }

    unsigned char *buf = (unsigned char *)malloc(fsize);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, fsize, f);

    /* Bepaal werkelijke sig_offset.
     * sig_offset < 0  = relatief aan einde: bijv. -1 → fsize-20, -28 → fsize-28.
     * sig_offset >= 0 = absoluut bestandsoffset.
     */
    size_t actual_sig_offset = (entry->sig_offset < 0)
        ? (size_t)((long)fsize + entry->sig_offset)
        : (size_t)entry->sig_offset;

    /* Body: van data_offset tot sig_offset (sig aan einde of vaste offset)
     *       of van data_offset tot einde bestand (sig aan begin, offset==0) */
    const unsigned char *body;
    size_t               body_len;

    if (entry->sig_offset == 0) {
        body     = buf + entry->data_offset;
        body_len = (size_t)fsize - entry->data_offset;
    } else {
        body     = buf + entry->data_offset;
        body_len = actual_sig_offset - entry->data_offset;
    }

    /* Optionele XOR32 checksum (bijv. Burnout 2):
     * Herbereken XOR van alle 32-bit words over [data_offset, crc32_offset)
     * en schrijf het 4-byte resultaat naar crc32_offset (little-endian).
     * Moet vóór de HMAC zodat de bijgewerkte CRC meegenomen wordt in de body.
     */
    if (entry->crc32_offset >= 0) {
        size_t crc_end = (size_t)entry->crc32_offset;
        unsigned int xor_val = 0;
        for (size_t k = (size_t)entry->data_offset; k + 3 < crc_end; k += 4) {
            unsigned int word = (unsigned int)buf[k]
                              | ((unsigned int)buf[k+1] <<  8)
                              | ((unsigned int)buf[k+2] << 16)
                              | ((unsigned int)buf[k+3] << 24);
            xor_val ^= word;
        }
        buf[entry->crc32_offset]     = (unsigned char)(xor_val      );
        buf[entry->crc32_offset + 1] = (unsigned char)(xor_val >>  8);
        buf[entry->crc32_offset + 2] = (unsigned char)(xor_val >> 16);
        buf[entry->crc32_offset + 3] = (unsigned char)(xor_val >> 24);
        log_print("resign: XOR32 @ %d = %08X\n", entry->crc32_offset, xor_val);
    }

    /* Stap 1: authkey = HMAC(XboxCERTKey, TitleSignatureKey)[0..15] */
    unsigned char temp[SHA1_DIGEST_SIZE];
    unsigned char authkey[16];
    hmac_sha1(XBOX_CERT_KEY, 16, entry->title_sig_key, 16, temp);
    memcpy(authkey, temp, 16);

    /* Stap 2: roamable = HMAC(authkey, body) */
    unsigned char roamable[SHA1_DIGEST_SIZE];
    hmac_sha1(authkey, 16, body, body_len, roamable);

    /* Stap 3: signature = HMAC(HDDKey, roamable) */
    unsigned char new_sig[SHA1_DIGEST_SIZE];
    hmac_sha1(new_key, HDDKEY_SIZE, roamable, SHA1_DIGEST_SIZE, new_sig);

    /* Schrijf CRC (indien aanwezig) en HMAC terug naar bestand */
    if (entry->crc32_offset >= 0) {
        fseek(f, (long)entry->crc32_offset, SEEK_SET);
        fwrite(buf + entry->crc32_offset, 1, 4, f);
    }
    fseek(f, (long)actual_sig_offset, SEEK_SET);
    fwrite(new_sig, 1, SHA1_DIGEST_SIZE, f);
    fclose(f);
    free(buf);

    char msg[MAX_PATH_LEN + 32];
    snprintf(msg, sizeof(msg), "re-signed: %s", local_path);
    ui_message_nowait("Resign", msg);
    return 0;
}
/* ── Process all saves under a TDATA/<TitleID>/ directory ───────────────── */
/*
 * PSO requires signing every file whose name starts with "PSO".
 * Other SIGN_NOROAM games: sign every file found.
 */
static int resign_file_needed(const NonRoamableEntry *entry,
                               const char *filename)
{
    /* Specifieke bestandsnaam filter */
    if (entry->sig_filename != NULL)
        return (util_strcasecmp(filename, entry->sig_filename) == 0);

    /* PSO: alleen bestanden die beginnen met "PSO" */
    if (entry->sign_type == SIGN_PSO)
        return (util_strncasecmp(filename, "PSO", 3) == 0);

    /* Alle andere types: sign alles */
    return 1;
}

int resign_process_title(const char *title_dir,
                          const NonRoamableEntry *entry,
                          const unsigned char old_key[HDDKEY_SIZE],
                          const unsigned char new_key[HDDKEY_SIZE])
{
    char pattern[MAX_PATH_LEN];
    snprintf(pattern, sizeof(pattern), "%s\\*", title_dir);

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int count = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 ||
            strcmp(fd.cFileName, "..") == 0) continue;

        char fullpath[MAX_PATH_LEN];
        snprintf(fullpath, sizeof(fullpath), "%s\\%s",
                 title_dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* Recurse into save subdirectories */
            count += resign_process_title(fullpath, entry, old_key, new_key);
        } else {
            if (resign_file_needed(entry, fd.cFileName)) {
                if (resign_save_file(fullpath, entry,
                                     old_key, new_key) == 0)
                    count++;
            }
        }
    } while (FindNextFile(h, &fd));

    FindClose(h);
    return count;
}

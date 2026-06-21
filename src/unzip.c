/*
 * unzip.c  –  Minimale ZIP-extractor voor SaveSyncX
 *
 * Geen externe dependencies — gebruikt alleen de nxdk stdlib (malloc/free/fopen).
 * Ondersteunt stored (method 0) en deflate (method 8) entries.
 * __MACOSX entries worden overgeslagen.
 *
 * inflate implementatie: puff.c van Mark Adler (zlib contrib), sterk
 * vereenvoudigd voor onze use case. Puff is public domain en heeft
 * nul externe dependencies buiten stddef.h.
 */

#include "unzip.h"
#include "fileops.h"
#include "../lib/util.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <windows.h>

/* ════════════════════════════════════════════════════════════════════════════
 * PUFF  –  inflate (raw deflate) — public domain, Mark Adler
 * Vereenvoudigde versie: alleen in-memory, geen streaming.
 * ════════════════════════════════════════════════════════════════════════════ */

#define MAXBITS  15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES  (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

typedef struct {
    const unsigned char *in;    /* input buffer */
    unsigned long        inlen;
    unsigned long        inpos;
    int                  bitbuf;
    int                  bitcnt;

    unsigned char       *out;   /* output buffer */
    unsigned long        outlen;
    unsigned long        outpos;
} puff_state;

typedef struct {
    int count[MAXBITS+1];
    int symbol[MAXCODES];
} huffman;

static int puff_bits(puff_state *s, int need)
{
    int val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) return -1;
        val |= (int)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return val & ((1 << need) - 1);
}

static int puff_build(huffman *h, const int *length, int n)
{
    int offs[MAXBITS+1];
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++)
        offs[len+1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; sym++)
        if (length[sym]) h->symbol[offs[length[sym]]++] = sym;
    return left;
}

static int puff_decode(puff_state *s, const huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int bit = puff_bits(s, 1);
        if (bit < 0) return -10;
        code |= bit;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -10;
}

static int puff_stored(puff_state *s)
{
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->inpos + 4 > s->inlen) return -1;
    unsigned len  = s->in[s->inpos] | ((unsigned)s->in[s->inpos+1] << 8);
    unsigned nlen = s->in[s->inpos+2] | ((unsigned)s->in[s->inpos+3] << 8);
    s->inpos += 4;
    if ((len ^ nlen) != 0xFFFF) return -2;
    if (s->inpos + len > s->inlen) return -3;
    if (s->outpos + len > s->outlen) return -4;
    memcpy(s->out + s->outpos, s->in + s->inpos, len);
    s->inpos  += len;
    s->outpos += len;
    return 0;
}

/* Lengte extra bits en base values */
static const int lens[29]  = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const int lext[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const int dists[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const int dext[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int puff_codes(puff_state *s, const huffman *lencode, const huffman *distcode)
{
    for (;;) {
        int sym = puff_decode(s, lencode);
        if (sym < 0) return sym;
        if (sym < 256) {
            if (s->outpos >= s->outlen) return -5;
            s->out[s->outpos++] = (unsigned char)sym;
        } else if (sym == 256) {
            break;
        } else {
            sym -= 257;
            if (sym >= 29) return -6;
            int extra = puff_bits(s, lext[sym]);
            if (extra < 0) return extra;
            int len = lens[sym] + extra;

            int dsym = puff_decode(s, distcode);
            if (dsym < 0) return dsym;
            extra = puff_bits(s, dext[dsym]);
            if (extra < 0) return extra;
            unsigned long dist = dists[dsym] + extra;

            if (s->outpos < dist) return -7;
            if (s->outpos + len > s->outlen) return -8;
            while (len--) {
                s->out[s->outpos] = s->out[s->outpos - dist];
                s->outpos++;
            }
        }
    }
    return 0;
}

static int puff_fixed(puff_state *s)
{
    static int virgin = 1;
    static huffman lenfix, distfix;
    if (virgin) {
        int lengths[FIXLCODES];
        int sym;
        for (sym = 0;   sym < 144; sym++) lengths[sym] = 8;
        for (;          sym < 256; sym++) lengths[sym] = 9;
        for (;          sym < 280; sym++) lengths[sym] = 7;
        for (;          sym < FIXLCODES; sym++) lengths[sym] = 8;
        puff_build(&lenfix, lengths, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
        puff_build(&distfix, lengths, MAXDCODES);
        virgin = 0;
    }
    return puff_codes(s, &lenfix, &distfix);
}

static int puff_dynamic(puff_state *s)
{
    static const int order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int nlen, ndist, ncode;
    int n = puff_bits(s, 5); if (n < 0) return n; nlen  = n + 257;
    n = puff_bits(s, 5); if (n < 0) return n; ndist = n + 1;
    n = puff_bits(s, 4); if (n < 0) return n; ncode = n + 4;

    int lengths[MAXCODES];
    memset(lengths, 0, sizeof(lengths));
    for (int i = 0; i < ncode; i++) {
        n = puff_bits(s, 3); if (n < 0) return n;
        lengths[order[i]] = n;
    }
    huffman lencode;
    if (puff_build(&lencode, lengths, 19) < 0) return -9;

    int index = 0;
    while (index < nlen + ndist) {
        int sym = puff_decode(s, &lencode);
        if (sym < 0) return sym;
        int len = 0, copy = 0;
        if (sym < 16) {
            lengths[index++] = sym;
        } else if (sym == 16) {
            n = puff_bits(s, 2); if (n < 0) return n;
            copy = 3 + n;
            len = index ? lengths[index-1] : 0;
        } else if (sym == 17) {
            n = puff_bits(s, 3); if (n < 0) return n;
            copy = 3 + n;
        } else {
            n = puff_bits(s, 7); if (n < 0) return n;
            copy = 11 + n;
        }
        while (copy--) {
            if (index >= MAXCODES) return -11;
            lengths[index++] = len;
        }
    }

    huffman lenc, distc;
    puff_build(&lenc,  lengths,        nlen);
    puff_build(&distc, lengths + nlen, ndist);
    return puff_codes(s, &lenc, &distc);
}

/*
 * puff_inflate  –  decomprimeer raw deflate data.
 * Retourneert aantal bytes in out (>=0) of negatief bij fout.
 */
static long puff_inflate(const unsigned char *in,  unsigned long inlen,
                          unsigned char       *out, unsigned long outlen)
{
    puff_state s;
    memset(&s, 0, sizeof(s));
    s.in     = in;
    s.inlen  = inlen;
    s.out    = out;
    s.outlen = outlen;

    int last, type, ret;
    do {
        last = puff_bits(&s, 1);
        type = puff_bits(&s, 2);
        if (last < 0 || type < 0) return -1;
        if      (type == 0) ret = puff_stored(&s);
        else if (type == 1) ret = puff_fixed(&s);
        else if (type == 2) ret = puff_dynamic(&s);
        else                return -1;
        if (ret < 0) {
            log_print("[UZ] puff_inflate fout: %d\n", ret);
            return ret;
        }
    } while (!last);

    return (long)s.outpos;
}

/* ════════════════════════════════════════════════════════════════════════════
 * ZIP local file header parser
 * ════════════════════════════════════════════════════════════════════════════ */

#define ZIP_LOCAL_SIG   0x04034b50UL
#define METHOD_STORED   0
#define METHOD_DEFLATE  8
#define ZIP_MAX_PATH    256
#define UNZIP_OUT_MAX   (1024 * 1024)

static unsigned int read_u16(const unsigned char *p)
{
    return ((unsigned int)p[0]) | ((unsigned int)p[1] << 8);
}

static unsigned long read_u32(const unsigned char *p)
{
    return ((unsigned long)p[0])        |
           ((unsigned long)p[1] <<  8)  |
           ((unsigned long)p[2] << 16)  |
           ((unsigned long)p[3] << 24);
}

static void slash_to_backslash(char *s)
{
    for (; *s; s++) if (*s == '/') *s = '\\';
}

static int write_file(const char *path, const unsigned char *data, unsigned long len)
{
    char dir[MAX_PATH_LEN];
    util_dirname(path, dir, sizeof(dir));
    fileops_ensure_dir(dir);

    FILE *f = fopen(path, "wb");
    if (!f) { log_print("[UZ] fopen mislukt: %s\n", path); return -1; }
    size_t written = fwrite(data, 1, (size_t)len, f);
    fclose(f);
    if (written != (size_t)len) {
        log_print("[UZ] fwrite onvolledig: %s\n", path);
        return -1;
    }
    return 0;
}

int unzip_to_dir(const unsigned char *zip_data, int zip_len,
                 const char *dest_dir)
{
    if (!zip_data || zip_len < 30 || !dest_dir) return -1;

    unsigned char *out_buf = (unsigned char *)malloc(UNZIP_OUT_MAX);
    if (!out_buf) { log_print("[UZ] malloc mislukt\n"); return -1; }

    int extracted = 0, pos = 0;

    while (pos + 30 <= zip_len) {
        unsigned long sig = read_u32(zip_data + pos);
        if (sig == 0x02014b50UL || sig == 0x06054b50UL) break;
        if (sig != ZIP_LOCAL_SIG) {
            log_print("[UZ] onverwachte sig 0x%08lx op pos %d\n", sig, pos);
            break;
        }

        unsigned int  method      = read_u16(zip_data + pos +  8);
        unsigned long comp_size   = read_u32(zip_data + pos + 18);
        unsigned long uncomp_size = read_u32(zip_data + pos + 22);
        unsigned int  fname_len   = read_u16(zip_data + pos + 26);
        unsigned int  extra_len   = read_u16(zip_data + pos + 28);
        int           data_offset = pos + 30 + (int)fname_len + (int)extra_len;

        char fname[ZIP_MAX_PATH];
        if (fname_len == 0 || fname_len >= ZIP_MAX_PATH) {
            pos = data_offset + (int)comp_size; continue;
        }
        memcpy(fname, zip_data + pos + 30, fname_len);
        fname[fname_len] = '\0';

        if (data_offset + (int)comp_size > zip_len) {
            log_print("[UZ] '%s' overschrijdt buffer\n", fname); break;
        }

        /* __MACOSX overslaan */
        if (strncmp(fname, "__MACOSX", 8) == 0 || strstr(fname, "/__MACOSX")) {
            pos = data_offset + (int)comp_size; continue;
        }

        /* Map-entry */
        int is_dir = (fname[fname_len-1] == '/' || fname[fname_len-1] == '\\');
        if (is_dir) {
            char bs[ZIP_MAX_PATH], dir_path[MAX_PATH_LEN];
            strncpy(bs, fname, sizeof(bs)-1); bs[sizeof(bs)-1] = '\0';
            slash_to_backslash(bs);
            int fl = (int)strlen(bs);
            if (fl > 0 && bs[fl-1] == '\\') bs[fl-1] = '\0';
            snprintf(dir_path, sizeof(dir_path), "%s\\%s", dest_dir, bs);
            fileops_ensure_dir(dir_path);
            log_print("[UZ] map: %s\n", dir_path);
            pos = data_offset + (int)comp_size; continue;
        }

        /* Doelpad */
        char bs[ZIP_MAX_PATH], dest_path[MAX_PATH_LEN];
        strncpy(bs, fname, sizeof(bs)-1); bs[sizeof(bs)-1] = '\0';
        slash_to_backslash(bs);
        snprintf(dest_path, sizeof(dest_path), "%s\\%s", dest_dir, bs);
        log_print("[UZ] %s (%lu bytes)\n", dest_path, uncomp_size);

        const unsigned char *entry = zip_data + data_offset;

        if (method == METHOD_STORED) {
            if (write_file(dest_path, entry, comp_size) == 0) extracted++;
        }
        else if (method == METHOD_DEFLATE) {
            if (uncomp_size > UNZIP_OUT_MAX) {
                log_print("[UZ] te groot (%lu), skip\n", uncomp_size);
                pos = data_offset + (int)comp_size; continue;
            }
            long result = puff_inflate(entry, comp_size, out_buf, UNZIP_OUT_MAX);
            if (result < 0) {
                log_print("[UZ] inflate mislukt: %ld\n", result);
            } else {
                if (write_file(dest_path, out_buf, (unsigned long)result) == 0)
                    extracted++;
            }
        }
        else {
            log_print("[UZ] onbekende methode %u, skip\n", method);
        }

        pos = data_offset + (int)comp_size;
    }

    free(out_buf);
    log_print("[UZ] klaar: %d bestanden uitgepakt\n", extracted);
    return extracted;
}

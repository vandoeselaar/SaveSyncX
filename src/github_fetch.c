/*
 * github_fetch.c  –  Minimale HTTPS GET van raw.githubusercontent.com
 *
 * Certificaatverificatie is UITGESCHAKELD via een permissive x509 class.
 * Dit is bewust: we verbinden altijd met dezelfde bekende host en een
 * MITM bij een savegame-download is geen reëel risico.
 *
 * Gebaseerd op nxdk-bearssl-tls-test (vandoeselaar).
 */

#include "github_fetch.h"
#include "../lib/util.h"   /* voor log_print */

#include "bearssl_inc/bearssl.h"
#include "trust_anchors.h"

#include <lwip/sockets.h>
#include <lwip/netdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GITHUB_RAW_HOST  "raw.githubusercontent.com"
#define GITHUB_RAW_PORT  443

/* ── Entropy ─────────────────────────────────────────────────────────────── */
#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>

static void seed_entropy(unsigned char *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        ULONGLONG t = KeQueryPerformanceCounter();
        DWORD tick  = GetTickCount();
        out[i] = (unsigned char)(
            (t    >> (i & 7))      ^ (t >> ((i + 11) & 15)) ^
            (tick >> ((i + 1) & 7)) ^ (unsigned char)(i * 0x6D ^ 0xA5)
        );
    }
}

/* ── TCP connect ──────────────────────────────────────────────────────────── */
static int tcp_connect(const char *host, int port)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    log_print("[GH] DNS resolving %s...\n", host);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        log_print("[GH] DNS failed\n");
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    if (connect(fd, res->ai_addr, (int)res->ai_addrlen) != 0) {
        log_print("[GH] connect() failed\n");
        closesocket(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    log_print("[GH] TCP connected\n");
    return fd;
}

int github_test_connection(void) {
    int fd = tcp_connect(GITHUB_RAW_HOST, GITHUB_RAW_PORT);
    if (fd < 0) return 0;
    closesocket(fd);
    return 1;
}

/* ── BearSSL engine driver ────────────────────────────────────────────────── */
/*
 * Rijdt SENDREC/RECVREC totdat want_flag actief is.
 * SENDREC krijgt altijd voorrang (anti-deadlock).
 * Gebruikt blocking recv() — select() werkt niet betrouwbaar op nxdk lwIP.
 */
static int engine_run(br_ssl_engine_context *eng, int fd, unsigned want_flag)
{
    for (;;) {
        unsigned st = br_ssl_engine_current_state(eng);

        if (st & BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(eng);
            if (err != BR_ERR_OK)
                log_print("[GH] TLS closed, err=%d\n", err);
            return -1;
        }

        if (st & want_flag)
            return 0;

        if (st & BR_SSL_SENDREC) {
            size_t len;
            unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
            int r = send(fd, (const char *)buf, (int)len, 0);
            if (r <= 0) { log_print("[GH] send error\n"); return -1; }
            br_ssl_engine_sendrec_ack(eng, (size_t)r);
            continue;
        }

        if (st & BR_SSL_RECVREC) {
            size_t len;
            unsigned char *buf = br_ssl_engine_recvrec_buf(eng, &len);
            int r = recv(fd, (char *)buf, (int)len, 0);
            if (r <= 0) { log_print("[GH] recv EOF/err\n"); return -1; }
            br_ssl_engine_recvrec_ack(eng, (size_t)r);
            continue;
        }

        log_print("[GH] engine stall, state=0x%x\n", st);
        return -1;
    }
}

static int tls_flush(br_ssl_engine_context *eng, int fd)
{
    for (;;) {
        unsigned st = br_ssl_engine_current_state(eng);
        if (st & BR_SSL_CLOSED) return -1;
        if (!(st & BR_SSL_SENDREC)) return 0;
        size_t len;
        unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
        int r = send(fd, (const char *)buf, (int)len, 0);
        if (r <= 0) return -1;
        br_ssl_engine_sendrec_ack(eng, (size_t)r);
    }
}

static int tls_write(br_ssl_engine_context *eng, int fd,
                     const char *data, size_t len)
{
    while (len > 0) {
        unsigned st = br_ssl_engine_current_state(eng);
        if (st & BR_SSL_CLOSED) return -1;
        if (!(st & BR_SSL_SENDAPP)) {
            if (engine_run(eng, fd, BR_SSL_SENDAPP) < 0) return -1;
            continue;
        }
        size_t avail;
        unsigned char *buf = br_ssl_engine_sendapp_buf(eng, &avail);
        size_t chunk = (avail < len) ? avail : len;
        memcpy(buf, data, chunk);
        br_ssl_engine_sendapp_ack(eng, chunk);
        br_ssl_engine_flush(eng, 0);
        data += chunk;
        len  -= chunk;
    }
    return tls_flush(eng, fd);
}

static int tls_read(br_ssl_engine_context *eng, int fd,
                    char *out, size_t maxlen)
{
    if (engine_run(eng, fd, BR_SSL_RECVAPP) < 0) {
        if (br_ssl_engine_current_state(eng) & BR_SSL_CLOSED)
            if (br_ssl_engine_last_error(eng) == BR_ERR_OK) return 0;
        return -1;
    }
    size_t avail;
    unsigned char *buf = br_ssl_engine_recvapp_buf(eng, &avail);
    size_t chunk = (avail < maxlen) ? avail : maxlen;
    memcpy(out, buf, chunk);
    br_ssl_engine_recvapp_ack(eng, chunk);
    return (int)chunk;
}

/* ── Hoofdfunctie ─────────────────────────────────────────────────────────── */
int github_fetch_raw(const char *path, char *out_buf, int out_size)
{
    int fd = tcp_connect(GITHUB_RAW_HOST, GITHUB_RAW_PORT);
    if (fd < 0) return -1;

    /* BearSSL buffers op heap */
    unsigned char *iobuf = (unsigned char *)malloc(BR_SSL_BUFSIZE_BIDI);
    if (!iobuf) {
        log_print("[GH] malloc iobuf failed\n");
        closesocket(fd);
        return -1;
    }

    /*
     * Initialiseer precies zoals de werkende tls-test:
     * buffer eerst, dan init_full met echte TAs, dan entropy.
     * raw.githubusercontent.com heeft een geldig publiek cert, dus
     * we doen gewoon volledige verificatie — geen no-op vtable nodig.
     */
    br_ssl_client_context sc;
    br_x509_minimal_context xc;

    br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
    br_ssl_engine_set_buffer(&sc.eng, iobuf, BR_SSL_BUFSIZE_BIDI, 1);

    unsigned char seed[64];
    seed_entropy(seed, sizeof(seed));
    br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof(seed));

    log_print("[GH] TLS handshake...\n");
    br_ssl_client_reset(&sc, GITHUB_RAW_HOST, 0);

    /* Wacht op SENDAPP (handshake klaar) */
    if (engine_run(&sc.eng, fd, BR_SSL_SENDAPP) < 0) {
        log_print("[GH] handshake failed\n");
        free(iobuf); closesocket(fd);
        return -1;
    }
    log_print("[GH] handshake OK\n");

    /* HTTP GET request */
    char req[512];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: " GITHUB_RAW_HOST "\r\n"
             "Connection: close\r\n"
             "User-Agent: SaveSyncX/1.0\r\n"
             "\r\n",
             path);

    log_print("[GH] GET %s\n", path);
    if (tls_write(&sc.eng, fd, req, strlen(req)) < 0) {
        log_print("[GH] write failed\n");
        free(iobuf); closesocket(fd);
        return -1;
    }

    /*
     * Lees volledige response in tijdelijke buffer.
     *
     * raw_size moet groot genoeg zijn voor:
     *   - HTTP headers (~1–2 KB)
     *   - chunk-size regels als GitHub chunked transfer gebruikt
     *     (elke chunk heeft een hex-getal + \r\n overhead, typisch
     *      1–8 bytes per chunk van ~16 KB; worst-case ±5% overhead)
     *   - de eigenlijke body (out_size bytes)
     *
     * We reserveren headers + 10% overhead boven out_size.
     */
    int   raw_size = out_size + out_size / 10 + 8192;
    char *raw_buf  = (char *)malloc(raw_size);
    if (!raw_buf) {
        log_print("[GH] malloc raw_buf failed\n");
        free(iobuf); closesocket(fd);
        return -1;
    }

    int total = 0;
    for (;;) {
        int room = raw_size - 1 - total;
        if (room <= 0) break;
        int r = tls_read(&sc.eng, fd, raw_buf + total, (size_t)room);
        if (r <= 0) break;
        total += r;
    }
    raw_buf[total] = '\0';

    /* Netjes afsluiten */
    br_ssl_engine_close(&sc.eng);
    tls_flush(&sc.eng, fd);
    closesocket(fd);
    free(iobuf);

    log_print("[GH] %d bytes ontvangen\n", total);

    /* Log altijd de HTTP statusregel (eerste regel van raw_buf) */
    {
        char status_line[64];
        int sl = 0;
        while (sl < 63 && raw_buf[sl] && raw_buf[sl] != '\r' && raw_buf[sl] != '\n') {
            status_line[sl] = raw_buf[sl];
            sl++;
        }
        status_line[sl] = '\0';
        log_print("[GH] HTTP status: %s\n", status_line);
    }

    /*
     * Zoek header/body scheiding binary-safe: zoek \r\n\r\n als
     * byte-reeks. strstr stopt bij nul-bytes in binary responses.
     */
    int header_end = -1;
    for (int i = 0; i <= total - 4; i++) {
        if (raw_buf[i]   == '\r' && raw_buf[i+1] == '\n' &&
            raw_buf[i+2] == '\r' && raw_buf[i+3] == '\n') {
            header_end = i + 4;
            break;
        }
    }
    if (header_end < 0) {
        log_print("[GH] geen HTTP header scheiding gevonden\n");
        free(raw_buf);
        return -1;
    }

    /* Controleer HTTP status */
    if (strncmp(raw_buf, "HTTP/1.1 200", 12) != 0 &&
        strncmp(raw_buf, "HTTP/1.0 200", 12) != 0) {
        log_print("[GH] niet-200 response, afgebroken\n");
        free(raw_buf);
        return -1;
    }

    /*
     * Detecteer chunked transfer encoding door de headers te scannen.
     * We zoeken naar "Transfer-Encoding: chunked" (case-insensitief
     * is niet nodig — GitHub stuurt het altijd lowercase).
     */
    int chunked = 0;
    {
        /* Tijdelijk nul-terminate na de headers voor strstr */
        char saved = raw_buf[header_end];
        raw_buf[header_end] = '\0';
        if (strstr(raw_buf, "Transfer-Encoding: chunked") != NULL)
            chunked = 1;
        raw_buf[header_end] = saved;
    }
    log_print("[GH] chunked=%d\n", chunked);

    const char *body_src = raw_buf + header_end;
    int         body_raw = total - header_end;
    int         body_len = 0;

    if (chunked) {
        /*
         * Chunked decoder: elk chunk heeft de vorm
         *   <hex-getal>\r\n
         *   <data van dat aantal bytes>\r\n
         * Afgesloten met:
         *   0\r\n
         *   \r\n
         *
         * We schrijven gedecodeerde bytes direct naar out_buf.
         */
        const char *p   = body_src;
        const char *end = body_src + body_raw;

        while (p < end) {
            /* Lees chunk-grootte als hex */
            char *crlf = NULL;
            /* Zoek \r\n na de hex-waarde */
            for (const char *q = p; q < end - 1; q++) {
                if (q[0] == '\r' && q[1] == '\n') {
                    crlf = (char *)q;
                    break;
                }
            }
            if (!crlf) break;   /* afgekapt — stop */

            int chunk_size = 0;
            for (const char *h = p; h < crlf; h++) {
                char c = *h;
                if      (c >= '0' && c <= '9') chunk_size = chunk_size * 16 + (c - '0');
                else if (c >= 'a' && c <= 'f') chunk_size = chunk_size * 16 + (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') chunk_size = chunk_size * 16 + (c - 'A' + 10);
                else break;   /* extensies na ';' negeren */
            }

            if (chunk_size == 0) break;   /* laatste chunk */

            p = crlf + 2;   /* sla \r\n over */

            /* Kopieer chunk-data naar out_buf */
            int copy = chunk_size;
            if (body_len + copy >= out_size)
                copy = out_size - 1 - body_len;
            if (copy > 0) {
                memcpy(out_buf + body_len, p, copy);
                body_len += copy;
            }
            p += chunk_size + 2;   /* sla data + afsluitende \r\n over */
        }
    } else {
        /* Geen chunked: gewone body, direct kopiëren */
        body_len = body_raw;
        if (body_len >= out_size) body_len = out_size - 1;
        memcpy(out_buf, body_src, body_len);
    }

    out_buf[body_len] = '\0';

    free(raw_buf);
    log_print("[GH] body: %d bytes (chunked=%d)\n", body_len, chunked);
    return body_len;
}

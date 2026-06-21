/*
 * webdav.c  –  WebDAV client over LwIP/TCP (nxdk)
 *
 * Implements PUT, GET, MKCOL, PROPFIND (directory listing),
 * and recursive upload_dir.
 *
 * v1.3: optionele TLS via BearSSL. Roep webdav_init(cfg) aan vanuit
 * main.c na config_load(). Alle bestaande callers (backup.c, restore.c)
 * hoeven niets te veranderen — de TLS-keuze zit in de module-state.
 */

#include "webdav.h"
#include "config.h"
#include "../lib/util.h"
#include "../lib/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

#include "bearssl_inc/bearssl.h"
#include "trust_anchors.h"
#include <xboxkrnl/xboxkrnl.h>   /* KeQueryPerformanceCounter voor entropy */

/* ── Module-state (gezet door webdav_init) ───────────────────────── */
static int  g_use_tls       = 0;
static int  g_tls_no_verify = 0;

void webdav_init(const AppConfig *cfg)
{
    g_use_tls       = cfg->use_tls;
    g_tls_no_verify = cfg->tls_no_verify;
    log_print("[WebDAV] init: use_tls=%d tls_no_verify=%d\n",
              g_use_tls, g_tls_no_verify);
}

/* ── Base64 ──────────────────────────────────────────────────────── */
static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void webdav_base64_encode(const unsigned char *in, int len, char *out)
{
    int i, j = 0;
    for (i = 0; i < len; i += 3) {
        unsigned int v = (unsigned char)in[i] << 16;
        if (i + 1 < len) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned char)in[i + 2];
        out[j++] = b64[(v >> 18) & 0x3F];
        out[j++] = b64[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? b64[(v >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < len) ? b64[v & 0x3F]        : '=';
    }
    out[j] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════
 * WdavConn  –  abstractielaag: plain TCP of BearSSL TLS
 *
 * Gebruik:
 *   WdavConn c;
 *   if (wdav_connect(&c, host, port) < 0) { ... fout ... }
 *   wdav_send(&c, buf, len);
 *   wdav_recv(&c, buf, len);
 *   wdav_close(&c);
 *
 * Intern schakelt wdav_connect op basis van g_use_tls.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    int  fd;                          /* lwip socket */
    int  is_tls;

    /* BearSSL state — alleen geldig als is_tls=1 */
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_x509_knownkey_context knownkey_ctx; /* voor no-verify modus */
    unsigned char *iobuf;             /* BR_SSL_BUFSIZE_BIDI, heap */
} WdavConn;

/* ── Entropy helper (zelfde als github_fetch.c) ──────────────────── */
static void wdav_seed_entropy(unsigned char *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        ULONGLONG t = KeQueryPerformanceCounter();
        DWORD tick  = GetTickCount();
        out[i] = (unsigned char)(
            (t    >> (i & 7))       ^ (t >> ((i + 11) & 15)) ^
            (tick >> ((i + 1) & 7)) ^ (unsigned char)(i * 0x6D ^ 0xA5)
        );
    }
}

/* ── No-verify x509 vtable ───────────────────────────────────────── */
/*
 * Wanneer tls_no_verify=1 (bijv. Nextcloud met zelfgesigneerd cert)
 * gebruiken we een minimale x509 validator die altijd slaagt.
 * Dezelfde techniek als in de BearSSL documentatie.
 */
/*
 * No-verify x509 vtable.
 *
 * De functies accepteren void* als eerste argument zodat we niet
 * afhankelijk zijn van de exacte const-kwalificatie die BearSSL's
 * br_x509_class struct leden declareren (varieert per versie).
 * We casten de functiepointers bij toewijzing — UB-vrij omdat alle
 * aanroepen via dezelfde ABI verlopen (één pointer-argument).
 *
 * nv_end_chain geeft altijd 0 (= geen fout) zodat elk certificaat
 * geaccepteerd wordt.
 */
static void     nv_start_chain_fn(void *ctx, const char *n) { (void)ctx; (void)n; }
static void     nv_start_cert_fn (void *ctx, uint32_t l)    { (void)ctx; (void)l; }
static void     nv_append_fn     (void *ctx, const unsigned char *b, size_t l)
                                  { (void)ctx; (void)b; (void)l; }
static void     nv_end_cert_fn   (void *ctx)                { (void)ctx; }
static unsigned nv_end_chain_fn  (void *ctx)                { (void)ctx; return 0; }
static const br_x509_pkey *nv_get_pkey_fn(void *ctx, unsigned *u)
{
    (void)ctx;
    if (u) *u = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return NULL;
}

/* Vtable wordt runtime gevuld om compiler-warnings over
   functiepointer-incompatibiliteit te vermijden. */
static br_x509_class nv_vtable;
static int           nv_vtable_ready = 0;

static void nv_vtable_init(void)
{
    if (nv_vtable_ready) return;
    memset(&nv_vtable, 0, sizeof(nv_vtable));
    nv_vtable.context_size = sizeof(br_x509_class *);
    /* Expliciete void*-casts: vermijdt -Wincompatible-function-pointer-types */
    memcpy(&nv_vtable.start_chain, &(void*){(void*)nv_start_chain_fn}, sizeof(void*));
    memcpy(&nv_vtable.start_cert,  &(void*){(void*)nv_start_cert_fn},  sizeof(void*));
    memcpy(&nv_vtable.append,      &(void*){(void*)nv_append_fn},      sizeof(void*));
    memcpy(&nv_vtable.end_cert,    &(void*){(void*)nv_end_cert_fn},    sizeof(void*));
    memcpy(&nv_vtable.end_chain,   &(void*){(void*)nv_end_chain_fn},   sizeof(void*));
    memcpy(&nv_vtable.get_pkey,    &(void*){(void*)nv_get_pkey_fn},    sizeof(void*));
    nv_vtable_ready = 1;
}

/* ── BearSSL engine driver (zelfde patroon als github_fetch.c) ───── */
static int tls_engine_run(br_ssl_engine_context *eng, int fd,
                          unsigned want_flag)
{
    for (;;) {
        unsigned st = br_ssl_engine_current_state(eng);

        if (st & BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(eng);
            if (err != BR_ERR_OK)
                log_print("[TLS] gesloten, err=%d\n", err);
            return -1;
        }

        if (st & want_flag) return 0;

        if (st & BR_SSL_SENDREC) {
            size_t len;
            unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
            int r = lwip_send(fd, (const char *)buf, (int)len, 0);
            if (r <= 0) { log_print("[TLS] send fout\n"); return -1; }
            br_ssl_engine_sendrec_ack(eng, (size_t)r);
            continue;
        }

        if (st & BR_SSL_RECVREC) {
            size_t len;
            unsigned char *buf = br_ssl_engine_recvrec_buf(eng, &len);
            int r = lwip_recv(fd, (char *)buf, (int)len, 0);
            if (r <= 0) { log_print("[TLS] recv EOF/fout\n"); return -1; }
            br_ssl_engine_recvrec_ack(eng, (size_t)r);
            continue;
        }

        log_print("[TLS] engine stall, state=0x%x\n", st);
        return -1;
    }
}

static int tls_flush(WdavConn *c)
{
    br_ssl_engine_context *eng = &c->sc.eng;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(eng);
        if (st & BR_SSL_CLOSED) return -1;
        if (!(st & BR_SSL_SENDREC)) return 0;
        size_t len;
        unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
        int r = lwip_send(c->fd, (const char *)buf, (int)len, 0);
        if (r <= 0) return -1;
        br_ssl_engine_sendrec_ack(eng, (size_t)r);
    }
}

/* ── wdav_connect ────────────────────────────────────────────────── */
static int wdav_connect(WdavConn *c, const char *host, int port)
{
    memset(c, 0, sizeof(*c));
    c->fd     = -1;
    c->iobuf  = NULL;
    c->is_tls = g_use_tls;

    /* TCP socket — gebruik getaddrinfo zodat zowel IP-adressen als
       hostnames (bijv. app.koofr.net) werken. */
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    log_print("[WebDAV] DNS resolving %s...\n", host);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        log_print("[WebDAV] DNS mislukt voor %s\n", host);
        return -1;
    }

    int sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { freeaddrinfo(res); return -1; }

    struct timeval tv = { 15, 0 };
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (lwip_connect(sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
        log_print("[WebDAV] connect() mislukt voor %s:%d\n", host, port);
        freeaddrinfo(res);
        lwip_close(sock);
        return -1;
    }
    freeaddrinfo(res);
    c->fd = sock;

    if (!c->is_tls) return 0;   /* plain HTTP klaar */

    /* ── TLS handshake ───────────────────────────────────────────── */
    c->iobuf = (unsigned char *)malloc(BR_SSL_BUFSIZE_BIDI);
    if (!c->iobuf) {
        log_print("[TLS] malloc iobuf mislukt\n");
        lwip_close(c->fd); c->fd = -1;
        return -1;
    }

    if (g_tls_no_verify) {
        /*
         * No-verify modus: vervang de x509-validator door onze permissive
         * vtable zodat zelfgesigneerde certificaten geaccepteerd worden.
         */
        nv_vtable_init();
        const br_x509_class *nv_ptr = &nv_vtable;
        br_ssl_client_init_full(&c->sc, &c->xc, TAs, TAs_NUM);
        br_ssl_engine_set_x509(&c->sc.eng,
                               (const br_x509_class **)&nv_ptr);
        log_print("[TLS] no-verify modus (zelfgesigneerd cert)\n");
    } else {
        br_ssl_client_init_full(&c->sc, &c->xc, TAs, TAs_NUM);
    }

    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, BR_SSL_BUFSIZE_BIDI, 1);

    unsigned char seed[64];
    wdav_seed_entropy(seed, sizeof(seed));
    br_ssl_engine_inject_entropy(&c->sc.eng, seed, sizeof(seed));

    br_ssl_client_reset(&c->sc, host, 0);

    if (tls_engine_run(&c->sc.eng, c->fd, BR_SSL_SENDAPP) < 0) {
        log_print("[TLS] handshake mislukt voor %s:%d\n", host, port);
        free(c->iobuf); c->iobuf = NULL;
        lwip_close(c->fd); c->fd = -1;
        return -1;
    }
    log_print("[TLS] handshake OK met %s:%d\n", host, port);
    return 0;
}

/* ── wdav_send ───────────────────────────────────────────────────── */
static int wdav_send(WdavConn *c, const char *data, int len)
{
    if (!c->is_tls) {
        return lwip_send(c->fd, data, len, 0);
    }

    /* TLS: schrijf in chunks van beschikbare SENDAPP-buffer */
    const char *p   = data;
    int         rem = len;
    while (rem > 0) {
        unsigned st = br_ssl_engine_current_state(&c->sc.eng);
        if (st & BR_SSL_CLOSED) return -1;
        if (!(st & BR_SSL_SENDAPP)) {
            if (tls_engine_run(&c->sc.eng, c->fd, BR_SSL_SENDAPP) < 0)
                return -1;
            continue;
        }
        size_t avail;
        unsigned char *buf = br_ssl_engine_sendapp_buf(&c->sc.eng, &avail);
        int chunk = (int)avail < rem ? (int)avail : rem;
        memcpy(buf, p, chunk);
        br_ssl_engine_sendapp_ack(&c->sc.eng, (size_t)chunk);
        br_ssl_engine_flush(&c->sc.eng, 0);
        p   += chunk;
        rem -= chunk;
    }
    return tls_flush(c) < 0 ? -1 : len;
}

/* ── wdav_recv ───────────────────────────────────────────────────── */
/*
 * Leest maximaal maxlen bytes in buf.
 * Geeft aantal ontvangen bytes terug, 0 bij EOF, -1 bij fout.
 */
static int wdav_recv(WdavConn *c, char *buf, int maxlen)
{
    if (!c->is_tls) {
        return lwip_recv(c->fd, buf, maxlen, 0);
    }

    /* Wacht tot RECVAPP beschikbaar is */
    if (tls_engine_run(&c->sc.eng, c->fd, BR_SSL_RECVAPP) < 0) {
        unsigned st = br_ssl_engine_current_state(&c->sc.eng);
        if ((st & BR_SSL_CLOSED) &&
            br_ssl_engine_last_error(&c->sc.eng) == BR_ERR_OK)
            return 0;   /* nette EOF */
        return -1;
    }
    size_t avail;
    unsigned char *tbuf = br_ssl_engine_recvapp_buf(&c->sc.eng, &avail);
    int chunk = (int)avail < maxlen ? (int)avail : maxlen;
    memcpy(buf, tbuf, chunk);
    br_ssl_engine_recvapp_ack(&c->sc.eng, (size_t)chunk);
    return chunk;
}

/* ── wdav_close ──────────────────────────────────────────────────── */
static void wdav_close(WdavConn *c)
{
    if (c->is_tls && c->fd >= 0) {
        br_ssl_engine_close(&c->sc.eng);
        /* Flush eventuele close_notify — fouten negeren we hier */
        tls_flush(c);
    }
    if (c->fd >= 0) { lwip_close(c->fd); c->fd = -1; }
    if (c->iobuf)   { free(c->iobuf); c->iobuf = NULL; }
}

/* ═══════════════════════════════════════════════════════════════════
 * HTTP helpers  –  lezen via WdavConn
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Lees één regel van de verbinding ───────────────────────────── */
static int read_line(WdavConn *c, char *buf, int bufsz)
{
    int i = 0;
    char ch;
    while (wdav_recv(c, &ch, 1) == 1) {
        if (ch == '\n') {
            if (i > 0 && buf[i-1] == '\r') i--;
            break;
        }
        if (i < bufsz - 1) buf[i++] = ch;
    }
    buf[i] = '\0';
    return i;
}

/* ── Lees HTTP statuscode en headers ────────────────────────────── */
/*
 * content_len_out wordt gevuld met:
 *   > 0  : exacte Content-Length van de server
 *     0  : geen Content-Length en geen chunked → leeg of onbekend
 *    -1  : Transfer-Encoding: chunked
 */
static int read_http_status(WdavConn *c, long *content_len_out)
{
    char line[512];
    int  status      = 0;
    long content_len = 0;
    int  chunked     = 0;

    if (read_line(c, line, sizeof(line)) > 0) {
        if (strlen(line) >= 12)
            sscanf(line + 9, "%d", &status);
    } else return 0;

    while (1) {
        int n = read_line(c, line, sizeof(line));
        if (n <= 0) break;
        if (util_strncasecmp(line, "Content-Length:", 15) == 0)
            content_len = atol(line + 15);
        if (util_strncasecmp(line, "Transfer-Encoding:", 18) == 0 &&
            strstr(line + 18, "chunked"))
            chunked = 1;
        if (line[0] == '\0') break;   /* lege regel = einde headers */
    }

    if (content_len_out) *content_len_out = chunked ? -1L : content_len;
    return status;
}

static int drain_body(WdavConn *c, long content_len)
{
    char drain[256];
    long rem = content_len;
    while (rem > 0) {
        int want = rem > (long)sizeof(drain) ? (int)sizeof(drain) : (int)rem;
        int r = wdav_recv(c, drain, want);
        if (r <= 0) break;
        rem -= r;
    }
    return 0;
}

/* ── URL-encode een pad ──────────────────────────────────────────── */
static void url_encode_path(const char *in, char *out, int out_sz)
{
    static const char safe[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789/-._~";
    int j = 0;
    for (int i = 0; in[i] && j < out_sz - 3; i++) {
        unsigned char c = (unsigned char)in[i];
        if (strchr(safe, c)) {
            out[j++] = c;
        } else {
            out[j++] = '%';
            out[j++] = "0123456789ABCDEF"[c >> 4];
            out[j++] = "0123456789ABCDEF"[c & 0xF];
        }
    }
    out[j] = '\0';
}

/* ── url_decode ──────────────────────────────────────────────────── */
static void url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char h = r[1], l = r[2];
            int hi = (h >= '0' && h <= '9') ? h - '0' :
                     (h >= 'A' && h <= 'F') ? h - 'A' + 10 :
                     (h >= 'a' && h <= 'f') ? h - 'a' + 10 : -1;
            int lo = (l >= '0' && l <= '9') ? l - '0' :
                     (l >= 'A' && l <= 'F') ? l - 'A' + 10 :
                     (l >= 'a' && l <= 'f') ? l - 'a' + 10 : -1;
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* ═══════════════════════════════════════════════════════════════════
 * webdav_request
 * ═══════════════════════════════════════════════════════════════════ */
int webdav_request(const char *method, const char *remote_path,
                   const char *host, int port, const char *creds64,
                   const char *extra_headers,
                   const char *body, int body_len,
                   char *resp_buf, int resp_sz,
                   int *status_out)
{
    char encoded[MAX_PATH_LEN * 3];
    url_encode_path(remote_path, encoded, sizeof(encoded));

    WdavConn c;
    if (wdav_connect(&c, host, port) < 0) {
        log_print("%s %s -> verbinding mislukt\n", method, remote_path);
        if (status_out) *status_out = -1;
        return -1;
    }

    char request[4096];
    int req_len = snprintf(request, sizeof(request),
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Length: %d\r\n"
        "%s"
        "Connection: close\r\n"
        "\r\n",
        method, encoded, host, port, creds64,
        body_len,
        extra_headers ? extra_headers : "");

    if (req_len <= 0 || req_len >= (int)sizeof(request)) {
        log_print("%s %s -> request header te groot (%d bytes)\n",
                  method, remote_path, req_len);
        wdav_close(&c);
        if (status_out) *status_out = -1;
        return -1;
    }

    log_print("%s %s -> verstuur request (%d bytes header, %d bytes body)\n",
              method, remote_path, req_len, body_len);

    wdav_send(&c, request, req_len);
    if (body && body_len > 0)
        wdav_send(&c, body, body_len);

    long content_len = 0;
    int status = read_http_status(&c, &content_len);
    log_print("%s %s -> HTTP %d  content_len=%ld\n",
              method, remote_path, status, content_len);
    if (status_out) *status_out = status;

    int received = 0;
    if (resp_buf && resp_sz > 0) {
        if (content_len > 0) {
            long rem = content_len;
            while (rem > 0 && received < resp_sz - 1) {
                int want = (int)(rem < (resp_sz - 1 - received)
                                 ? rem : (resp_sz - 1 - received));
                int r = wdav_recv(&c, resp_buf + received, want);
                if (r <= 0) break;
                received += r;
                rem -= r;
            }
        } else {
            while (received < resp_sz - 1) {
                int want = resp_sz - 1 - received;
                int r = wdav_recv(&c, resp_buf + received, want);
                if (r <= 0) break;
                received += r;
            }
        }
        resp_buf[received] = '\0';
    } else {
        if (content_len > 0) drain_body(&c, content_len);
        else { char d[256]; while (wdav_recv(&c, d, sizeof(d)) > 0) {} }
    }

    wdav_close(&c);
    return received;
}

/* ═══════════════════════════════════════════════════════════════════
 * webdav_put_file
 * ═══════════════════════════════════════════════════════════════════ */
int webdav_put_file(const char *local_path, const char *remote_path,
                    const char *creds64, const char *host, int port)
{
    HANDLE hf = CreateFile(local_path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        log_print("PUT: kan %s niet openen\n", local_path);
        return -1;
    }
    DWORD file_size = GetFileSize(hf, NULL);
    char *body = (char *)malloc(file_size);
    if (!body) { CloseHandle(hf); return -1; }
    DWORD bytes_read;
    ReadFile(hf, body, file_size, &bytes_read, NULL);
    CloseHandle(hf);

    int status;
    char cl_header[64];
    snprintf(cl_header, sizeof(cl_header), "Content-Type: application/octet-stream\r\n");
    webdav_request("PUT", remote_path, host, port, creds64,
                   cl_header, body, (int)bytes_read, NULL, 0, &status);
    free(body);

    log_print("PUT %s -> %d\n", remote_path, status);
    return status;
}

/* ═══════════════════════════════════════════════════════════════════
 * webdav_get_file
 * ═══════════════════════════════════════════════════════════════════ */
int webdav_get_file(const char *remote_path, const char *local_path,
                    const char *creds64, const char *host, int port)
{
    char encoded[MAX_PATH_LEN * 3];
    url_encode_path(remote_path, encoded, sizeof(encoded));

    WdavConn c;
    if (wdav_connect(&c, host, port) < 0) {
        log_print("GET %s -> verbinding mislukt\n", remote_path);
        return -1;
    }

    char request[4096];
    int req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Authorization: Basic %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        encoded, host, port, creds64);

    if (req_len <= 0 || req_len >= (int)sizeof(request)) {
        log_print("GET %s -> request header te groot (%d bytes)\n",
                  remote_path, req_len);
        wdav_close(&c);
        return -1;
    }

    wdav_send(&c, request, req_len);

    long content_len = 0;
    int status = read_http_status(&c, &content_len);
    log_print("GET %s -> HTTP %d  content_len=%ld\n",
              remote_path, status, content_len);

    if (status != 200) {
        drain_body(&c, content_len > 0 ? content_len : 0);
        wdav_close(&c);
        return -1;
    }

    HANDLE hf = CreateFile(local_path, GENERIC_WRITE, 0,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        log_print("GET %s -> CreateFile mislukt voor %s\n",
                  remote_path, local_path);
        drain_body(&c, content_len > 0 ? content_len : 0);
        wdav_close(&c);
        return -1;
    }

    char *buf = (char *)malloc(TRANSFER_BUF_SZ);
    if (!buf) { CloseHandle(hf); wdav_close(&c); return -1; }

    long total_written = 0;
    int  ok            = 1;

    if (content_len > 0) {
        long rem = content_len;
        while (rem > 0) {
            int want = (int)(rem > (long)TRANSFER_BUF_SZ
                             ? (long)TRANSFER_BUF_SZ : rem);
            int r = wdav_recv(&c, buf, want);
            if (r <= 0) { ok = 0; break; }
            DWORD written;
            WriteFile(hf, buf, r, &written, NULL);
            total_written += r;
            rem -= r;
        }
    } else {
        while (1) {
            int r = wdav_recv(&c, buf, TRANSFER_BUF_SZ);
            if (r <= 0) break;
            DWORD written;
            WriteFile(hf, buf, r, &written, NULL);
            total_written += r;
        }
        if (total_written == 0) ok = 0;
    }

    log_print("GET %s -> %ld bytes geschreven, ok=%d\n",
              remote_path, total_written, ok);

    free(buf);
    CloseHandle(hf);
    wdav_close(&c);
    return ok ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════
 * webdav_mkcol
 * ═══════════════════════════════════════════════════════════════════ */
int webdav_mkcol(const char *remote_path,
                 const char *creds64, const char *host, int port)
{
    int status;
    webdav_request("MKCOL", remote_path, host, port, creds64,
                   NULL, NULL, 0, NULL, 0, &status);
    log_print("MKCOL %s -> %d\n", remote_path, status);
    return status;
}

/* ═══════════════════════════════════════════════════════════════════
 * webdav_list_directory / webdav_list_directory_ex
 *
 * list_dir_core doet de PROPFIND direct via WdavConn (niet via
 * webdav_request) omdat het een dynamisch groeiende response-buffer
 * nodig heeft die niet past in de vaste resp_buf van webdav_request.
 * ═══════════════════════════════════════════════════════════════════ */
static int list_dir_core(const char *remote_path,
                          char items[][MAX_PATH_LEN], int is_dir_out[],
                          int max_items,
                          const char *creds64, const char *host, int port,
                          int collections_only)
{
    static const char propfind_body[] =
        "<?xml version=\"1.0\"?>"
        "<D:propfind xmlns:D=\"DAV:\">"
        "<D:prop><D:resourcetype/></D:prop>"
        "</D:propfind>";

    char encoded[MAX_PATH_LEN * 3];
    url_encode_path(remote_path, encoded, sizeof(encoded));

    WdavConn c;
    if (wdav_connect(&c, host, port) < 0) {
        log_print("PROPFIND %s -> verbinding mislukt\n", remote_path);
        return -1;
    }

    char request[4096];
    int req_len = snprintf(request, sizeof(request),
        "PROPFIND %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Authorization: Basic %s\r\n"
        "Depth: 1\r\n"
        "Content-Type: application/xml\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        encoded, host, port, creds64, (int)(sizeof(propfind_body) - 1));

    if (req_len <= 0 || req_len >= (int)sizeof(request)) {
        log_print("PROPFIND %s -> request buffer te klein (%d bytes nodig)\n",
                  remote_path, req_len);
        wdav_close(&c);
        return -1;
    }

    wdav_send(&c, request, req_len);
    wdav_send(&c, propfind_body, (int)(sizeof(propfind_body) - 1));

    long content_len = 0;
    int status = read_http_status(&c, &content_len);
    log_print("PROPFIND %s -> HTTP %d  content_len=%ld\n",
              remote_path, status, content_len);

    if (status != 207) {
        wdav_close(&c);
        return -1;
    }

    /* Dynamisch groeiende buffer */
    int  buf_sz   = 64 * 1024;
    int  received = 0;
    char *resp    = (char *)malloc(buf_sz);
    if (!resp) { wdav_close(&c); return -1; }

    if (content_len > 0) {
        if (content_len + 1 > buf_sz) {
            buf_sz = (int)content_len + 1;
            char *tmp = (char *)realloc(resp, buf_sz);
            if (!tmp) { free(resp); wdav_close(&c); return -1; }
            resp = tmp;
        }
        long rem = content_len;
        while (rem > 0) {
            int want = (int)(rem > 4096 ? 4096 : rem);
            int r = wdav_recv(&c, resp + received, want);
            if (r <= 0) break;
            received += r;
            rem -= r;
        }
    } else {
        while (1) {
            if (received >= buf_sz - 1) {
                buf_sz += 64 * 1024;
                char *tmp = (char *)realloc(resp, buf_sz);
                if (!tmp) break;
                resp = tmp;
            }
            int r = wdav_recv(&c, resp + received, buf_sz - received - 1);
            if (r <= 0) break;
            received += r;
        }
    }
    resp[received] = '\0';
    wdav_close(&c);

    log_print("PROPFIND %s -> %d bytes ontvangen (buf_sz=%d)\n",
              remote_path, received, buf_sz);

    if (received <= 0) { free(resp); return -1; }

    int count = 0;
    char *p = resp;

    /* Normaliseer remote_path: strip trailing slash */
    char base_norm[MAX_PATH_LEN];
    strncpy(base_norm, remote_path, sizeof(base_norm) - 1);
    base_norm[sizeof(base_norm) - 1] = '\0';
    int blen = (int)strlen(base_norm);
    if (blen > 0 && base_norm[blen - 1] == '/') base_norm[--blen] = '\0';

    while (count < max_items) {
        char *resp_start = strstr(p, "<D:response>");
        if (!resp_start) break;
        char *resp_end = strstr(resp_start + 1, "</D:response>");
        if (!resp_end) break;

        int block_len = (int)(resp_end - resp_start) + 13;
        char *block = (char *)malloc(block_len + 1);
        if (!block) { p = resp_end; continue; }
        strncpy(block, resp_start, block_len);
        block[block_len] = '\0';

        int is_collection = (strstr(block, "<D:collection") != NULL);

        char *href_start = strstr(block, "<D:href>");
        if (!href_start) { free(block); p = resp_end; continue; }
        char *val_start = href_start + 8;
        char *val_end   = strchr(val_start, '<');
        if (!val_end) { free(block); p = resp_end; continue; }

        int val_len = (int)(val_end - val_start);
        if (val_len <= 0 || val_len >= MAX_PATH_LEN) {
            free(block); p = resp_end; continue;
        }

        char href[MAX_PATH_LEN];
        strncpy(href, val_start, val_len);
        href[val_len] = '\0';

        free(block);

        /* Strip trailing slash */
        int hlen = (int)strlen(href);
        if (hlen > 0 && href[hlen - 1] == '/') href[--hlen] = '\0';

        char *name = strrchr(href, '/');
        name = name ? name + 1 : href;
        url_decode(name);

        int is_self = (util_strcasecmp(href, base_norm) == 0);

        if (!is_self && (!collections_only || is_collection) && name[0] != '\0') {
            strncpy(items[count], name, MAX_PATH_LEN - 1);
            items[count][MAX_PATH_LEN - 1] = '\0';
            if (is_dir_out) is_dir_out[count] = is_collection;
            log_print("  item[%d]: %s (is_dir=%d)\n",
                      count, name, is_collection);
            count++;
        }

        p = resp_end;
    }

    free(resp);
    log_print("PROPFIND %s -> %d items gevonden\n", remote_path, count);
    return count;
}

int webdav_list_directory(const char *remote_path,
                           char items[][MAX_PATH_LEN], int max_items,
                           const char *creds64, const char *host, int port,
                           int collections_only)
{
    return list_dir_core(remote_path, items, NULL, max_items,
                         creds64, host, port, collections_only);
}

int webdav_list_directory_ex(const char *remote_path,
                              char items[][MAX_PATH_LEN], int is_dir[],
                              int max_items,
                              const char *creds64, const char *host, int port)
{
    return list_dir_core(remote_path, items, is_dir, max_items,
                         creds64, host, port, 0);
}

/* ═══════════════════════════════════════════════════════════════════
 * upload_dir
 * ═══════════════════════════════════════════════════════════════════ */
int upload_dir(const char *local_dir, const char *remote_dir,
               const char *creds64, const char *host, int port,
               TransferState *progress)
{
    int uploaded = 0;

    webdav_mkcol(remote_dir, creds64, host, port);

    char search[MAX_PATH_LEN];
    snprintf(search, sizeof(search), "%s\\*", local_dir);

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    do {
        if (strcmp(fd.cFileName, ".") == 0 ||
            strcmp(fd.cFileName, "..") == 0) continue;

        if (progress && progress->error) break;

        char local_child[MAX_PATH_LEN];
        char remote_child[MAX_PATH_LEN];
        snprintf(local_child,  sizeof(local_child),  "%s\\%s", local_dir,  fd.cFileName);
        snprintf(remote_child, sizeof(remote_child), "%s/%s",  remote_dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            int sub = upload_dir(local_child, remote_child,
                                 creds64, host, port, progress);
            if (sub < 0) { uploaded = -1; break; }
            uploaded += sub;
        } else {
            if (progress) {
                strncpy(progress->current_file, fd.cFileName,
                        sizeof(progress->current_file) - 1);
                progress->current_file[sizeof(progress->current_file) - 1] = '\0';
            }

            int s = webdav_put_file(local_child, remote_child,
                                    creds64, host, port);
            if (s == 200 || s == 201 || s == 204)
                uploaded++;

            if (progress) {
                progress->files_done++;
                uint64_t fsize = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                progress->bytes_done += (size_t)fsize;

                snprintf(progress->log[progress->log_next], TRANSFER_LOG_WIDTH,
                         "%s %s",
                         (s == 200 || s == 201 || s == 204) ? "OK " : "FAIL",
                         fd.cFileName);
                progress->log_next = (progress->log_next + 1) % TRANSFER_LOG_LINES;

                if (s != 200 && s != 201 && s != 204) {
                    snprintf(progress->status_msg, sizeof(progress->status_msg),
                             "Failed: %s (HTTP %d)", fd.cFileName, s);
                } else {
                    progress->status_msg[0] = '\0';
                }

                if (ui_progress(progress) < 0) {
                    progress->error = 1;
                    snprintf(progress->status_msg, sizeof(progress->status_msg),
                             "Cancelled by user");
                    uploaded = -1;
                    break;
                }
            }
        }
    } while (FindNextFile(h, &fd));

    FindClose(h);
    return uploaded;
}

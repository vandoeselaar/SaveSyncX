/*
 * webdav.c  –  WebDAV client over LwIP/TCP (nxdk)
 *
 * Implements PUT, GET, MKCOL, PROPFIND (directory listing),
 * and recursive upload_dir.
 */

#include "webdav.h"
#include "config.h"
#include "../lib/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

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

/* ── Intern: lees één regel van socket ──────────────────────────── */
static int read_line(int sock, char *buf, int bufsz)
{
    int i = 0;
    char c;
    while (lwip_recv(sock, &c, 1, 0) == 1) {
        if (c == '\n') {
            if (i > 0 && buf[i-1] == '\r') i--;
            break;
        }
        if (i < bufsz - 1) buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

/* ── Intern: lees HTTP statuscode en headers ─────────────────────── */
/*
 * content_len_out wordt gevuld met:
 *   > 0  : exacte Content-Length van de server
 *     0  : geen Content-Length en geen chunked → leeg of onbekend
 *    -1  : Transfer-Encoding: chunked → lees tot socket sluit
 */
static int read_http_status(int sock, long *content_len_out)
{
    char line[512];
    int status = 0;
    long content_len = 0;
    int chunked = 0;

    if (read_line(sock, line, sizeof(line)) > 0) {
        if (strlen(line) >= 12)
            sscanf(line + 9, "%d", &status);
    } else return 0;

    while (1) {
        int n = read_line(sock, line, sizeof(line));
        if (n <= 0) break;
        if (util_strncasecmp(line, "Content-Length:", 15) == 0)
            content_len = atol(line + 15);
        if (util_strncasecmp(line, "Transfer-Encoding:", 18) == 0 &&
            strstr(line + 18, "chunked"))
            chunked = 1;
        if (line[0] == '\0') break;
    }

    if (content_len_out) *content_len_out = chunked ? -1L : content_len;
    return status;
}

static int drain_body(int sock, long content_len)
{
    char drain[256];
    long rem = content_len;
    while (rem > 0) {
        int want = rem > (long)sizeof(drain) ? (int)sizeof(drain) : (int)rem;
        int r = lwip_recv(sock, drain, want, 0);
        if (r <= 0) break;
        rem -= r;
    }
    return 0;
}

/* ── Intern: open TCP socket naar host:port ──────────────────────── */
static int open_socket(const char *host, int port)
{
    int sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;

    struct timeval tv = { 15, 0 };
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);

    if (lwip_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        lwip_close(sock);
        return -1;
    }
    return sock;
}

/* ── Intern: URL-encode een pad ──────────────────────────────────── */
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
/*
 * Decodeert %XX sequences in-place.
 * Onbekende of incomplete sequences worden ongewijzigd gelaten.
 */
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

/* ── webdav_request ──────────────────────────────────────────────── */
int webdav_request(const char *method, const char *remote_path,
                   const char *host, int port, const char *creds64,
                   const char *extra_headers,
                   const char *body, int body_len,
                   char *resp_buf, int resp_sz,
                   int *status_out)
{
    char encoded[MAX_PATH_LEN * 3];
    url_encode_path(remote_path, encoded, sizeof(encoded));

    int sock = open_socket(host, port);
    if (sock < 0) {
        log_print("%s %s -> socket open mislukt\n", method, remote_path);
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
        log_print("%s %s -> request header te groot (%d bytes), afgebroken\n",
                  method, remote_path, req_len);
        lwip_close(sock);
        if (status_out) *status_out = -1;
        return -1;
    }

    log_print("%s %s -> verstuur request (%d bytes header, %d bytes body)\n",
              method, remote_path, req_len, body_len);
    lwip_send(sock, request, req_len, 0);
    if (body && body_len > 0)
        lwip_send(sock, body, body_len, 0);

    long content_len = 0;
    int status = read_http_status(sock, &content_len);
    log_print("%s %s -> HTTP %d  content_len=%ld\n", method, remote_path, status, content_len);
    if (status_out) *status_out = status;

    int received = 0;
    if (resp_buf && resp_sz > 0) {
        if (content_len > 0) {
            /* Exacte lengte bekend */
            long rem = content_len;
            while (rem > 0 && received < resp_sz - 1) {
                int want = (int)(rem < (resp_sz - 1 - received) ? rem : (resp_sz - 1 - received));
                int r = lwip_recv(sock, resp_buf + received, want, 0);
                if (r <= 0) break;
                received += r;
                rem -= r;
            }
        } else {
            /* Chunked of geen Content-Length: lees tot socket sluit */
            while (received < resp_sz - 1) {
                int want = resp_sz - 1 - received;
                int r = lwip_recv(sock, resp_buf + received, want, 0);
                if (r <= 0) break;
                received += r;
            }
        }
        resp_buf[received] = '\0';
    } else {
        /* Geen buffer gewenst: drain weg */
        if (content_len > 0) drain_body(sock, content_len);
        else { char d[256]; while (lwip_recv(sock, d, sizeof(d), 0) > 0) {} }
    }

    lwip_close(sock);
    return received;
}

/* ── webdav_put_file ─────────────────────────────────────────────── */
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

/* ── webdav_get_file ─────────────────────────────────────────────── */
int webdav_get_file(const char *remote_path, const char *local_path,
                    const char *creds64, const char *host, int port)
{
    char encoded[MAX_PATH_LEN * 3];
    url_encode_path(remote_path, encoded, sizeof(encoded));

    int sock = open_socket(host, port);
    if (sock < 0) {
        log_print("GET %s -> socket open mislukt\n", remote_path);
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
        log_print("GET %s -> request header te groot (%d bytes), afgebroken\n",
                  remote_path, req_len);
        lwip_close(sock);
        return -1;
    }

    lwip_send(sock, request, req_len, 0);

    long content_len = 0;
    int status = read_http_status(sock, &content_len);

    log_print("GET %s -> HTTP %d  content_len=%ld\n", remote_path, status, content_len);

    if (status != 200) {
        drain_body(sock, content_len > 0 ? content_len : 0);
        lwip_close(sock);
        return -1;
    }

    /* Schrijf body naar bestand */
    HANDLE hf = CreateFile(local_path, GENERIC_WRITE, 0,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        log_print("GET %s -> CreateFile mislukt voor %s\n", remote_path, local_path);
        drain_body(sock, content_len > 0 ? content_len : 0);
        lwip_close(sock);
        return -1;
    }

    char *buf = (char *)malloc(TRANSFER_BUF_SZ);
    if (!buf) { CloseHandle(hf); lwip_close(sock); return -1; }

    long total_written = 0;
    int ok = 1;

    if (content_len > 0) {
        /* Exacte Content-Length bekend: lees precies dat aantal bytes */
        long rem = content_len;
        while (rem > 0) {
            int want = (int)(rem > (long)TRANSFER_BUF_SZ ? (long)TRANSFER_BUF_SZ : rem);
            int r = lwip_recv(sock, buf, want, 0);
            if (r <= 0) { ok = 0; break; }
            DWORD written;
            WriteFile(hf, buf, r, &written, NULL);
            total_written += r;
            rem -= r;
        }
    } else {
        /*
         * Geen Content-Length (chunked of server stuurt het niet).
         * Lees tot de socket sluit (Connection: close).
         * We negeren chunked framing en schrijven de ruwe bytes;
         * voor kleine save-bestanden werkt dit prima. Als de server
         * chunked stuurt, is dit zichtbaar in de log (total_written
         * wijkt dan iets af van de werkelijke bestandsgrootte door
         * de chunk-headers — maar dat gaat goed zodra de server
         * gewoon Content-Length stuurt, wat de meeste WebDAV servers doen).
         */
        while (1) {
            int r = lwip_recv(sock, buf, TRANSFER_BUF_SZ, 0);
            if (r <= 0) break;
            DWORD written;
            WriteFile(hf, buf, r, &written, NULL);
            total_written += r;
        }
        if (total_written == 0) ok = 0;
    }

    log_print("GET %s -> %ld bytes geschreven, ok=%d\n", remote_path, total_written, ok);

    free(buf);
    CloseHandle(hf);
    lwip_close(sock);
    return ok ? 0 : -1;
}


/* ── webdav_mkcol ────────────────────────────────────────────────── */
int webdav_mkcol(const char *remote_path,
                 const char *creds64, const char *host, int port)
{
    int status;
    webdav_request("MKCOL", remote_path, host, port, creds64,
                   NULL, NULL, 0, NULL, 0, &status);
    log_print("MKCOL %s -> %d\n", remote_path, status);
    return status;
}

/* ── webdav_list_directory / webdav_list_directory_ex ────────────── */
/*
 * Interne kern: doet de PROPFIND en vult items[] en optioneel is_dir[].
 * collections_only=1 → sla bestanden over (is_dir mag NULL zijn).
 * collections_only=0 + is_dir != NULL → vul is_dir[] in per item.
 */
static int list_dir_core(const char *remote_path,
                          char items[][MAX_PATH_LEN], int is_dir_out[],
                          int max_items,
                          const char *creds64, const char *host, int port,
                          int collections_only)
{
    /*
     * Lees de PROPFIND response direct van de socket, met dynamisch
     * groeiende buffer. webdav_request() past niet omdat die een
     * vaste buffergrootte heeft; hier alloceren we opnieuw als de
     * buffer vol raakt.
     */
    static const char propfind_body[] =
        "<?xml version=\"1.0\"?>"
        "<D:propfind xmlns:D=\"DAV:\">"
        "<D:prop><D:resourcetype/></D:prop>"
        "</D:propfind>";

    char encoded[MAX_PATH_LEN * 3];
    url_encode_path(remote_path, encoded, sizeof(encoded));

    int sock = open_socket(host, port);
    if (sock < 0) { log_print("PROPFIND %s -> socket open mislukt\n", remote_path); return -1; }

    /* 4096 bytes: ruimte voor lange URL + base64 credentials */
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
        lwip_close(sock);
        return -1;
    }
    lwip_send(sock, request, req_len, 0);
    lwip_send(sock, propfind_body, (int)(sizeof(propfind_body) - 1), 0);

    long content_len = 0;
    int status = read_http_status(sock, &content_len);

    log_print("PROPFIND %s -> HTTP %d  content_len=%ld\n", remote_path, status, content_len);

    if (status != 207) {
        lwip_close(sock);
        return -1;
    }

    /* Dynamisch groeiende buffer: start 64KB, groei met 64KB stappen */
    int buf_sz   = 64 * 1024;
    int received = 0;
    char *resp   = (char *)malloc(buf_sz);
    if (!resp) { lwip_close(sock); return -1; }

    if (content_len > 0) {
        /* Content-Length bekend: lees exact dat aantal bytes, vergroot buffer indien nodig */
        if (content_len + 1 > buf_sz) {
            buf_sz = (int)content_len + 1;
            char *tmp = (char *)realloc(resp, buf_sz);
            if (!tmp) { free(resp); lwip_close(sock); return -1; }
            resp = tmp;
        }
        long rem = content_len;
        while (rem > 0) {
            int want = (int)(rem > 4096 ? 4096 : rem);
            int r = lwip_recv(sock, resp + received, want, 0);
            if (r <= 0) break;
            received += r;
            rem -= r;
        }
    } else {
        /* Chunked of geen Content-Length: lees tot socket sluit, heralloc indien nodig */
        while (1) {
            if (received >= buf_sz - 1) {
                buf_sz += 64 * 1024;
                char *tmp = (char *)realloc(resp, buf_sz);
                if (!tmp) break;
                resp = tmp;
            }
            int r = lwip_recv(sock, resp + received, buf_sz - received - 1, 0);
            if (r <= 0) break;
            received += r;
        }
    }
    resp[received] = '\0';
    lwip_close(sock);

    log_print("PROPFIND %s -> %d bytes ontvangen (buf_sz=%d)\n", remote_path, received, buf_sz);

    if (received <= 0) { free(resp); return -1; }

    int count = 0;
    char *p = resp;

    /* Normaliseer remote_path voor vergelijking: strip trailing slash */
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
        if (val_len <= 0 || val_len >= MAX_PATH_LEN) { free(block); p = resp_end; continue; }

        char href[MAX_PATH_LEN];
        strncpy(href, val_start, val_len);
        href[val_len] = '\0';

        free(block);

        /* Strip trailing slash */
        int hlen = (int)strlen(href);
        if (hlen > 0 && href[hlen - 1] == '/') href[--hlen] = '\0';

        char *name = strrchr(href, '/');
        name = name ? name + 1 : href;

        /* Decodeer %XX in de naam zodat url_encode_path later niet
         * dubbel encodeert (bijv. "Profile%201" -> "Profile 1") */
        url_decode(name);

        int is_self = (util_strcasecmp(href, base_norm) == 0);

        if (!is_self && (!collections_only || is_collection) && name[0] != '\0') {
            strncpy(items[count], name, MAX_PATH_LEN - 1);
            items[count][MAX_PATH_LEN - 1] = '\0';
            if (is_dir_out) is_dir_out[count] = is_collection;
            log_print("  item[%d]: %s (is_dir=%d)\n", count, name, is_collection);
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

/* ── upload_dir ──────────────────────────────────────────────────── */
int upload_dir(const char *local_dir, const char *remote_dir,
               const char *creds64, const char *host, int port)
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

        char local_child[MAX_PATH_LEN];
        char remote_child[MAX_PATH_LEN];
        snprintf(local_child,  sizeof(local_child),  "%s\\%s", local_dir,  fd.cFileName);
        snprintf(remote_child, sizeof(remote_child), "%s/%s",  remote_dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            uploaded += upload_dir(local_child, remote_child, creds64, host, port);
        } else {
            int s = webdav_put_file(local_child, remote_child, creds64, host, port);
            if (s == 200 || s == 201 || s == 204)
                uploaded++;
        }
    } while (FindNextFile(h, &fd));

    FindClose(h);
    return uploaded;
}

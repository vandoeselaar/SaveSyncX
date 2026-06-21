#ifndef GITHUB_FETCH_H
#define GITHUB_FETCH_H

/*
 * github_fetch.h  –  Minimale HTTPS GET via BearSSL voor raw.githubusercontent.com
 *
 * Certificaatverificatie is uitgeschakeld (permissive x509): we praten
 * altijd met een bekende host en een MITM-aanval is geen reëel risico.
 *
 * Hergebruikt de engine_run/tls_write/tls_read patronen uit het
 * nxdk-bearssl-tls-test project.
 */

#include <stddef.h>

/*
 * github_fetch_raw
 *
 * Haalt een bestand op van raw.githubusercontent.com via HTTPS.
 *
 * path:     het URL-pad, bijv.
 *           "/vandoeselaar/SaveSyncX/main/savegames/list.json"
 * out_buf:  buffer voor de response body (null-terminated na afloop)
 * out_size: grootte van out_buf
 *
 * Retourneert het aantal bytes in out_buf (>=0), of -1 bij fout.
 * Bij succes is out_buf altijd null-terminated.
 */
int github_fetch_raw(const char *path, char *out_buf, int out_size);

#endif /* GITHUB_FETCH_H */

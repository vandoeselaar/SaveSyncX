#ifndef WEBDAV_H
#define WEBDAV_H

#include "config.h"
#include <stddef.h>

/*
 * webdav.h  –  WebDAV client over LwIP/TCP
 *
 * All functions take a host (IP or hostname string) and port.
 * creds64 is a Base64-encoded "username:password" string for Basic auth.
 */

/*
 * webdav_base64_encode  –  Encode binary data as Base64 string.
 * out must be at least ceil(len/3)*4 + 1 bytes.
 */
void webdav_base64_encode(const unsigned char *in, int len, char *out);

/*
 * webdav_put_file  –  HTTP PUT a local file to a remote path.
 * Returns HTTP status code, or -1 on socket/IO error.
 */
int webdav_put_file(const char *local_path, const char *remote_path,
                    const char *creds64, const char *host, int port);

/*
 * webdav_get_file  –  HTTP GET a remote file and write it locally.
 * Returns 0 on success, -1 on error.
 */
int webdav_get_file(const char *remote_path, const char *local_path,
                    const char *creds64, const char *host, int port);

/*
 * webdav_mkcol  –  WebDAV MKCOL (create collection/directory).
 * Returns HTTP status code, or -1 on error.
 * Returns 405 if collection already exists (not an error).
 */
int webdav_mkcol(const char *remote_path,
                 const char *creds64, const char *host, int port);

/*
 * webdav_request  –  Send a raw WebDAV request with optional body.
 * method:       e.g. "PROPFIND", "DELETE", "HEAD"
 * extra_headers: additional headers string (e.g. "Depth: 0\r\n"), or NULL
 * body/body_len: request body, or NULL/0
 * resp_buf/resp_sz: optional buffer to receive response body
 * status_out:   filled with HTTP status code
 * Returns number of response body bytes written, or -1 on error.
 */
int webdav_request(const char *method, const char *remote_path,
                   const char *host, int port, const char *creds64,
                   const char *extra_headers,
                   const char *body, int body_len,
                   char *resp_buf, int resp_sz,
                   int *status_out);

/*
 * webdav_list_directory  –  PROPFIND Depth:1 to list a remote directory.
 * Fills items[][MAX_PATH_LEN] with the names (not full paths) of children.
 * collections_only: 1 = only return subdirectories, 0 = return everything
 * Returns count of items found, or -1 on error.
 */
int webdav_list_directory(const char *remote_path,
                           char items[][MAX_PATH_LEN], int max_items,
                           const char *creds64, const char *host, int port,
                           int collections_only);

/*
 * webdav_list_directory_ex  –  zoals webdav_list_directory (collections_only=0),
 * maar vult ook is_dir[] in: 1 = map, 0 = bestand.
 * is_dir moet minstens max_items elementen groot zijn.
 * Returns count of items found, or -1 on error.
 */
int webdav_list_directory_ex(const char *remote_path,
                              char items[][MAX_PATH_LEN], int is_dir[],
                              int max_items,
                              const char *creds64, const char *host, int port);

/*
 * upload_dir  –  Recursively upload a local directory tree via PUT + MKCOL.
 * Returns number of files successfully uploaded.
 */
int upload_dir(const char *local_dir, const char *remote_dir,
               const char *creds64, const char *host, int port);

#endif /* WEBDAV_H */

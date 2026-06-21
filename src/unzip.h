#ifndef UNZIP_H
#define UNZIP_H

/*
 * unzip.h  –  Minimale ZIP-extractor bovenop nxdk's ingebouwde zlib.
 *
 * Ondersteunt:
 *   - Deflate-gecomprimeerde entries (method 8)
 *   - Ongecomprimeerde entries (method 0, stored)
 *   - Geneste mappen (worden automatisch aangemaakt)
 *   - __MACOSX/ entries worden overgeslagen
 *
 * Vereist geen externe libraries — zlib zit al in nxdk (NXDK_NET=y).
 * Geen Makefile-wijzigingen nodig.
 */

/*
 * unzip_to_dir
 *
 * Pakt alle bestanden in de ZIP-buffer (zip_data, zip_len) uit naar
 * dest_dir (bijv. "E:\\UDATA").
 *
 * De mapstructuur in de ZIP wordt 1-op-1 gereproduceerd onder dest_dir,
 * met backslashes als scheidingsteken (Xbox/Win32 API).
 *
 * Retourneert het aantal succesvol uitgepakte bestanden, of -1 bij fout.
 */
int unzip_to_dir(const unsigned char *zip_data, int zip_len,
                 const char *dest_dir);

#endif /* UNZIP_H */

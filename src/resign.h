#ifndef RESIGN_H
#define RESIGN_H

#include <stddef.h>

/*
 * resign.h  –  Non-roamable (EEPROM-locked) save detection and re-signing
 *
 * Some Xbox games sign their save files using the console's XboxHDKey,
 * which is derived from the console's EEPROM.  These saves cannot be
 * loaded on a different console without being re-signed with that console's
 * own HDDKey.
 *
 * Reference: https://consolemods.org/wiki/Xbox:Games_with_Non-Roamable_(EEPROM-Locked)_Saves
 *
 * Our approach:
 *   BACKUP  – Read HDDKey from EEPROM, store as  <remote_base>/hddkey/<TitleID>.key
 *             Tag non-roamable titles in  <remote_base>/nonroamable.txt
 *   RESTORE – Detect non-roamable saves, read SOURCE hddkey + local hddkey,
 *             re-sign each affected file with HMAC-SHA1 using the new key.
 */

/* ── HDDKey size (128-bit / 16 bytes) ───────────────────────────────────── */
#define HDDKEY_SIZE  16

/* ── Save-signing algorithm used per game ───────────────────────────────── */
typedef enum {
    SIGN_NONE    = 0,   /* Normal roamable save – no action needed          */
    SIGN_NOROAM  = 1,   /* HMAC-SHA1 over save data with XboxHDKey          */
    SIGN_FORZA   = 2,   /* Forza Motorsport – custom signing (ForzaSign)    */
    SIGN_PSO     = 3,   /* Phantasy Star Online – signs ALL PSO* files      */
} SignType;

/* ── Per-title entry ─────────────────────────────────────────────────────── */
typedef struct {
    const char   *title_id;
    const char   *title_name;
    SignType      sign_type;
    unsigned char title_sig_key[16];
    int           data_offset;
    int           sig_offset;
    int           sig_size;
    const char   *sig_filename; /* NULL = alle bestanden; anders exact bestandsnaam */
} NonRoamableEntry;

/*
 * The known non-roamable game database.
 * Title IDs sourced from the consolemods.org wiki and XSavSig005 resign.ini.
 */
extern const NonRoamableEntry NONROAMABLE_DB[];
extern const int              NONROAMABLE_DB_COUNT;

/*
 * resign_is_nonroamable  –  Returns the entry for a TitleID, or NULL.
 * title_id must be the 8-char upper-case hex string (e.g. from TDATA path).
 */
const NonRoamableEntry *resign_lookup(const char *title_id);

/*
 * resign_read_hddkey  –  Read the XboxHDKey from the local console EEPROM.
 * Fills key_out[HDDKEY_SIZE].  Returns 0 on success.
 *
 * nxdk exposes the EEPROM via XQueryValue(XC_FACTORY_SETTINGS, ...).
 * The HDDKey is at offset 0x30 in the EEPROM image (16 bytes).
 */
int resign_read_hddkey(unsigned char key_out[HDDKEY_SIZE]);

/*
 * resign_hddkey_to_hex  –  Convert 16-byte key to 32-char hex string + NUL.
 */
void resign_hddkey_to_hex(const unsigned char key[HDDKEY_SIZE],
                           char hex_out[HDDKEY_SIZE * 2 + 1]);

/*
 * resign_hex_to_hddkey  –  Parse 32-char hex string into 16-byte key.
 * Returns 0 on success.
 */
int resign_hex_to_hddkey(const char *hex,
                          unsigned char key_out[HDDKEY_SIZE]);

/*
 * resign_save_file  –  Re-sign a single save file.
 *
 *   local_path   – path on the Xbox filesystem
 *   entry        – game entry from the DB (determines algorithm)
 *   old_key      – HDDKey that was used when the save was created (source Xbox)
 *   new_key      – HDDKey of this console (destination Xbox)
 *
 * Returns 0 on success, -1 on error.
 *
 * Algorithm (SIGN_NOROAM):
 *   1. Read the save file into memory.
 *   2. The first `sign_size` bytes are the existing HMAC-SHA1 signature.
 *   3. Verify the existing signature with old_key  (optional – warn if mismatch).
 *   4. Recompute HMAC-SHA1 over the save body (bytes sign_size .. end) with new_key.
 *   5. Write the new signature back to the file (first sign_size bytes).
 */
int resign_save_file(const char *local_path,
                     const NonRoamableEntry *entry,
                     const unsigned char old_key[HDDKEY_SIZE],
                     const unsigned char new_key[HDDKEY_SIZE]);

/*
 * resign_process_title  –  Re-sign all save files under a TDATA/<TitleID>/
 * directory after a restore.
 *
 * Walks every file under title_dir, looks up the entry, calls
 * resign_save_file() for each file that needs signing.
 *
 * Returns number of files re-signed, or -1 on fatal error.
 */
int resign_process_title(const char *title_dir,
                          const NonRoamableEntry *entry,
                          const unsigned char old_key[HDDKEY_SIZE],
                          const unsigned char new_key[HDDKEY_SIZE]);

#endif /* RESIGN_H */

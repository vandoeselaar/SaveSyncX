#ifndef BACKUP_H
#define BACKUP_H

#include "titlescan.h"
#include "config.h"

/*
 * do_backup  –  Backup menu: toon lokale saves, upload naar server.
 *
 *   titles / n   – lijst van gevonden TitleEntry's op E:\UDATA
 *   creds64      – Base64 "user:pass" voor WebDAV
 *   cfg          – app configuratie
 *
 * Knoppen:
 *   up/down  – scroll
 *   A        – upload geselecteerde save naar server
 *   Y        – verwijder geselecteerde save van Xbox (met bevestiging)
 *   B        – terug naar hoofdmenu
 */
void do_backup(TitleEntry *titles, int *n,
               const char *creds64, const AppConfig *cfg);

#endif /* BACKUP_H */

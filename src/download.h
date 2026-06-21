#ifndef DOWNLOAD_H
#define DOWNLOAD_H

/*
 * download.h  –  "Download Saves" scherm voor SaveSyncX
 *
 * Haalt list.json op van GitHub, toont de beschikbare saves en laat
 * de gebruiker er één downloaden naar E:\UDATA\{titleid}\.
 */

#include "config.h"

/*
 * do_download
 *
 * Hoofdfunctie voor het downloadscherm.
 * cfg wordt gebruikt voor de (eventuele) WebDAV-host; voor GitHub
 * is geen authenticatie nodig.
 *
 * Retourneert 0 na normale terugkeer naar het hoofdmenu.
 */
int do_download(const AppConfig *cfg);

#endif /* DOWNLOAD_H */

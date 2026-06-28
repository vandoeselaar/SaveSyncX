/*
 * download.c  –  "Download Saves" scherm
 *
 * Flow:
 *  1. Toon "Fetching list..." splash
 *  2. Haal list.json op via github_fetch_raw
 *  3. Parse naar SaveList
 *  4. Toon tweelaags menu: games → saves
 *  5. Bij A: bevestig → download zip → uitpakken (TODO: unzip)
 *  6. Bij B: terug naar hoofdmenu
 *
 * UI-conventies volgen de bestaande schermen (do_backup, do_restore).
 */

#include "download.h"
#include "github_fetch.h"
#include "savelist.h"
#include "unzip.h"
#include "resign.h"
#include "../lib/ui.h"
#include "../lib/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* URL-prefix voor downloads — bestandspad in SaveEntry is relatief hieraan */
#define GITHUB_RAW_PATH_PREFIX  "/vandoeselaar/SaveSyncX-Saves/main/"

/* Maximale grootte van de list.json response */
#define LIST_JSON_MAX   (640 * 1024)

/* Maximale grootte van een gedownloade save zip */
#define SAVE_ZIP_MAX    (4 * 1024 * 1024)   /* 4 MB, ruim voor save-zips */

/* Tijdelijk downloadpad */
/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void fmt_size(int bytes, char *out, int out_len)
{
    if (bytes < 0)
        snprintf(out, out_len, "?");
    else if (bytes < 1024)
        snprintf(out, out_len, "%d B", bytes);
    else if (bytes < 1024 * 1024)
        snprintf(out, out_len, "%d KB", bytes / 1024);
    else
        snprintf(out, out_len, "%.1f MB", (float)bytes / (1024.0f * 1024.0f));
}

/* ── Download + opslaan van een zip-bestand ─────────────────────────────── */
static int download_save(const SaveEntry *se)
{
    /* Bouw het volledige pad: /vandoeselaar/SaveSyncX-Saves/main/... */
    char path[256];
    snprintf(path, sizeof(path), "/" "%s%s",
             "vandoeselaar/SaveSyncX-Saves/main/",
             se->file);

    log_print("[DL] downloading %s\n", path);

    char *buf = (char *)malloc(SAVE_ZIP_MAX);
    if (!buf) {
        ui_message("Error", "Out of memory");
        return -1;
    }

    ui_message_nowait("Downloading", se->label);

    int len = github_fetch_raw(path, buf, SAVE_ZIP_MAX);
    if (len <= 0) {
        free(buf);
        ui_message("Error", "Download failed");
        return -1;
    }

    log_print("[DL] %d bytes ontvangen, uitpakken naar E:\\UDATA\\\n", len);
    ui_message_nowait("Extracting...", se->label);

    int files = unzip_to_dir((const unsigned char *)buf, len, "E:\\UDATA");
    free(buf);

    if (files <= 0) {
        ui_message("Error", "Unzip failed");
        return -1;
    }

    log_print("[DL] %d bestanden uitgepakt\n", files);

    /*
     * Resign check: de ZIP-rootmap is de TitleID (bijv. "4541005b").
     * We halen de TitleID uit se->file: "{titleid}/...".
     * resign_lookup verwacht uppercase.
     */
    char title_id_lower[16] = "";
    char title_id_upper[16] = "";

    /* Extraheer TitleID uit "/4541005b/..." */
    const char *sg = se->file;
    int i = 0;
    while (sg[i] && sg[i] != '/' && i < 15) {
        title_id_lower[i] = sg[i];
        title_id_upper[i] = (sg[i] >= 'a' && sg[i] <= 'f')
                             ? sg[i] - 32 : sg[i];
        i++;
    }
    title_id_lower[i] = title_id_upper[i] = '\0';

    const NonRoamableEntry *entry = title_id_upper[0]
                                    ? resign_lookup(title_id_upper)
                                    : NULL;

    if (!entry || entry->sign_type == SIGN_NONE) {
        /* Geen resign nodig */
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "%d file%s extracted\nNo resign needed.",
                 files, files == 1 ? "" : "s");
        ui_message("Done!", msg);
        return 0;
    }

    /* Resign nodig: lees eigen HDDKey */
    log_print("[DL] resign nodig voor %s (%s)\n",
              title_id_upper, entry->title_name);
    ui_message_nowait("Resigning...", entry->title_name);

    unsigned char new_key[HDDKEY_SIZE];
    if (resign_read_hddkey(new_key) != 0) {
        ui_message("Error", "Could not read HDDKey from EEPROM");
        return -1;
    }

    /*
     * old_key: de save komt van GitHub, niet van een specifieke Xbox.
     * We gebruiken een nul-key — resign_save_file geeft een waarschuwing
     * bij verify-mismatch maar schrijft de nieuwe handtekening gewoon.
     */
    unsigned char old_key[HDDKEY_SIZE];
    memset(old_key, 0, sizeof(old_key));

    char title_dir[MAX_PATH_LEN];
    snprintf(title_dir, sizeof(title_dir),
             "E:\\UDATA\\%s", title_id_lower);

    int resigned = resign_process_title(title_dir, entry, old_key, new_key);
    if (resigned < 0) {
        ui_message("Error", "Resign failed");
        return -1;
    }

    log_print("[DL] %d bestanden geresigned\n", resigned);

    char msg[128];
    snprintf(msg, sizeof(msg),
             "%d file%s extracted\n%d file%s resigned.",
             files, files == 1 ? "" : "s",
             resigned, resigned == 1 ? "" : "s");
    ui_message("Done!", msg);
    return 0;
}

/* ── Save-selectiescherm (tweede niveau) ─────────────────────────────────── */
static void show_save_menu(const GameSaveList *game)
{
    if (game->save_count == 0) {
        ui_message("No saves", "This game has no saves available.");
        return;
    }

    int sel = 0;
    ScrollState ss = {0};

    while (1) {
        ui_pump_events();

        /* Render */
        ui_clear();
        ui_draw_text(MARGIN_X, MARGIN_Y,
                     game->game_name, UI_COL_TITLE);

        int y = MARGIN_Y + CHAR_H * 2;
        for (int i = 0; i < game->save_count; i++) {
            const SaveEntry *se = &game->saves[i];
            Uint32 col = (i == sel) ? UI_COL_SELECT : UI_COL_ITEM;

            if (i == sel) {
                ui_fill_rect(MARGIN_X - 4, y - 2,
                             SCREEN_W - MARGIN_X * 2 + 8, CHAR_H + 4,
                             UI_COL_SELBG);
            }

            char size_str[16];
            fmt_size(se->size_bytes, size_str, sizeof(size_str));

            char line[128];
            snprintf(line, sizeof(line), "%s  [%s]", se->label, size_str);
            ui_draw_text(MARGIN_X, y, line, col);
            y += CHAR_H + 4;

            /* Notes onder de geselecteerde entry */
            if (i == sel && se->notes[0]) {
                char notes[64];
                snprintf(notes, sizeof(notes), "  %s", se->notes);
                ui_draw_text(MARGIN_X, y, notes, UI_COL_DIM);
                y += CHAR_H;
            }
        }

        ui_draw_text(MARGIN_X, SCREEN_H - MARGIN_Y - CHAR_H,
                     "A=Download  B=Back", UI_COL_DIM);
        ui_flip();

        /* Input */
        int delta = scroll_update(&ss, game->save_count);
        scroll_apply(&sel, delta, game->save_count);

        if (btn_pressed(BTN_A)) {
            const SaveEntry *se = &game->saves[sel];
            char confirm[128];
            snprintf(confirm, sizeof(confirm),
                     "Download '%s'?", se->label);
            if (ui_confirm(confirm)) {
                download_save(se);
            }
        }

        if (btn_pressed(BTN_B)) return;
    }
}

/* ── Game-selectiescherm (eerste niveau) ─────────────────────────────────── */
static void show_game_menu(const SaveList *list)
{
    if (list->game_count == 0) {
        ui_message("No saves", "list.json bevat geen games.");
        return;
    }

    int sel    = 0;
    int offset = 0;
    ScrollState ss = {0};

    while (1) {
        ui_pump_events();

        ui_clear();
        ui_draw_text(MARGIN_X, MARGIN_Y, "Download Saves", UI_COL_TITLE);

        char sub[64];
        snprintf(sub, sizeof(sub), "Updated: %s  (%d games)",
                 list->updated, list->game_count);
        ui_draw_text(MARGIN_X, MARGIN_Y + CHAR_H, sub, UI_COL_DIM);

        int y = MARGIN_Y + CHAR_H * 3;

        /* Pas offset aan zodat sel altijd zichtbaar is */
        if (sel < offset) offset = sel;
        if (sel >= offset + LIST_PAGE_SIZE) offset = sel - LIST_PAGE_SIZE + 1;

        for (int i = offset;
             i < list->game_count && i < offset + LIST_PAGE_SIZE;
             i++) {
            const GameSaveList *g = &list->games[i];
            Uint32 col = (i == sel) ? UI_COL_SELECT : UI_COL_ITEM;

            if (i == sel) {
                ui_fill_rect(MARGIN_X - 4, y - 2,
                             SCREEN_W - MARGIN_X * 2 + 8, CHAR_H + 4,
                             UI_COL_SELBG);
            }

            char line[128];
            snprintf(line, sizeof(line), "%s  (%d save%s)",
                     g->game_name,
                     g->save_count,
                     g->save_count == 1 ? "" : "s");
            ui_draw_text(MARGIN_X, y, line, col);

            /* TitleID rechts */
            char tid[24];
            snprintf(tid, sizeof(tid), "[%s]", g->title_id);
            ui_draw_text(SCREEN_W - MARGIN_X - (int)strlen(tid) * 8,
                         y, tid, UI_COL_DIM);

            y += CHAR_H + 2;
        }

        /* Scroll-indicator */
        if (list->game_count > LIST_PAGE_SIZE) {
            char sc[16];
            snprintf(sc, sizeof(sc), "%d/%d", sel + 1, list->game_count);
            ui_draw_text(SCREEN_W - MARGIN_X - (int)strlen(sc) * 8,
                         SCREEN_H - MARGIN_Y - CHAR_H,
                         sc, UI_COL_DIM);
        }

        ui_draw_text(MARGIN_X, SCREEN_H - MARGIN_Y - CHAR_H,
                     "A=Select  B=Back", UI_COL_DIM);
        ui_flip();

        int delta = scroll_update(&ss, list->game_count);
        scroll_apply(&sel, delta, list->game_count);

        if (btn_pressed(BTN_A)) {
            show_save_menu(&list->games[sel]);
        }

        if (btn_pressed(BTN_B)) return;
    }
}

/* ── Hoofdfunctie ─────────────────────────────────────────────────────────── */
int do_download(const AppConfig *cfg)
{
    (void)cfg;

    /* Stap 1: splash */
    ui_message_nowait("Download Saves", "Fetching list from GitHub...");

    /* Stap 2: list.json ophalen */
    char *json_buf = (char *)malloc(LIST_JSON_MAX);
    if (!json_buf) {
        ui_message("Error", "Out of memory");
        return -1;
    }

    int len = github_fetch_raw(
        "/vandoeselaar/SaveSyncX-Saves/main/list.json",
        json_buf, LIST_JSON_MAX);

    if (len <= 0) {
        free(json_buf);
        ui_message("Error", "Could not fetch list.json from GitHub.\n"
                             "Check network connection.");
        return -1;
    }

    log_print("[DL] list.json: %d bytes\n", len);

    /* Stap 3: parse */
    SaveList *list = (SaveList *)malloc(sizeof(SaveList));
    if (!list) {
        free(json_buf);
        ui_message("Error", "Out of memory");
        return -1;
    }

    int game_count = savelist_parse(json_buf, list);
    free(json_buf);

    if (game_count < 0) {
        free(list);
        ui_message("Error", "Failed to parse list.json");
        return -1;
    }

    if (game_count == 0) {
        free(list);
        ui_message("Download Saves", "No saves available yet.");
        return 0;
    }

    /* Stap 4: toon menu */
    show_game_menu(list);

    free(list);
    return 0;
}

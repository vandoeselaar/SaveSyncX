/*
 * savelist.c  –  Simpele hand-parser voor savegames/list.json
 *
 * Geheugen wordt dynamisch gealloceerd per game en per save, zodat de
 * geheugenvoetafdruk overeenkomt met de werkelijke inhoud van list.json.
 * GTA San Andreas met 104 saves kost dan hetzelfde als een game met 2 saves
 * maal 52 — in plaats van dat de MAX_SAVES limiet voor alle games geldt.
 */

#include "savelist.h"
#include "../lib/util.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Hulpfuncties ────────────────────────────────────────────────────────── */

static const char *read_str_field(const char *src, const char *key,
                                   char *dst, int dst_size)
{
    const char *p = strstr(src, key);
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ':' || *p == ' ' || *p == '\t') p++;

    if (*p == 'n') {
        dst[0] = '\0';
        while (*p && *p != ',' && *p != '\n' && *p != '}') p++;
        return p;
    }
    if (*p != '"') return NULL;
    p++;

    int i = 0;
    while (*p && *p != '"' && i < dst_size - 1) {
        if (*p == '\\' && *(p+1)) p++;
        dst[i++] = *p++;
    }
    dst[i] = '\0';
    if (*p == '"') p++;
    return p;
}

static const char *read_int_field(const char *src, const char *key, int *out)
{
    const char *p = strstr(src, key);
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    *out = (int)strtol(p, NULL, 10);
    while (*p && *p != ',' && *p != '\n' && *p != '}') p++;
    return p;
}

/* ── Geheugenhelpers ─────────────────────────────────────────────────────── */

/* Voeg een lege SaveEntry toe aan een GameSaveList, groeit de array indien nodig.
   Retourneert pointer naar de nieuwe entry, of NULL bij malloc-fout. */
static SaveEntry *game_add_save(GameSaveList *g)
{
    if (g->save_count >= g->save_cap) {
        int new_cap = g->save_cap == 0 ? 8 : g->save_cap * 2;
        SaveEntry *tmp = (SaveEntry *)realloc(g->saves,
                                              new_cap * sizeof(SaveEntry));
        if (!tmp) return NULL;
        g->saves    = tmp;
        g->save_cap = new_cap;
    }
    SaveEntry *se = &g->saves[g->save_count++];
    memset(se, 0, sizeof(*se));
    return se;
}

/* ── Hoofd-parser ────────────────────────────────────────────────────────── */

int savelist_parse(const char *json, SaveList *out)
{
    if (!json || !out) return -1;
    memset(out, 0, sizeof(*out));

    /* Alloceer games pointer-array */
    out->games = (GameSaveList **)calloc(SAVELIST_MAX_GAMES,
                                          sizeof(GameSaveList *));
    if (!out->games) return -1;

    read_int_field(json, "\"version\"", &out->version);
    read_str_field(json, "\"updated\"", out->updated, sizeof(out->updated));

    /* Sla buitenste "saves": [ over */
    const char *saves_start = strstr(json, "\"saves\"");
    if (!saves_start) {
        log_print("[SL] 'saves' array niet gevonden\n");
        free(out->games);
        out->games = NULL;
        return -1;
    }
    const char *p = strchr(saves_start, '[');
    if (!p) { free(out->games); out->games = NULL; return -1; }
    p++;

    while (out->game_count < SAVELIST_MAX_GAMES) {

        /* Zoek volgende title_id */
        const char *tid_pos = strstr(p, "\"title_id\"");
        if (!tid_pos) break;

        /* Alloceer GameSaveList op de heap */
        GameSaveList *g = (GameSaveList *)calloc(1, sizeof(GameSaveList));
        if (!g) break;

        /* title_id */
        const char *after = read_str_field(tid_pos, "\"title_id\"",
                                            g->title_id, sizeof(g->title_id));
        if (!after) { free(g); p = tid_pos + 1; continue; }

        /* Bepaal het einde van dit game-object (volgende title_id of einde) */
        const char *next_tid = strstr(after, "\"title_id\"");
        int search_len = next_tid ? (int)(next_tid - tid_pos)
                                  : (int)strlen(tid_pos);

        char *obj = (char *)malloc(search_len + 1);
        if (!obj) { free(g); break; }
        memcpy(obj, tid_pos, search_len);
        obj[search_len] = '\0';

        read_str_field(obj, "\"game_name\"",
                       g->game_name, sizeof(g->game_name));

        /* Interne saves array — bracket-teller voor correcte afbakening */
        const char *inner_saves = strstr(obj, "\"saves\"");
        if (inner_saves) {
            const char *arr = strchr(inner_saves, '[');
            if (arr) {
                arr++;
                int depth = 1;
                const char *q = arr;
                while (*q && depth > 0) {
                    if      (*q == '[') depth++;
                    else if (*q == ']') depth--;
                    q++;
                }
                int arr_len = (int)((q - 1) - arr);

                char *save_block = (char *)malloc(arr_len + 1);
                if (save_block) {
                    memcpy(save_block, arr, arr_len);
                    save_block[arr_len] = '\0';

                    const char *sp = save_block;
                    while (1) {
                        const char *id_pos = strstr(sp, "\"id\"");
                        if (!id_pos) break;

                        const char *next_id = strstr(id_pos + 4, "\"id\"");
                        int slen = next_id ? (int)(next_id - id_pos)
                                           : (int)strlen(id_pos);

                        char *sobj = (char *)malloc(slen + 1);
                        if (!sobj) break;
                        memcpy(sobj, id_pos, slen);
                        sobj[slen] = '\0';

                        SaveEntry *se = game_add_save(g);
                        if (!se) { free(sobj); break; }

                        read_str_field(sobj, "\"id\"",
                                       se->id,     sizeof(se->id));
                        read_str_field(sobj, "\"label\"",
                                       se->label,  sizeof(se->label));
                        read_str_field(sobj, "\"file\"",
                                       se->file,   sizeof(se->file));
                        read_str_field(sobj, "\"author\"",
                                       se->author, sizeof(se->author));
                        read_str_field(sobj, "\"notes\"",
                                       se->notes,  sizeof(se->notes));
                        read_int_field(sobj, "\"size_bytes\"",
                                       &se->size_bytes);

                        free(sobj);
                        if (next_id) sp = next_id;
                        else break;
                    }
                    free(save_block);
                }
            }
        }

        free(obj);
        out->games[out->game_count++] = g;
        p = next_tid ? next_tid : (tid_pos + 1);
    }

    log_print("[SL] parsed: %d games\n", out->game_count);
    return out->game_count;
}

/* ── Opruimen ────────────────────────────────────────────────────────────── */

void savelist_free(SaveList *list)
{
    if (!list) return;
    if (list->games) {
        for (int i = 0; i < list->game_count; i++) {
            if (list->games[i]) {
                free(list->games[i]->saves);
                free(list->games[i]);
            }
        }
        free(list->games);
        list->games = NULL;
    }
    list->game_count = 0;
}

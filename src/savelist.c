/*
 * savelist.c  –  Simpele hand-parser voor savegames/list.json
 *
 * Strategie: we zoeken sleutelwoorden met strstr en lezen de waarden
 * die er direct achter staan. Geen recursieve parser, geen heap-allocatie.
 * Werkt zolang het JSON-formaat stabiel is (en dat is het, want wij
 * schrijven het zelf).
 *
 * Verwacht formaat (zie eerder in gesprek):
 * {
 *   "version": 1,
 *   "updated": "2025-01-15",
 *   "saves": [
 *     {
 *       "title_id": "4d530004",
 *       "game_name": "Halo: Combat Evolved",
 *       "saves": [
 *         {
 *           "id": "default",
 *           "label": "Campaign - All missions unlocked",
 *           "file": "savegames/4d530004/default.zip",
 *           "size_bytes": 24576,
 *           "author": "vandoeselaar",
 *           "notes": "100% complete"
 *         }
 *       ]
 *     }
 *   ]
 * }
 */

#include "savelist.h"
#include "../lib/util.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Hulpfuncties ────────────────────────────────────────────────────────── */

/*
 * Lees de string-waarde na `"key":` in src (inclusief aanhalingstekens).
 * Schrijft naar dst (max dst_size bytes, null-terminated).
 * Retourneert een pointer naar het teken NA de sluitende quote, of NULL.
 */
static const char *read_str_field(const char *src,
                                   const char *key,
                                   char *dst, int dst_size)
{
    /* Zoek "key": */
    const char *p = strstr(src, key);
    if (!p) return NULL;
    p += strlen(key);

    /* Sla whitespace en ':' over */
    while (*p == ':' || *p == ' ' || *p == '\t') p++;

    if (*p == 'n') {
        /* null waarde */
        dst[0] = '\0';
        /* sla "null" over */
        while (*p && *p != ',' && *p != '\n' && *p != '}') p++;
        return p;
    }

    if (*p != '"') return NULL;
    p++;    /* sla openingsquote over */

    int i = 0;
    while (*p && *p != '"' && i < dst_size - 1) {
        /* Eenvoudige escape handling */
        if (*p == '\\' && *(p+1)) { p++; }
        dst[i++] = *p++;
    }
    dst[i] = '\0';

    if (*p == '"') p++;  /* sla sluitingsquote over */
    return p;
}

/*
 * Lees een integer-waarde na `"key":`.
 * Retourneert een pointer voorbij de waarde, of NULL.
 */
static const char *read_int_field(const char *src,
                                   const char *key,
                                   int *out)
{
    const char *p = strstr(src, key);
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    *out = (int)strtol(p, NULL, 10);
    while (*p && *p != ',' && *p != '\n' && *p != '}') p++;
    return p;
}

/*
 * Zoek het n-de voorkomen van `needle` in `haystack`.
 * (0-gebaseerd: n=0 is het eerste voorkomen)
 */
static const char *find_nth(const char *haystack, const char *needle, int n)
{
    const char *p = haystack;
    for (int i = 0; i <= n; i++) {
        p = strstr(p, needle);
        if (!p) return NULL;
        if (i < n) p++;
    }
    return p;
}

/* ── Hoofd-parser ────────────────────────────────────────────────────────── */
int savelist_parse(const char *json, SaveList *out)
{
    if (!json || !out) return -1;
    memset(out, 0, sizeof(*out));

    /* version */
    read_int_field(json, "\"version\"", &out->version);

    /* updated */
    read_str_field(json, "\"updated\"", out->updated, sizeof(out->updated));

    /*
     * Loop door game-objecten in de buitenste "saves": [...] array.
     * We herkennen elk game-object aan het voorkomen van "title_id".
     */
    const char *p = json;

    /* Sla de buitenste array header over */
    const char *saves_start = strstr(p, "\"saves\"");
    if (!saves_start) {
        log_print("[SL] 'saves' array niet gevonden\n");
        return -1;
    }
    /* Sla "saves": [ over */
    p = strchr(saves_start, '[');
    if (!p) return -1;
    p++;

    out->game_count = 0;

    while (out->game_count < SAVELIST_MAX_GAMES) {
        /* Zoek volgende title_id */
        const char *tid_pos = strstr(p, "\"title_id\"");
        if (!tid_pos) break;

        GameSaveList *g = &out->games[out->game_count];

        /* title_id */
        const char *after = read_str_field(tid_pos, "\"title_id\"",
                                            g->title_id, sizeof(g->title_id));
        if (!after) { p = tid_pos + 1; continue; }

        /* game_name — zoek in dezelfde buurt (tot de volgende title_id) */
        const char *next_tid = strstr(after, "\"title_id\"");
        int  search_len = next_tid
                          ? (int)(next_tid - tid_pos)
                          : (int)strlen(tid_pos);

        /* Tijdelijke kopie voor field-zoekopdrachten binnen dit object */
        char *obj = (char *)malloc(search_len + 1);
        if (!obj) break;
        memcpy(obj, tid_pos, search_len);
        obj[search_len] = '\0';

        read_str_field(obj, "\"game_name\"",
                       g->game_name, sizeof(g->game_name));

        /* Interne "saves": [ array voor dit game-object */
        const char *inner_saves = strstr(obj, "\"saves\"");
        g->save_count = 0;

        if (inner_saves) {
            const char *arr = strchr(inner_saves, '[');
            if (arr) {
                arr++;  /* voorbij '[' */
                const char *arr_end = strchr(arr, ']');
                int arr_len = arr_end ? (int)(arr_end - arr) : (int)strlen(arr);

                char *save_block = (char *)malloc(arr_len + 1);
                if (save_block) {
                    memcpy(save_block, arr, arr_len);
                    save_block[arr_len] = '\0';

                    const char *sp = save_block;
                    while (g->save_count < SAVELIST_MAX_SAVES) {
                        /* Zoek volgende save-object aan de hand van "id": */
                        const char *id_pos = strstr(sp, "\"id\"");
                        if (!id_pos) break;

                        SaveEntry *se = &g->saves[g->save_count];

                        /* Bepaal einde van dit save-object (volgende "id" of einde) */
                        const char *next_id = strstr(id_pos + 4, "\"id\"");
                        int slen = next_id
                                   ? (int)(next_id - id_pos)
                                   : (int)strlen(id_pos);

                        char *sobj = (char *)malloc(slen + 1);
                        if (!sobj) break;
                        memcpy(sobj, id_pos, slen);
                        sobj[slen] = '\0';

                        read_str_field(sobj, "\"id\"",
                                       se->id, sizeof(se->id));
                        read_str_field(sobj, "\"label\"",
                                       se->label, sizeof(se->label));
                        read_str_field(sobj, "\"file\"",
                                       se->file, sizeof(se->file));
                        read_str_field(sobj, "\"author\"",
                                       se->author, sizeof(se->author));
                        read_str_field(sobj, "\"notes\"",
                                       se->notes, sizeof(se->notes));
                        read_int_field(sobj, "\"size_bytes\"",
                                       &se->size_bytes);

                        free(sobj);
                        g->save_count++;

                        if (next_id) sp = next_id;
                        else break;
                    }
                    free(save_block);
                }
            }
        }

        free(obj);
        out->game_count++;

        /* Ga verder voorbij dit game-object */
        p = next_tid ? next_tid : (tid_pos + 1);
    }

    log_print("[SL] parsed: %d games\n", out->game_count);
    return out->game_count;
}

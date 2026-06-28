#ifndef SAVELIST_H
#define SAVELIST_H

/*
 * savelist.h  –  Datastructuren en parser voor savegames/list.json
 *
 * JSON wordt hand-geparst met strstr/sscanf. Geen externe library.
 * Het formaat is stabiel en voorspelbaar, dus dat is prima.
 */

#define SAVELIST_MAX_GAMES    256    /* ruim voor huidige ~230 games */
#define SAVELIST_MAX_SAVES    32     /* saves per game; Baldur's Gate heeft 10 */
#define SAVELIST_MAX_STR      192    /* labels kunnen lang zijn */

/* Één downloadbare save */
typedef struct {
    char id[SAVELIST_MAX_STR];          /* bijv. "default", "good_ending" */
    char label[SAVELIST_MAX_STR];       /* weergavenaam in de UI */
    char file[SAVELIST_MAX_STR];        /* relatief pad, bijv. "savegames/4d530004/default.zip" */
    char author[SAVELIST_MAX_STR];
    char notes[SAVELIST_MAX_STR];
    int  size_bytes;
} SaveEntry;

/* Alle saves voor één game / TitleID */
typedef struct {
    char      title_id[16];                     /* bijv. "4d530004" */
    char      game_name[SAVELIST_MAX_STR];
    SaveEntry saves[SAVELIST_MAX_SAVES];
    int       save_count;
} GameSaveList;

/* Volledige lijst uit list.json */
typedef struct {
    int           version;
    char          updated[32];
    GameSaveList  games[SAVELIST_MAX_GAMES];
    int           game_count;
} SaveList;

/*
 * savelist_parse
 *
 * Parst een null-terminated JSON string (uit github_fetch_raw)
 * naar een SaveList struct.
 *
 * Retourneert het aantal gevonden games (>=0), of -1 bij parseerfout.
 */
int savelist_parse(const char *json, SaveList *out);

#endif /* SAVELIST_H */

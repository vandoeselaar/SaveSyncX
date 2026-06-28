#ifndef SAVELIST_H
#define SAVELIST_H

/*
 * savelist.h  –  Datastructuren en parser voor savegames/list.json
 *
 * JSON wordt hand-geparst met strstr/sscanf. Geen externe library.
 * Het formaat is stabiel en voorspelbaar, dus dat is prima.
 *
 * Geheugen wordt dynamisch gealloceerd zodat de footprint overeenkomt
 * met de werkelijke inhoud van list.json. Gebruik savelist_free() om
 * alles weer vrij te geven.
 */

#define SAVELIST_MAX_GAMES  1024   /* harde bovengrens voor de games-pointer-array */
#define SAVELIST_MAX_STR     192   /* veldlengte voor strings */

/* Één downloadbare save */
typedef struct {
    char id[SAVELIST_MAX_STR];
    char label[SAVELIST_MAX_STR];
    char file[SAVELIST_MAX_STR];
    char author[SAVELIST_MAX_STR];
    char notes[SAVELIST_MAX_STR];
    int  size_bytes;
} SaveEntry;

/* Alle saves voor één game / TitleID */
typedef struct {
    char       title_id[16];
    char       game_name[SAVELIST_MAX_STR];
    SaveEntry *saves;       /* heap-gealloceerde array, savelist_free() geeft vrij */
    int        save_count;
    int        save_cap;    /* gealloceerde capaciteit */
} GameSaveList;

/* Volledige lijst uit list.json */
typedef struct {
    int            version;
    char           updated[32];
    GameSaveList **games;       /* array van pointers, heap-gealloceerd */
    int            game_count;
} SaveList;

/*
 * savelist_parse
 * Parst een null-terminated JSON string naar een SaveList.
 * Retourneert het aantal gevonden games (>=0), of -1 bij parseerfout.
 * Roep savelist_free() aan als de lijst niet meer nodig is.
 */
int savelist_parse(const char *json, SaveList *out);

/*
 * savelist_free
 * Geeft alle door savelist_parse gealloceerde heap-geheugen vrij.
 */
void savelist_free(SaveList *list);

#endif /* SAVELIST_H */

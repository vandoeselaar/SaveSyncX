#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xboxkrnl/xboxkrnl.h>

#include "config.h"

/* ------------------------------------------------------------------ */
/* Intern: basismap van de draaiende XBE bepalen                       */
/*                                                                     */
/* Volgorde:                                                           */
/*   1. XeImageFileName  — map van de draaiende XBE (bijv.            */
/*      \Device\Harddisk0\Partition1\Apps\SaveSyncX\default.xbe)      */
/*      omgezet naar DOS-pad (E:\Apps\SaveSyncX)                      */
/*   2. E:\Apps\SaveSyncX  (fallback)                                 */
/*   3. E:\Applications\SaveSyncX  (fallback)                         */
/* ------------------------------------------------------------------ */

/*
 * Zet een kernel-pad (\Device\Harddisk0\PartitionN\...) om naar een
 * DOS-pad (E:\...) op basis van de standaard Xbox partitie-mapping.
 * Geeft 1 terug bij succes, 0 als het pad niet herkend wordt.
 */
static int kernel_to_dos_path(const char *kernel, char *out, int out_sz)
{
    static const struct { const char *device; char letter; } map[] = {
        { "\\Device\\Harddisk0\\Partition1", 'E' },
        { "\\Device\\Harddisk0\\Partition2", 'C' },
        { "\\Device\\Harddisk0\\Partition3", 'X' },
        { "\\Device\\Harddisk0\\Partition4", 'Y' },
        { "\\Device\\Harddisk0\\Partition5", 'Z' },
        { "\\Device\\Harddisk0\\Partition6", 'F' },
        { "\\Device\\Harddisk0\\Partition7", 'G' },
        { "\\Device\\CdRom0",               'D' },
    };
    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++) {
        size_t dev_len = strlen(map[i].device);
        if (strncmp(kernel, map[i].device, dev_len) == 0) {
            const char *rest = kernel + dev_len; /* "\foo\bar" or "" */
            snprintf(out, out_sz, "%c:%s", map[i].letter, rest);
            /* forward slash → backslash */
            for (char *p = out; *p; p++)
                if (*p == '/') *p = '\\';
            return 1;
        }
    }
    return 0;
}

/*
 * Bepaal de map van de draaiende XBE en schrijf die naar *out.
 * Geeft 1 terug als het gelukt is via XeImageFileName, anders 0.
 */
static int get_xbe_dir(char *out, int out_sz)
{
    /* XeImageFileName is ANSI_STRING[1] in nxdk (xboxkrnl.h:1613) */
    extern ANSI_STRING XeImageFileName[1];
    if (!XeImageFileName[0].Buffer || XeImageFileName[0].Length == 0)
        return 0;

    /* Kopieer kernel-pad naar tijdelijke buffer */
    char kpath[MAX_PATH_LEN];
    int klen = XeImageFileName[0].Length;
    if (klen >= (int)sizeof(kpath)) klen = (int)sizeof(kpath) - 1;
    memcpy(kpath, XeImageFileName[0].Buffer, klen);
    kpath[klen] = '\0';

    char dospath[MAX_PATH_LEN];
    if (!kernel_to_dos_path(kpath, dospath, sizeof(dospath)))
        return 0;

    /* Strip bestandsnaam, houd alleen de map over */
    char *last_sep = NULL;
    for (char *p = dospath; *p; p++)
        if (*p == '\\') last_sep = p;
    if (!last_sep) return 0;
    *last_sep = '\0';

    strncpy(out, dospath, out_sz - 1);
    out[out_sz - 1] = '\0';
    return 1;
}

/* Publieke wrapper — gebruikt door main.c voor log én ini */
int config_get_app_dir(char *out, int out_sz)
{
    return get_xbe_dir(out, out_sz);
}

/*
 * Bepaal het VOLLEDIGE pad (map + bestandsnaam) van de draaiende XBE
 * als DOS-pad, bijv. "E:\Apps\SaveSyncX\default.xbe".
 *
 * Dit is wat XLaunchXBE verwacht: de nxdk-implementatie roept zelf
 * XConvertDOSFilenameToXBOX aan op het pad dat je meegeeft, en zet
 * de ';'-padscheider ook zelf (XLaunchXBEEx in lib/hal/xbox.c) — dus
 * een kernel-pad of voorafgaande ';'-vervanging door de caller is
 * niet nodig en zou averechts werken.
 *
 * Geeft 1 terug bij succes, 0 als XeImageFileName niet beschikbaar is
 * of het kernel-pad niet naar een DOS-pad omgezet kon worden.
 */
static int get_xbe_filepath(char *out, int out_sz)
{
    extern ANSI_STRING XeImageFileName[1];
    if (!XeImageFileName[0].Buffer || XeImageFileName[0].Length == 0)
        return 0;

    char kpath[MAX_PATH_LEN];
    int klen = XeImageFileName[0].Length;
    if (klen >= (int)sizeof(kpath)) klen = (int)sizeof(kpath) - 1;
    memcpy(kpath, XeImageFileName[0].Buffer, klen);
    kpath[klen] = '\0';

    return kernel_to_dos_path(kpath, out, out_sz);
}

/* Publieke wrapper — gebruikt door main.c voor de net-init relaunch workaround */
int config_get_self_xbe_path(char *out, int out_sz)
{
    return get_xbe_filepath(out, out_sz);
}

static void get_ini_path(char *out, int out_sz)
{
    char dir[MAX_PATH_LEN];

    if (get_xbe_dir(dir, sizeof(dir))) {
        snprintf(out, out_sz, "%s\\savesyncx.ini", dir);
        return;
    }

    /* Fallback 1 */
    FILE *f = fopen("E:\\Apps\\SaveSyncX\\savesyncx.ini", "r");
    if (f) { fclose(f); strncpy(out, "E:\\Apps\\SaveSyncX\\savesyncx.ini", out_sz); return; }

    /* Fallback 2 */
    f = fopen("E:\\Applications\\SaveSyncX\\savesyncx.ini", "r");
    if (f) { fclose(f); strncpy(out, "E:\\Applications\\SaveSyncX\\savesyncx.ini", out_sz); return; }

    /* Laatste redmiddel: root van E: */
    strncpy(out, "E:\\savesyncx.ini", out_sz);
    out[out_sz - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/* Intern: whitespace trimmen (in-place, geeft pointer terug)          */
/* ------------------------------------------------------------------ */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;

    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        end--;
    *end = '\0';

    return s;
}

/* Splitst "key=value". Geeft 1 terug bij succes. */
static int parse_line(char *line, char **key_out, char **val_out)
{
    char *eq = strchr(line, '=');
    if (!eq) return 0;

    *eq      = '\0';
    *key_out = trim(line);
    *val_out = trim(eq + 1);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Standaardwaarden                                                     */
/* ------------------------------------------------------------------ */

void config_defaults(AppConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->host,        WEBDAV_DEFAULT_HOST,      sizeof(cfg->host)        - 1);
    cfg->port          = WEBDAV_DEFAULT_PORT;
    cfg->use_tls       = 0;   /* standaard plain HTTP — bestaande setups breken niet */
    cfg->tls_no_verify = 0;   /* als TLS aan staat, verificeer certificaat            */
    strncpy(cfg->remote_base, WEBDAV_DEFAULT_BASE_PATH, sizeof(cfg->remote_base) - 1);
    /* username en password blijven leeg; gebruiker moet die invullen */
}

/* ------------------------------------------------------------------ */
/* Laden                                                                */
/* ------------------------------------------------------------------ */

int config_load(AppConfig *cfg, const char *path)
{
    char ini_path[MAX_PATH_LEN];
    if (path && path[0])
        strncpy(ini_path, path, sizeof(ini_path) - 1);
    else
        get_ini_path(ini_path, sizeof(ini_path));
    ini_path[sizeof(ini_path) - 1] = '\0';

    FILE *f = fopen(ini_path, "r");
    if (!f)
        return -1;

    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        char *p = trim(line);
        if (*p == '#' || *p == ';' || *p == '\0') continue;

        char *key, *val;
        if (!parse_line(p, &key, &val)) continue;

        if      (strcmp(key, CFG_KEY_HOST)       == 0) strncpy(cfg->host,        val, sizeof(cfg->host)        - 1);
        else if (strcmp(key, CFG_KEY_PORT)       == 0) cfg->port          = atoi(val);
        else if (strcmp(key, CFG_KEY_USER)       == 0) strncpy(cfg->username,    val, sizeof(cfg->username)    - 1);
        else if (strcmp(key, CFG_KEY_PASS)       == 0) strncpy(cfg->password,    val, sizeof(cfg->password)    - 1);
        else if (strcmp(key, CFG_KEY_PATH)       == 0) strncpy(cfg->remote_base, val, sizeof(cfg->remote_base) - 1);
        else if (strcmp(key, CFG_KEY_TLS)        == 0) cfg->use_tls       = atoi(val);
        else if (strcmp(key, CFG_KEY_TLS_VERIFY) == 0) cfg->tls_no_verify = (atoi(val) == 0) ? 1 : 0;
        /* onbekende sleutels stilzwijgend negeren */
    }

    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Opslaan                                                              */
/* ------------------------------------------------------------------ */

int config_save(const AppConfig *cfg)
{
    char ini_path[MAX_PATH_LEN];
    get_ini_path(ini_path, sizeof(ini_path));

    FILE *f = fopen(ini_path, "w");
    if (!f)
        return -1;

    fprintf(f, "# SaveSyncX configuratie\n");
    fprintf(f, "# Pas dit bestand aan en herstart de applicatie.\n\n");
    fprintf(f, "%s=%s\n",  CFG_KEY_HOST, cfg->host);
    fprintf(f, "%s=%d\n",  CFG_KEY_PORT, cfg->port);
    fprintf(f, "%s=%s\n",  CFG_KEY_USER, cfg->username);
    fprintf(f, "%s=%s\n",  CFG_KEY_PASS, cfg->password);
    fprintf(f, "%s=%s\n",  CFG_KEY_PATH, cfg->remote_base);
    fprintf(f, "%s=%d\n",  CFG_KEY_TLS,  cfg->use_tls);
    /* tls_verify is het inverse van tls_no_verify voor leesbaarheid in het ini-bestand:
       tls_verify=1 betekent "verificeer het certificaat" (= tls_no_verify=0) */
    fprintf(f, "%s=%d\n",  CFG_KEY_TLS_VERIFY, cfg->tls_no_verify ? 0 : 1);

    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Validatie                                                            */
/* ------------------------------------------------------------------ */

int config_is_valid(const AppConfig *cfg)
{
    if (cfg->host[0] == '\0') return 0;
    if (cfg->port <= 0 || cfg->port > 65535) return 0;
    if (cfg->remote_base[0] == '\0') return 0;
    /* username/password mogen leeg zijn (server zonder auth) */
    return 1;
}

/* ------------------------------------------------------------------ */
/* nxNetInit relaunch-teller                                            */
/*                                                                       */
/* Sommige dashboards (bijv. LithiumX) laten de lwIP-stack in een       */
/* staat achter waarin een verse nxNetInit kan falen, omdat tcpip_init  */
/* al gedraaid heeft en er geen publieke API is om die thread af te     */
/* breken. Workaround: relaunch onze eigen XBE voor een schone memory   */
/* space. Om een oneindige relaunch-loop te voorkomen (bijv. als er     */
/* gewoon geen netwerkkabel in zit) houden we een pogingenteller bij    */
/* in een klein bestand op E:\ -- dat overleeft de relaunch, een        */
/* static counter in RAM niet.                                          */
/* ------------------------------------------------------------------ */

#define NET_RETRY_FILE "E:\\savesyncx_netretry.tmp"

/*
 * Leest de huidige pogingenteller. Geeft 0 terug als het bestand niet
 * bestaat (eerste poging) of niet leesbaar is.
 */
int config_net_retry_count_load(void)
{
    FILE *f = fopen(NET_RETRY_FILE, "r");
    if (!f) return 0;

    int count = 0;
    if (fscanf(f, "%d", &count) != 1) count = 0;
    fclose(f);
    return count;
}

/* Schrijft de pogingenteller weg. Geeft 1 terug bij succes, 0 bij falen. */
int config_net_retry_count_save(int count)
{
    FILE *f = fopen(NET_RETRY_FILE, "w");
    if (!f) return 0;
    fprintf(f, "%d\n", count);
    fclose(f);
    return 1;
}

/*
 * Verwijdert de tellerbestand. Wordt aangeroepen zodra nxNetInit
 * succesvol is, zodat de volgende KOUDE start weer bij 0 begint.
 */
void config_net_retry_count_clear(void)
{
    remove(NET_RETRY_FILE);
}

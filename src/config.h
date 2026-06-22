#ifndef CONFIG_H
#define CONFIG_H

#define APP_VERSION  "v1.3.1"
#define APP_TITLE    "SaveSyncX  " APP_VERSION

/* ── WebDAV Server Settings ─────────────────────────────────────────────── */
/* Edit these values, or store them in E:\savesync.ini on the Xbox           */

#define WEBDAV_DEFAULT_HOST     "192.168.1.1"
#define WEBDAV_DEFAULT_PORT     80
#define WEBDAV_DEFAULT_BASE_PATH "/SaveSync"

/* E:\savesyncx.ini keys */
#define CFG_KEY_HOST       "host"
#define CFG_KEY_PORT       "port"
#define CFG_KEY_USER       "username"
#define CFG_KEY_PASS       "password"
#define CFG_KEY_PATH       "remote_path"
#define CFG_KEY_TLS        "use_tls"
#define CFG_KEY_TLS_VERIFY "tls_verify"
#define CFG_INI_PATH       "E:\\savesyncx.ini"

/* ── Xbox Paths ──────────────────────────────────────────────────────────── */
#define XBOX_TDATA_PATH  "E:\\TDATA"
#define XBOX_UDATA_PATH  "E:\\UDATA"

/* ── Limits ──────────────────────────────────────────────────────────────── */
#define MAX_PATH_LEN     256
#define MAX_CRED_LEN     128
#define MAX_FILES        2048
#define TRANSFER_BUF_SZ  (64 * 1024)   /* 64 KB read/write buffer */

/* ── Config struct ───────────────────────────────────────────────────────── */
typedef struct {
    char host[MAX_CRED_LEN];
    int  port;
    char username[MAX_CRED_LEN];
    char password[MAX_CRED_LEN];
    char remote_base[MAX_PATH_LEN];
    int  use_tls;        /* 0 = plain HTTP (standaard), 1 = HTTPS via BearSSL  */
    int  tls_no_verify;  /* 0 = volledige certificaatverificatie (standaard)   */
                         /* 1 = geen verificatie (zelfgesigneerde certs, bijv. */
                         /*     Nextcloud met self-signed cert)                 */
} AppConfig;

/* Volledig pad (map + bestandsnaam) van de draaiende XBE, bijv.
   "E:\Apps\SaveSyncX\default.xbe". Voor gebruik met XLaunchXBE.
   Geeft 1 terug bij succes, 0 als het pad niet bepaald kon worden. */
int config_get_self_xbe_path(char *out, int out_sz);

/* nxNetInit relaunch-teller (zie config.c voor uitleg).
   Persisteert over XLaunchXBE-relaunches heen via een bestand op E:\. */
int  config_net_retry_count_load(void);
int  config_net_retry_count_save(int count);
void config_net_retry_count_clear(void);

/* Bepaal de map van de draaiende XBE. Geeft 1 terug bij succes. */
int  config_get_app_dir(char *out, int out_sz);

/* Load config from INI file.
   Als path NULL is, wordt het pad automatisch bepaald via config_get_app_dir.
   Returns 0 on success. */
int  config_load(AppConfig *cfg, const char *path);

/* Save config back to INI file. Returns 0 on success. */
int  config_save(const AppConfig *cfg);

/* Fill *cfg with compiled-in defaults. */
void config_defaults(AppConfig *cfg);

/* Returns 1 if cfg has the minimum required fields filled in, 0 otherwise. */
int config_is_valid(const AppConfig *cfg);

#endif /* CONFIG_H */

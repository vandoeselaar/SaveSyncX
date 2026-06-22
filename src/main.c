#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hal/video.h>
#include <hal/debug.h>
#include <hal/xbox.h>
#include <windows.h>
#include <nxdk/net.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <xboxkrnl/xboxkrnl.h>
#include <SDL.h>

#include "../lib/ui.h"
#include "../lib/util.h"
#include "titlescan.h"
#include "config.h"
#include "backup.h"
#include "restore.h"
#include "webdav.h"
#include "resign.h"
#include "download.h"
#include "github_fetch.h"

#include <winbase.h>

/* ── Savegame selectiescherm ──────────────────────────────────── */
#define CHAR_W    8

/* Max. aantal keer dat we onze eigen XBE relaunchen als nxNetInit
   faalt (workaround voor een vervuilde lwIP-staat na dashboards als
   LithiumX). Voorkomt een oneindige loop bij permanente netwerkfouten. */
#define NET_INIT_MAX_RETRIES 3



/* ── Read-only instellingen weergeven ─────────────────────────── */
static void do_settings_view(const AppConfig *cfg)
{
    unsigned char hddkey[HDDKEY_SIZE];
    char hddkey_str[HDDKEY_SIZE * 2 + 1];

    if (resign_read_hddkey(hddkey) == 0)
        resign_hddkey_to_hex(hddkey, hddkey_str);
    else
        strncpy(hddkey_str, "(read failed)", sizeof(hddkey_str));

    char buffer[512];
    snprintf(buffer, sizeof(buffer),
             "Host: %s\nPort: %d\nUsername: %s\nPassword: %s\nRemote path: %s\nHDDKey: %s",
             cfg->host, cfg->port,
             cfg->username, cfg->password, cfg->remote_base,
             hddkey_str);
    ui_message("Current settings", buffer);
}

/* ── Initiële WebDAV connectietest ────────────────────────────── */
static int initial_test(const AppConfig *cfg, const char *creds64)
{
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/upload_test.txt", cfg->remote_base);

    const char *body = "SaveSyncX upload test\r\n";
    int status;
    webdav_request("PUT", path,
                   cfg->host, cfg->port, creds64,
                   "Content-Type: application/octet-stream\r\n",
                   body, (int)strlen(body), NULL, 0, &status);
    return status;
}

/* ── Main ─────────────────────────────────────────────────────── */
void __cdecl main(void)
{
    /* Schijven mounten */
    static const struct { char letter; char *device; } drives[] = {
        { 'C', "\\Device\\Harddisk0\\Partition2" },
        { 'E', "\\Device\\Harddisk0\\Partition1" },
        { 'T', "\\Device\\Harddisk0\\Partition0" },
    };
    for (int i = 0; i < 3; i++) {
        char sym[8];
        sym[0]='\\'; sym[1]='?'; sym[2]='?'; sym[3]='\\';
        sym[4]=drives[i].letter; sym[5]=':'; sym[6]='\0';
        STRING s = { (USHORT)strlen(sym),   (USHORT)(strlen(sym)+1),   sym };
        STRING d = { (USHORT)strlen(drives[i].device),
                     (USHORT)(strlen(drives[i].device)+1),
                     drives[i].device };
        IoCreateSymbolicLink(&s, &d);
    }

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    /* ── Bepaal de app-map (naast default.xbe) ───────────────────── */
    char app_dir[MAX_PATH_LEN]  = "";
    char log_path[MAX_PATH_LEN] = "C:\\savesyncx.log";
    char ini_path[MAX_PATH_LEN] = "";

    if (config_get_app_dir(app_dir, sizeof(app_dir))) {
        snprintf(log_path, sizeof(log_path), "%s\\savesyncx.log", app_dir);
        snprintf(ini_path, sizeof(ini_path), "%s\\savesyncx.ini", app_dir);
    }

    /* Log initialiseren — zo vroeg mogelijk, vóór elke andere aanroep */
    log_init_path(log_path);
    log_print("=== SaveSyncX opgestart ===\n");
    log_print("app_dir : %s\n", app_dir[0] ? app_dir : "(onbekend)");
    log_print("log_path: %s\n", log_path);
    log_print("ini_path: %s\n", ini_path[0] ? ini_path : "(auto)");

    /* UI init (SDL window + joystick) */
    if (ui_init() != 0) {
        debugPrint("ui_init failed\n");
        return;
    }

    /* ── Netwerk ──────────────────────────────────────────────────
     * Sommige dashboards (bijv. LithiumX) laten lwIP in een vervuilde
     * staat achter, waardoor een verse nxNetInit hier kan falen.
     * Workaround: relaunch onze eigen XBE voor een schone memory space.
     * Een tellerbestand op E:\ voorkomt een oneindige relaunch-loop
     * (bijv. als er gewoon geen netwerkkabel in zit). */
    ui_message_nowait("SaveSyncX", "Initialize network - Waiting for DHCP...");
    if (nxNetInit(NULL) != 0) {
        int attempts = config_net_retry_count_load();

        if (attempts < NET_INIT_MAX_RETRIES) {
            char self_path[MAX_PATH_LEN];
            if (config_get_self_xbe_path(self_path, sizeof(self_path))) {
                config_net_retry_count_save(attempts + 1);
                log_print("nxNetInit failed (attempt %d/%d) - relaunching %s\n",
                          attempts + 1, NET_INIT_MAX_RETRIES, self_path);

                char msg[96];
                snprintf(msg, sizeof(msg),
                         "Network init failed, retrying (%d/%d)...",
                         attempts + 1, NET_INIT_MAX_RETRIES);
                ui_message_nowait("SaveSyncX", msg);
                SDL_Delay(500);

                ui_shutdown();
                XLaunchXBE(self_path);
                /* XLaunchXBE keert normaal niet terug; onderstaande
                   regel is alleen een vangnet als het toch misgaat. */
                return;
            }
            log_print("nxNetInit failed and self_path kon niet bepaald worden - geen relaunch mogelijk\n");
        } else {
            log_print("nxNetInit failed na %d pogingen - geef op\n", attempts);
        }

        config_net_retry_count_clear();
        ui_message("Error", "nxNetInit failed (network stack busy?)");
        ui_shutdown();
        return;
    }
    /* Netwerk succesvol geinitialiseerd: teller resetten zodat de
       volgende KOUDE start weer bij 0 begint. */
    config_net_retry_count_clear();
    SDL_Delay(2000);

    /* Configuratie laden */
    AppConfig cfg;
    config_defaults(&cfg);
    config_load(&cfg, ini_path[0] ? ini_path : NULL);
    webdav_init(&cfg);
    log_print("remote_base: %s\n", cfg.remote_base);

    if (!config_is_valid(&cfg)) {
        ui_message("Error", "Invalid config: check savesyncx.ini");
        ui_shutdown();
        nxNetShutdown();
        return;
    }

    /* Credentials */
    char creds[MAX_CRED_LEN * 2], creds64[MAX_CRED_LEN * 3];
    snprintf(creds, sizeof(creds), "%s:%s", cfg.username, cfg.password);
    webdav_base64_encode((const unsigned char *)creds, strlen(creds), creds64);

    /* ── Connectietests ──────────────────────────────────────────── */
    int webdav_ok = 0;
    int github_ok = 0;

    ui_message_nowait("SaveSyncX", "Test WebDAV connection...");
    int test_status = initial_test(&cfg, creds64);
    webdav_ok = (test_status == 200 || test_status == 201 || test_status == 204);
    log_print("WebDAV test: HTTP %d -> %s\n", test_status, webdav_ok ? "OK" : "FAIL");

    ui_message_nowait("SaveSyncX", "Test GitHub connection...");
    github_ok = github_test_connection();
    log_print("GitHub test: %s\n", github_ok ? "OK" : "FAIL");

    if (!webdav_ok && !github_ok) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "WebDAV: HTTP %d\nGitHub: unreachable\n\nNo services available.",
                 test_status);
        ui_message("Error", msg);
        ui_shutdown();
        nxNetShutdown();
        return;
    }
    if (!webdav_ok) {
        char msg[80];
        snprintf(msg, sizeof(msg),
                 "WebDAV unavailable (HTTP %d)\nBackup/Restore disabled.", test_status);
        ui_message_timeout("Warning", msg, 3000);
    }
    if (!github_ok) {
        ui_message_timeout("Warning", "GitHub unreachable\nDownload disabled.", 3000);
    }

    /* Titels scannen (één keer, voor Backup) */
    static TitleEntry titles[TITLESCAN_MAX];
    int n = titlescan_scan(titles, TITLESCAN_MAX);
    if (n <= 0) {
        ui_message("Error", "No save games found in E:\\UDATA");
        ui_shutdown();
        nxNetShutdown();
        return;
    }

    /* ── Hoofdmenu loop (gebruikt ui_main_menu) ───────────────────── */
    int flags = (webdav_ok ? UI_FLAG_WEBDAV_OK : 0)
              | (github_ok ? UI_FLAG_GITHUB_OK  : 0);

    while (1) {
        MenuID choice = ui_main_menu(flags);

        switch (choice) {
            case MENU_BACKUP:
                do_backup(titles, &n, creds64, &cfg);
                break;
            case MENU_RESTORE:
                do_restore(creds64, &cfg);
                break;
            case MENU_DOWNLOAD:                    
                do_download(&cfg);
                break;
            case MENU_SETTINGS:
                do_settings_view(&cfg);
                break;
            case MENU_CREDITS:
                ui_credits_screen();
                break;
            case MENU_EXIT:
                goto shutdown;
            default:
                break;
        }
    }

shutdown:
    nxNetShutdown();
    ui_shutdown();
}

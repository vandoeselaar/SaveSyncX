# SaveSyncX

Xbox homebrew application that backs up and restores game saves via WebDAV. Built with [nxdk](https://github.com/XboxDev/nxdk).

---

## Features

- Browse all save games found in `E:\UDATA` directly on the console
- Back up saves to any WebDAV server over your local network
- Restore saves from the server back to the console
- Automatic re-signing of EEPROM-locked (non-roamable) saves after restore
- Delete local saves or remote backup snapshots from the UI
- Game title lookup from a built-in database of 500+ titles
- Configuration loaded from `savesyncx.ini` next to `default.xbe` — no recompile needed
- Credits screen listing all references and dependencies

---

## Requirements

- Original Xbox (any hardware revision) with a modchip or softmod
- Network connection (DHCP)
- A WebDAV server — either self-hosted or a cloud provider via the included proxy

---

# To Do

## In progress / next up

- [ ] **Edit settings from UI**
  Settings screen is currently read-only. Add an on-screen keyboard so host,
  port, username, password and remote path can be edited and saved back to
  `savesyncx.ini` without needing a PC.

## Planned

- [ ] **TLS for direct WebDAV communication**
  The Xbox currently cannot make HTTPS connections, requiring the `webdav-proxy`
  workaround for cloud providers. Adding TLS (via mbedTLS, which ships with nxdk)
  would allow SaveSyncX to connect directly to HTTPS WebDAV servers without a
  proxy on the local network.

- [ ] **Add name or tag to a backup snapshot**
  Right now backups are identified only by their timestamp
  (`2025-06-01_14-30-00`). Allow the user to attach a short label
  (e.g. "before final boss", "100% run") that gets stored alongside the snapshot
  and shown in the restore list.

- [ ] **Game save browser with 100% complete saves**
  A built-in browser of community-sourced 100% complete save files, organised by
  title. User picks a game, downloads the save directly from the server, and it is
  restored (and re-signed where needed) in one step. Requires a hosted index file
  and a curated save repository.

## Ideas / backlog

- [ ] **Incremental / differential backup**
  Only upload files that have changed since the last backup run, based on file
  size or a stored checksum, to reduce transfer time for large saves.

- [ ] **Scheduled / automatic backup on launch**
  Optionally trigger a background backup when a title is launched from a
  compatible dashboard, without requiring the user to open SaveSyncX manually.

- [ ] **TDATA support**
  Currently only `E:\UDATA` is scanned. Some titles store additional data in
  `E:\TDATA` (downloadable content, patches). Include those in backup and restore.

- [ ] **Progress bar during transfer**
  Show bytes transferred and a visual progress bar during upload and download.

---

## Building

### Dependencies

- [nxdk](https://github.com/XboxDev/nxdk) — follow its setup instructions to get the toolchain working
- SDL2 and lwIP are included via nxdk — no separate install needed

### Build

```sh
git clone https://github.com/vandoeselaar/SaveSyncX
cd SaveSyncX
make -f Makefile.nxdk
```

This produces `default.xbe` and `savesyncx.iso`.

---

## Installation

Copy the following to `E:\Apps\SaveSyncX\` on your Xbox:

```
E:\Apps\SaveSyncX\
  default.xbe
  savesyncx.ini
```

SaveSyncX looks for `savesyncx.ini` in the same folder as `default.xbe`. It also writes its log file (`savesyncx.log`) there.

---

## Configuration

Edit `savesyncx.ini` before copying it to the Xbox:

```ini
host=192.168.1.100
port=80
username=youruser
password=yourpass
remote_path=/SaveSync
```

All fields except `username` and `password` are required. Leave credentials blank if your WebDAV server does not require authentication.

---

## WebDAV server options

SaveSyncX needs a WebDAV server to store backups. There are two approaches: self-hosted on your local network, or a cloud provider via the included HTTPS proxy.

### Option A — Self-hosted with Docker (recommended)

The easiest way to run a local WebDAV server is with [WsgiDAV](https://wsgidav.readthedocs.io/) via Docker.

Create a `docker-compose.yml`:

```yaml
services:
  webdav:
    image: jonasped/wsgidav
    restart: unless-stopped
    ports:
      - "80:80"
    volumes:
      - ./data:/var/webdav/data
    environment:
      USERNAME: youruser
      PASSWORD: yourpass
```

Start it:

```sh
docker compose up -d
```

Then configure `savesyncx.ini`:

```ini
host=192.168.1.100   # IP of the machine running Docker
port=80
username=youruser
password=yourpass
remote_path=/SaveSync
```

### Option B — Cloud WebDAV via the included proxy

The Xbox cannot make HTTPS connections, but most cloud WebDAV providers (Koofr, Nextcloud, Box) require them. The included `webdav-proxy` solves this by accepting plain HTTP from the Xbox and forwarding it to the upstream provider over HTTPS.

```
Xbox (HTTP) → webdav-proxy → cloud provider (HTTPS)
```

#### Setup

Edit `webdav-proxy/docker-compose.yml` and set `UPSTREAM_HOST` to your provider:

| Provider    | UPSTREAM_HOST               | remote_path prefix    |
|------------|-----------------------------|-----------------------|
| Koofr       | `app.koofr.net`             | `/dav/Koofr/`         |
| Nextcloud   | `yournextcloud.example.com` | `/remote.php/dav/`    |
| Box         | `dav.box.com`               | `/dav/`               |

```yaml
environment:
  UPSTREAM_HOST: "app.koofr.net"
  UPSTREAM_PORT: 443
  LISTEN_PORT: 8080
```

Start the proxy:

```sh
cd webdav-proxy
docker compose up -d
```

Then configure `savesyncx.ini`:

```ini
host=192.168.1.100        # IP of the machine running the proxy
port=8080
username=you@example.com
password=your-app-password
remote_path=/dav/Koofr/SaveSync
```

> **Note:** Most cloud providers require an application-specific password for WebDAV access rather than your account password. Generate one in your provider's account settings.

---

## Remote directory structure

SaveSyncX organises backups on the server as follows:

```
/SaveSync/
  <titleid>/
    2025-06-01_14-30-00/
      SaveGame.dat
      ...
    2025-06-10_09-15-42/
      SaveGame.dat
      ...
```

Each backup run creates a new timestamped snapshot. Existing backups are never overwritten.

---

## Non-roamable saves

Some games sign their save files using the console's EEPROM key (XboxHDKey). These saves will not load on a different console without being re-signed.

SaveSyncX automatically detects these games at restore time and re-signs the affected files using the local console's HDDKey. This works transparently when restoring to the same console. Restoring to a different console is not yet supported.

Games known to require re-signing include Halo 2, Burnout 3, Forza Motorsport, and others. See `src/resign.c` for the full list.

> **Forza Motorsport** uses a custom signing scheme that SaveSyncX cannot handle automatically. After restoring a Forza save, use [ForzaSign](https://github.com/feudalnate/Original-Xbox-Save-Resigner) on PC to re-sign the save before playing.

---

## Controls

| Button | Action |
|--------|--------|
| D-pad up / down | Navigate list |
| A | Select / confirm |
| B | Back / cancel |
| Y | Delete (save or backup snapshot) |
| Start | Confirm |

---

## Project structure

```
src/
  main.c          entry point, menu loop
  backup.c/h      backup flow (scan → upload)
  restore.c/h     restore flow (pick title → pick snapshot → download → resign)
  webdav.c/h      HTTP/WebDAV client (PUT, GET, MKCOL, PROPFIND, DELETE)
  resign.c/h      HDDKey read, HMAC-SHA1 re-signing, non-roamable game database
  titlescan.c/h   scan E:\UDATA for installed saves
  titledb.h       built-in title ID → name lookup table
  fileops.c/h     recursive directory upload/download helpers
  config.c/h      INI file parser, XBE path detection
lib/
  ui.c/h          SDL2 framebuffer UI (menus, lists, settings screen, credits)
  util.c/h        logging, string helpers
  font8x16.h      embedded bitmap font
webdav-proxy/
  proxy.py        plain HTTP → HTTPS WebDAV proxy
  Dockerfile
  docker-compose.yml
```

---

## References

- [nxdk](https://github.com/XboxDev/nxdk) — open-source Xbox development kit
- [feudalnate's Original Xbox Save Resigner](https://github.com/feudalnate/Original-Xbox-Save-Resigner) — signing research and XSavSig005 resign.ini
- [consolemods.org — Non-Roamable Saves](https://consolemods.org/wiki/Xbox:Games_with_Non-Roamable_(EEPROM-Locked)_Saves) — game list and offsets

---

## License

MIT

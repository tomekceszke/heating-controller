# heating-controller

IoT heating monitor: **two ESP32 boards** read DS18B20 temperature sensors and publish readings over MQTT to a local
Mosquitto + PostgreSQL backend (Proxmox CT `hc-data`, 192.168.11.16). **Collecting readings is the product: the UI,
notifications and the web app must never weaken it.**

## Status

- **Both boards run firmware 2.0.3** and collect normally: hc-1 (3 sensors) and hc-2 (7 sensors, refitted
  2026-09-21). Both are on the ESP-IDF 5.4.2 bootloader and the water/gate partition layout with native rollback.
  Migration story in `docs/IDF5_MIGRATION.md`.
- The OTA server on .15 is left stopped; start it (`cd ~/apps/ota-server && python3 ota_server.py`) only when
  publishing deliberately, and remember both boards poll the same file name.
- Updates from now on: `tools/build_release.sh`, publish `releases/heating-controller.bin` as
  `heating-controller.bin` on the OTA server, `POST /admin/su` (or the app's Settings tab) — **one board at a time**,
  because both poll the same file name and the download deletes it.

## Repository structure

```
firmware/   # ESP32 firmware (ESP-IDF 5.4.2 / C) on the shared framework home-idf
migrator/   # one-shot legacy -> 2.x image (home-idf hi_migrator)
server/     # hc-data backend: Mosquitto, PostgreSQL, MQTT->DB ingest, deploy + history migration
tools/      # build_release.sh (firmware + migrator), preview.py (UI with mocked data)
gcp/        # LEGACY: old Cloud Functions + service account keys (gitignored); delete after GCP shutdown
```

## Devices

One binary runs on both boards; everything board-specific is picked at runtime by STA MAC in
`firmware/main/config/devices.h` (hostname, app headline, ntfy title, the three Live numbers).

| Name | IP | MAC | Hostname | Sensors |
|---|---|---|---|---|
| heating-controller-1 | `192.168.11.248` | `24:62:AB:F2:00:DC` | `h-controller-1` | 3, incl. outdoor `9b00000009029f28` |
| heating-controller-2 | `192.168.11.249` | `EC:62:60:83:A6:6C` | `h-controller-2` | 7 |

DHCP reservations, ESP32 range 192.168.11.240–254. Target chip **ESP32**, 4 MB flash (DIO, 40 MHz).

## Firmware

### Commands

```sh
firmware/build.sh                          # idf.py build; uses ../../home-idf (sibling checkout) when present
HOME_IDF_FROM_GIT=1 firmware/build.sh      # against the pinned tag (main/idf_component.yml), what CI builds
firmware/build.sh size                     # app must fit ota_1 (1.875 MB)
firmware/build.sh -p /dev/cu.usbserial-0001 flash monitor   # USB recovery: bootloader + table + app
tools/build_release.sh                     # firmware + migrator into releases/ with sha256
tools/preview.py --board 2                 # the app with mocked API data, no device needed
```

- `sdkconfig` is generated from `sdkconfig.defaults` and is **not** committed; never run `idf.py set-target`.
- `partitions.csv` is the water/gate/floor layout: nvs/otadata/phy as in `two_ota`, ota_0 2M, ota_1 1.875M at the
  legacy ota_1 offset, coredump.
- A home-idf change needs a new tag, a bump in `firmware/main/idf_component.yml` **and**
  `migrator/main/idf_component.yml`, deleting `firmware/dependencies.lock`, then
  `HOME_IDF_FROM_GIT=1 firmware/build.sh` to regenerate it.
- Version is `set(PROJECT_VER ...)` in `firmware/CMakeLists.txt`, reported by `/admin/hw-status` and the app.

### Setup (required before building)

1. **Credentials** — copy and fill in:
   ```sh
   cp firmware/main/config/credentials-example.h firmware/main/config/credentials.h
   ```
   `WIFI_SSID`, `WIFI_PASS`, `HEADER_AUTHORIZATION_VALUE` (admin endpoints and the migrator commit),
   `AUTH_PASSWORD_*` (web sign-in, from `home-idf/tools/hash_password.py`; empty hash disables it),
   `NTFY_TOPIC` / `NTFY_ERROR_TOPIC`, `MQTT_USER` / `MQTT_PASS` (= `MQTT_DEVICE_PASS` in `server/secrets.env`).
   Values may be `obf1:` strings from `home-idf/tools/obfuscate.py` (obfuscation, not encryption).
2. **Certificate** — `firmware/certs/ota_server_cert_15.pem` (gitignored) must exist: self-signed cert of the local
   OTA HTTPS server, embedded as its TLS trust anchor.
3. **1-Wire drivers** — vendored in `firmware/components/`: `esp32-owb` (RMT) @ 60d977e and `esp32-ds18b20` @
   d677f09 (DavidAntliff), with small local IDF 5 fixes. They use the legacy RMT driver (two deprecation warnings
   are expected; it must go before ESP-IDF 6). Do **not** switch to `esp-idf-lib/ds18x20` (bit-banged, as in
   floor-heating-controller): on heating-controller-2's bus it found only 1 of 7 sensors (2026-09-11). The
   replacement is `firmware/docs/IDF6_MIGRATION.md`, planned as 2.1.0.

### Layout

```
firmware/
├── CMakeLists.txt          PROJECT_VER, HOME_IDF_LOCAL block
├── partitions.csv          ota_0 2M / ota_1 1.875M / coredump
├── sdkconfig.defaults
├── components/             esp32-owb + esp32-ds18b20 (vendored)
├── certs/                  OTA server TLS cert (NEVER committed)
├── web/                    app.html (Live / History / Settings on the home-idf app shell), manifest, icon
└── main/
    ├── main.c              boot: NVS -> device -> events/history -> sensor task -> health -> log ->
    │                       WiFi/notify/NTP/OTA -> MQTT -> auth/httpd
    ├── device.c            which board this is (STA MAC), sensor labels and Live highlights
    ├── sensor.c            1-Wire discovery and sampling task, per-sensor state, mutexed bus
    ├── metrics.c           esp-mqtt client on its own queue and task; topic contract with hc-ingest
    ├── events.c            RAM ring of 50 events, ntfy mapping
    ├── history.c           24 h of one-minute samples per sensor in RAM
    ├── api.c               routes on the home-idf HTTP server
    └── config/
        ├── config.h               non-secret configuration
        ├── devices.h              per-board table + sensor metadata (mirrors the DB `sensor` table)
        ├── credentials.h          secrets (NEVER committed)
        └── credentials-example.h  template
```

**Nothing the sensor task calls may block on the network.** `esp_mqtt_client_enqueue()` waits on the esp-mqtt API
lock with no timeout and the client task holds that lock across `transport_connect`, so publishing from the sensor
task stalls it for `CONFIG_MQTT_NETWORK_TIMEOUT_MS` whenever the broker is unreachable — that reset hc-1 on
2026-09-20. `metrics` therefore owns a queue and a task, and the sensor task only does a non-blocking
`metrics_post()`, the same shape as `water-controller/firmware/main/telemetry.c`.

The sensor task is the only code that touches the 1-Wire bus in a loop; `hi_health`'s predicate is
`sensor_alive() && hi_wifi_is_connected()`, so a stalled task rolls a fresh image back, and the task watchdog
(90 s, above `SAMPLE_PERIOD_S`) reboots a hung one.

Sensor IDs are 16 lowercase hex chars, ROM bytes 7→0 (e.g. `9b00000009029f28`), from `owb_string_from_rom_code()`.
They are keys in PostgreSQL `temperature_raw` (history since 2023) — **do not change the format**. Slots are
assigned on the first scan and never move, so a rescan keeps a known sensor's history series.

### HTTP API (port 80)

Common routes come from home-idf: `/`, `/api/session`, `/api/login`, `/api/logout`, `/api/reboot`, `/api/ota`,
`/admin/su`, `/admin/reboot`, `/admin/hw-status`. Heating routes:

| Method | Path | Guard | Description |
|---|---|---|---|
| GET | `/api/status` | session | `device`, `mac`, `sensors_alive`, `sensors[]` (id, label, group, dir, order, c, age_s, errors, lost), `highlights[]` (the three Live numbers), `mqtt`, `system` (with `bootloader_idf`) |
| GET | `/api/events` | session | last 50 events (sensors, sensor_lost, sensor_back, mqtt, outbox_full) |
| GET | `/api/history` | session | `{period_s, newest, newest_age_s, series:[{id,label,t:[0.1 °C or null]}]}`, oldest first, chunked |
| GET | `/admin/status` | `Authorization` header | same as `/api/status` |
| GET | `/sensor?sensor_id=<id>` | none | `{"value":21.5}`, `-273` on error/unknown id |

`/sensor` is unchanged from the legacy firmware and stays unauthenticated: `~/dev/ulanzi_tc001` polls the outdoor
sensor through it. It is the only route that reads the bus inline; everything else serves the last sample.

### Notifications (ntfy)

Topics follow the owner's convention `<prefix>-heating-controller` and `<prefix>-heating-controller-error`
(obfuscated in `credentials.h`). **Both boards share the one pair** and put their name in the title
("Heating 1" / "Heating 2").

- `NTFY_TOPIC`: boot (min priority).
- `NTFY_ERROR_TOPIC`: a sensor lost (3 failed reads in a row) or recovered, a rescan finding a different number of
  sensors, MQTT outbox full, every `ESP_LOGE` line, unexpected resets (home-idf, one-hour dedup).

### UI work

```sh
../home-idf/tools/dev_proxy.py 192.168.11.249 --page firmware/web/app.html --name h-controller   # against a board
tools/preview.py --board 2                                                                       # mocked, no board
tools/preview.py --board 2 --scene history --shoot docs/img                                      # README screenshots (app-<board>-<scene>.png)
```

Same layout as the other controllers (shared-main-view decision 2026-09-16): wordmark + status, headline, three
numbers, main view, latest events, tabs Live / History / Settings. There is no dock: the board has no actuator, so
there is nothing to switch. The main view is one row per circuit, pairing a supply sensor with its return and
showing the drop across it; circuits come from the `group` field of the sensor table. English.

### Editor (Zed / clangd)

- `firmware/.clangd` (tracked) points clangd at `build/compile_commands.json` and strips GCC-only flags.
  Run `firmware/build.sh` once (and again after adding source files) to refresh the database.
- Use Espressif's clangd (`~/.espressif/tools/esp-clang/<ver>/esp-clang/bin/clangd`) with
  `--query-driver=<xtensa-esp-elf bin>/xtensa-esp32-elf-*`; Apple/Zed clangd do not know the Xtensa target.
- `.zed/settings.json` + `.zed/tasks.json` (gitignored, machine paths). The activation script defines `idf.py` as an
  alias, which a one-line `source … && idf.py` does not see; tasks call `build.sh` instead.

### CI

`.github/workflows/firmware.yml`: firmware + migrator build with placeholder credentials and certificate, size
checks (app slot 1.875 MB, migrator 1 MB), both images as artifacts. There is no CD — releases go out through
`tools/build_release.sh` and the OTA server.

## Local backend (hc-data)

Proxmox LXC CT `hc-data`, **192.168.11.16** (Debian 13, 1 vCPU, 1 GB RAM, 8 GB disk), root SSH with key.
OTA server and UDP log server stay on 192.168.11.15.

| Service | Details |
|---|---|
| `mosquitto` | :1883, no anonymous; shared with water-controller: `install.sh` writes `/etc/mosquitto/{passwd.d/heating,acl.d/heating.acl}` and concatenates all fragments (never write `/etc/mosquitto/passwd` directly, it would drop the water users); users `heating-controller` (write `heating/+/temp/+`, rw `heating/+/status`) and `hc-ingest` (read `heating/#`); persistence on |
| `postgresql` (17) | DB `heating`, UTF-8. Roles: `hc_ingest` (peer auth, INSERT), `hc_read` (password, SELECT, from 192.168.11.0/24 — DataGrip) |
| `hc-ingest` | `server/ingest/hc_ingest.py` in `/opt/hc-server/venv`; persistent MQTT session + manual ack after DB commit → at-least-once; malformed messages logged and dropped |
| `hc-pg-backup.timer` | nightly `pg_dump -Fc` to `/var/backups/heating`, 14 days kept |

Schema (`server/db/schema.sql`): `temperature_raw(ts timestamptz, sensor_id, value, device)`, PK `(sensor_id, ts)`
(dedups redeliveries); `sensor` metadata; views ported from BigQuery (`temperature`, `temperature_last1h|6h`,
`temperature_outdoor[_recent]`, `temperature_power_recent`; `timestamp` column = Europe/Warsaw local time).
`device` is the publisher MAC (`2462abf200dc` = hc-1, `ec626083a66c` = hc-2), NULL for imported history.
The `sensor` table is the source of truth for labels; `firmware/main/config/devices.h` mirrors it so the app can
name sensors offline. Keep them in step when a sensor is added.

```sh
server/deploy.sh                 # copy server/ to /opt/hc-server + idempotent install.sh (needs server/secrets.env)
ssh root@192.168.11.16 journalctl -u hc-ingest -f
ssh root@192.168.11.16 'runuser -u postgres -- psql heating'
```

History: BigQuery `heating_controller_ds` (1,480,475 rows, 2023-02-11 → 2026-09-11 22:06 CEST cutover) imported via
`server/migrate/bq_export.py` + `import.sh`; raw export archived in `~/Documents/archive/heating-controller-bq-2026-09-11.tar.gz`.
GCP project `heating-controller` receives nothing since the cutover and is pending shutdown (then delete `gcp/`).

## Data flow

```
ESP32 x2
  └─ DS18B20 sensors (1-Wire, GPIO4)
  └─ WiFi → NTP (clock sync)
  └─ MQTT QoS1 heating/<mac>/temp/<sensor_id> → Mosquitto (hc-data :1883)
                                                   └─ hc-ingest → PostgreSQL heating.temperature_raw
```

## Infrastructure

- OTA server `https://192.168.11.15:8070`, file `heating-controller.bin` (`~/apps/ota-server` on .15), shared by
  both boards.
- UDP logs `192.168.11.15:1340`, both boards. They are collected by `heating-controller-logging.service`
  (`~/apps/logging_server_hc.py`, runs as `tomek`) and land in journald: `journalctl | grep 192.168.11.249`.
  `nc -ul 1340` there gets nothing, because that service already holds the port. The script line-buffers its
  output since 2026-09-21; without that, journald showed nothing until a few KB had built up.

## Rules

- **NEVER** commit or stage: `firmware/certs/*`, `firmware/main/config/credentials.h`, `gcp/keys/`,
  `*.private.env.json`, `server/secrets.env`, `server/migrate/out/`, `releases/`, `firmware/releases/`.
  Run `git check-ignore` before any `git add`.
- Commit messages clean, no AI attribution.
- Anything writing the bootloader or partition table needs the owner's approval at that moment.
- The MQTT topics and payload are the contract with `hc-ingest` and 3 years of history: never change them.
- Use `snprintf`, never `sprintf`; no VLAs sized from request data.

## TODO (cross-project, 2026-09-17)

- [ ] **Off-host backups of hc-data**: `hc-pg-backup` (`/var/backups/heating`) and water-controller's `wc-pg-backup`
  (`/var/backups/water`) only write to the CT's own disk. Choose a second location (Proxmox vzdump storage, NAS,
  another host) and copy the dumps there.
- [ ] **Data visualisation for hc-data** (shared with water-controller): dashboards over the `heating` and `water`
  databases, probably Grafana with a PostgreSQL data source on the read-only roles (`hc_read`, `wc_read`). Decide
  where it runs (hc-data CT or a separate one), then build dashboards (temperatures, outdoor, power; water flow,
  events, alerts).
- [ ] `server/install.sh` was changed locally (2026-09-15) to assemble Mosquitto passwd/acl from fragments shared
  with water-controller; keep it that way when committing.
- [ ] **ESP-IDF 6 readiness (2.1.0)**: replace the vendored legacy-RMT 1-Wire components with
  `espressif/onewire_bus` + `espressif/ds18b20`, verifying the sensor ids byte for byte on heating-controller-2
  first. See `firmware/docs/IDF6_MIGRATION.md`.
- [x] README with screenshots, as the other controllers have (2026-09-22).

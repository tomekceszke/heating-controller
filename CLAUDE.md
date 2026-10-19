# sensors-controller

Pipe temperature monitor (formerly **heating-controller**, renamed 2026-10-19): one ESP32 board (sc-2) reads
DS18B20 sensors at the floor pump set and the radiator manifold and publishes readings over MQTT to a local Mosquitto +
PostgreSQL backend (Proxmox CT `hc-data`, 192.168.11.16). The second board, hc-1 (heating-controller-1, at the boiler),
was switched off on 2026-10-15 and never ran as sensors-controller: its three sensors moved to
`../heatronic-controller/`. **Collecting readings is the product: the UI,
notifications and the web app must never weaken it.**

## Status

- **Rename 2026-10-19** (owner): heating-controller → sensors-controller, everywhere: repository, firmware 2.3.0
  (`project(sensors-controller)`, OTA file `sensors-controller.bin`, hostname `s-controller-2`, label „Sensors 2”,
  MQTT user `sensors-controller`, topics `sensors/<mac>/…`), broker fragments `sensors`, database `heating` →
  `sensors`, roles `hc_ingest`/`hc_read` → `sc_ingest`/`sc_read`, `hc-ingest`/`hc-pg-backup` → `sc-ingest`/
  `sc-pg-backup`, `/opt|/etc/hc-server` → `sc-server`, `/var/backups/heating` → `sensors`, Grafana datasource
  `sensors`. Unchanged: `/etc/mosquitto/conf.d/hc.conf` (whole broker; changing it needs a restart), the ntfy
  topics (secrets), the payload, tables and views, the migrator (`heating-migrator`, a legacy one-shot). Older
  entries below keep the old names (hc-2 = sc-2).
- 2026-10-19: hc-1 removed from the board table (`devices.h`), `tools/preview.py` and the README; goes out with
  the next release (2.3.0 still carries it, harmless: no board with that MAC runs).
- 2026-10-19 15:51–15:55: **cutover on production**. Database, roles and services renamed on hc-data (Mosquitto and
  PostgreSQL only reloaded), `sc-ingest` took over through a pre-created persistent session, Grafana / hv-status on
  `sensors`, plugin 1.3.0 on .13; sc-2 on **2.3.0** over OTA 15:54 (published as `heating-controller.bin`, sha256
  `9f789424…`, home-idf v0.1.17 from git, `build-git/`); ~50 s without readings around its reboot. Old retained
  `heating/…` topics, the `hc-ingest` session and the `heating` broker fragments removed. Still on the old name:
  the UDP log collector on .15 (`heating-controller-logging.service`, `logging_server_hc.py`; renaming it needs sudo
  there).
- **Both boards run firmware 2.1.0** (OTA 2026-09-21, hc-2 10:28, hc-1 10:53; sha256 `4b024ea2…`, commit `d8bfd59`:
  retained MQTT state topic, read-only admin key; home-idf v0.1.13) and collect normally: hc-1 (3 sensors) and hc-2
  (7 sensors, refitted 2026-09-21). Both are on the ESP-IDF 5.4.2 bootloader and the water/gate partition layout
  with native rollback. Migration story in `docs/IDF5_MIGRATION.md`.
- 2026-10-15 14:10: **hc-2 on firmware 2.2.0** over OTA (single install; hc-1 is off since the heatronic move):
  sensors read every 5 s, sent on a 0.2 °C change or 55 s after the last send (home-idf v0.1.17 `hi_deadband`).
  Built by `tools/build_release.sh` from the sibling home-idf checkout at tag v0.1.17 before the tag was pushed;
  `dependencies.lock` follows once it is.
- 2026-10-19: **firmware 2.4.0** built (`releases/sensors-controller.bin`, sha256 `7f7e4e11…`): `MAX_DEVICES` 8 → 16
  for the hydraulics sensors on hc-2, MQTT outbox 24 → 32 KB; DRAM static 52 %, heap ~23 KB lower. OTA by the owner
  before the new sensors go on the bus.
- The OTA server on .15 is left stopped; start it (`cd ~/apps/ota-server && python3 ota_server.py`) only when
  publishing deliberately.
- Updates from now on: `tools/build_release.sh`, publish `releases/sensors-controller.bin` as
  `sensors-controller.bin` on the OTA server, `POST /admin/su` (or the app's Settings tab); the download deletes
  it. A board still on 2.2.0 or older polls `heating-controller.bin`: publish 2.3.0 under that old name.

## Repository structure

```
firmware/   # ESP32 firmware (ESP-IDF 5.4.2 / C) on the shared framework home-idf
migrator/   # one-shot legacy -> 2.x image (home-idf hi_migrator)
server/     # hc-data backend: Mosquitto, PostgreSQL, MQTT->DB ingest, deploy + history migration
tools/      # build_release.sh (firmware + migrator), preview.py (UI with mocked data)
gcp/        # LEGACY: old Cloud Functions + service account keys (gitignored); delete after GCP shutdown
```

## Devices

Everything board-specific is picked at runtime by STA MAC in
`firmware/main/config/devices.h` (hostname, app headline, ntfy title, the three Live numbers).

| Name | IP | MAC | Hostname | Sensors |
|---|---|---|---|---|
| sensors-controller-2 (sc-2) | `192.168.11.249` | `EC:62:60:83:A6:6C` | `s-controller-2` | 7 |

DHCP reservations, ESP32 range 192.168.11.240–254. Target chip **ESP32**, 4 MB flash (DIO, 40 MHz).

### Sensor positions (owner, 2026-10-13)

All DS18B20s are bare strap-ons (cable ties, no insulation), so they read low: about 2 K under the water on copper,
4–5 K on the alu-pex 16 at 36 °C flow, and the pex ones lag by minutes. Pipes from the boiler to the manifolds are
soldered copper 22 mm OD; every loop and radiator connection is alu-pex 16.

| Sensor | Id | Position |
|---|---|---|
| Heater supply / return (heatronic, formerly hc-1) | `370000001287ce28` / `5900000008f0a828` | boiler pipes at the boiler, heating side after the 3-way valve (exact spot not given). **Supply to be re-mounted** (owner, 2026-10-13): 2 K under the floor set's supply sensor 4.5 m downstream |
| Outdoor (heatronic, formerly hc-1) | `9b00000009029f28` | outside |
| DHW coil supply / return (heatronic) | `5f0625920d100f28` / `030625922cad0f28` | the cylinder coil's upper / lower connection pipes, since 2026-10-14 23:21 on heatronic-controller's GPIO10 bus; `sensor` rows DHW / COIL / POWER, BACK (12, 13); confirmed at the first DHW charge 2026-10-15 |
| Floor supply (sc-2) | `7100000008ece828` | floor pump set (Prandelli RPM-R, garage), copper boiler supply **before** the thermostatic injection valve, next to floor-heating-controller's sensor: boiler water, not the floor's |
| Floor return | `a60416586fb5ff28` | the set's copper return to the boiler, right after the last valve (return regulating valve 7). It leaves the set's **upper** bar, so it carries mixed floor-supply water (~29 °C), not the loops' return |
| Hallway floor supply / return | `77041469f284ff28` / `f10316555c4bff28` | the middle loop (hall) at the floor set, alu-pex 16: supply on the pipe from the upper bar (flow meters), return on the pipe into the lower bar. The only sensors after the mixing |
| Kitchen radiators supply / return | `2800000008efc028` / `39000000150fc228` | the kitchen radiator's pair at the ground floor radiator manifold (garage), alu-pex 16 |
| Radiators return | `b700000014e83928` | that manifold's copper 22 return, after the last valve; its supply has no sensor (about the floor set's) |

Details and the readings against the boiler's own flow sensor: `../heatronic-controller/docs/INSTALLATION.md`,
Measurement points.

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
   floor-heating-controller): on sensors-controller-2's bus it found only 1 of 7 sensors (2026-09-11). The
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
    ├── metrics.c           esp-mqtt client on its own queue and task; topic contract with sc-ingest
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
(90 s) reboots a hung one.

Since 2.2.0 the task reads every `READ_PERIOD_S` (5 s) and sends a reading through `hi_deadband` (home-idf 0.1.17):
on a change of `SEND_DELTA_C` (0.2 °C) from the last one sent, or `SEND_MAX_GAP_S` (55 s) after it, so each sensor
has a row at least once a minute. Rows in `temperature_raw` are therefore unevenly spaced: average over time
buckets, not over rows. A sensor counts as lost after `SENSOR_LOST_AFTER_S` (180 s) of failed reads, as before;
the RAM history stays one column a minute.

Sensor IDs are 16 lowercase hex chars, ROM bytes 7→0 (e.g. `9b00000009029f28`), from `owb_string_from_rom_code()`.
They are keys in PostgreSQL `temperature_raw` (history since 2023) — **do not change the format**. Slots are
assigned on the first scan and never move, so a rescan keeps a known sensor's history series.

### HTTP API (port 80)

Common routes come from home-idf: `/`, `/api/session`, `/api/login`, `/api/logout`, `/api/reboot`, `/api/ota`,
`/admin/su`, `/admin/reboot`, `/admin/hw-status`. Controller routes:

| Method | Path | Guard | Description |
|---|---|---|---|
| GET | `/api/status` | session | `device`, `mac`, `sensors_alive`, `sensors[]` (id, label, group, dir, order, c, age_s, errors, lost), `highlights[]` (the three Live numbers), `mqtt`, `system` (with `bootloader_idf`) |
| GET | `/api/events` | session | last 50 events (sensors, sensor_lost, sensor_back, mqtt, outbox_full) |
| GET | `/api/history` | session | `{period_s, newest, newest_age_s, series:[{id,label,t:[0.1 °C or null]}]}`, oldest first, chunked |
| GET | `/admin/status` | `Authorization` header | same as `/api/status` |
| GET | `/sensor?sensor_id=<id>` | none | `{"value":21.5}`, `-273` on error/unknown id |

`/sensor` is unchanged from the legacy firmware and stays unauthenticated: `~/dev/ulanzi_tc001` polled the outdoor
sensor through it on hc-1 (since 2026-10-15 it polls heatronic-controller, .246). It is the only route that reads the bus inline; everything else serves the last sample.

### Notifications (ntfy)

Topics follow the owner's convention `<prefix>-heating-controller` and `<prefix>-heating-controller-error`
(obfuscated in `credentials.h`; kept through the rename, so phones stay subscribed). The board puts its name in the title
("Sensors 2").

- `NTFY_TOPIC`: boot (min priority).
- `NTFY_ERROR_TOPIC`: a sensor lost (3 failed reads in a row) or recovered, a rescan finding a different number of
  sensors, MQTT outbox full, every `ESP_LOGE` line, unexpected resets (home-idf, one-hour dedup).

### UI work

```sh
../home-idf/tools/dev_proxy.py 192.168.11.249 --page firmware/web/app.html --name s-controller   # against a board
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
| `mosquitto` | :1883, no anonymous; shared by every controller: `install.sh` writes `/etc/mosquitto/{passwd.d/sensors,acl.d/sensors.acl}` and concatenates all fragments (never write `/etc/mosquitto/passwd` directly, it would drop the other users); users `sensors-controller` (write `sensors/+/temp/+`, `sensors/+/state`, rw `sensors/+/status`) and `sc-ingest` (read `sensors/#`, plus `heatronic/+/temp/+` from heatronic.acl: heatronic-controller reads the former hc-1 sensors); persistence on |
| `postgresql` (17) | DB `sensors` (`heating` until 2026-10-19), UTF-8. Roles: `sc_ingest` (peer auth, INSERT), `sc_read` (password, SELECT, from 192.168.11.0/24 — DataGrip, Grafana) |
| `sc-ingest` | `server/ingest/sc_ingest.py` in `/opt/sc-server/venv`; persistent MQTT session + manual ack after DB commit → at-least-once; malformed messages logged and dropped |
| `sc-pg-backup.timer` | nightly `pg_dump -Fc` to `/var/backups/sensors`, 14 days kept |

Schema (`server/db/schema.sql`): `temperature_raw(ts timestamptz, sensor_id, value, device)`, PK `(sensor_id, ts)`
(dedups redeliveries), CHECK value −55..125 °C (`sc_ingest.py` drops anything outside first); `temperature_rejected`: the
legacy history's sensor errors (−2048 °C, outdoor 85 °C) with a reason; `sensor` metadata; views ported from BigQuery (`temperature`, `temperature_last1h|6h`,
`temperature_outdoor[_recent]`, `temperature_power_recent`; `timestamp` column = Europe/Warsaw local time).
`device` is the publisher MAC (`2462abf200dc` = hc-1, `ec626083a66c` = sc-2, `44b17617b48c` = heatronic since
2026-10-15), NULL for imported history.
The `sensor` table is the source of truth for labels; `firmware/main/config/devices.h` mirrors it so the app can
name sensors offline. Keep them in step when a sensor is added.

```sh
server/deploy.sh                 # copy server/ to /opt/sc-server + idempotent install.sh (needs server/secrets.env)
ssh root@192.168.11.16 journalctl -u sc-ingest -f
ssh root@192.168.11.16 'runuser -u postgres -- psql sensors'
```

History: BigQuery `heating_controller_ds` (1,480,475 rows, 2023-02-11 → 2026-09-11 22:06 CEST cutover) imported via
`server/migrate/bq_export.py` + `import.sh`; raw export archived in `~/Documents/archive/heating-controller-bq-2026-09-11.tar.gz`.
GCP project `heating-controller` was shut down on 2026-09-21 (`gcp/` is legacy, gitignored keys).

## Data flow

```
ESP32 (sc-2)
  └─ DS18B20 sensors (1-Wire, GPIO4)
  └─ WiFi → NTP (clock sync)
  └─ MQTT QoS1 sensors/<mac>/temp/<sensor_id> → Mosquitto (hc-data :1883)
                                                   └─ sc-ingest → PostgreSQL sensors.temperature_raw
heatronic-controller ── heatronic/<mac>/temp/<sensor_id> ──┘ (same payload, same ingest)
```

## Infrastructure

- OTA server `https://192.168.11.15:8070`, file `sensors-controller.bin` (`~/apps/ota-server` on .15;
  `heating-controller.bin` before 2.3.0).
- UDP logs `192.168.11.15:1340`. They are collected by `heating-controller-logging.service`
  (`~/apps/logging_server_hc.py`, runs as `tomek`; not renamed yet, needs sudo on .15) and land in journald: `journalctl | grep 192.168.11.249`.
  `nc -ul 1340` there gets nothing, because that service already holds the port. The script line-buffers its
  output since 2026-09-21; without that, journald showed nothing until a few KB had built up.

## Related projects

- `../heatronic-controller/` (started 2026-10-04): a read-only ESP32-C3 on the gas boiler's Heatronic 3 bus, next to
  the FW100. It reports the boiler's own flow/return temperatures, the AF outdoor sensor, the FW100's requested flow
  temperature, burner modulation, flame, pump and DHW. Cross-checks both ways: **Heater supply** `370000001287ce28`
  / **Heater return** `5900000008f0a828` against the boiler's flow/return, **Outdoor** `9b00000009029f28` against
  the AF sensor; this history (since 2023) gives the heating curve a baseline. Its stage 3 backend follows this
  repository's hc-data pattern (Mosquitto fragments, ingest with manual ack, PostgreSQL roles).
  Since 2026-10-15 its board carries hc-1's three sensors (plus two on the DHW coil) and publishes them on
  `heatronic/<mac>/temp/<sensor_id>` with this repository's payload; `sc-ingest` stores them in
  `sensors.temperature_raw` (history continues, only `device` changes). It also serves `/sensor` for the Ulanzi
  clock (.246). hc-1 is off.
- `../floor-heating-controller/`: pump on the floor circuit fed from the same boiler supply through a mixer.

## Rules

- **NEVER** commit or stage: `firmware/certs/*`, `firmware/main/config/credentials.h`, `gcp/keys/`,
  `*.private.env.json`, `server/secrets.env`, `server/migrate/out/`, `releases/`, `firmware/releases/`.
  Run `git check-ignore` before any `git add`.
- Commit messages clean, no AI attribution.
- Anything writing the bootloader or partition table needs the owner's approval at that moment.
- The MQTT topics and payload are the contract with `sc-ingest` and 3 years of history: never change them without
  a transition in the ingest and the ACL (as for the 2026-10-19 rename).
- Use `snprintf`, never `sprintf`; no VLAs sized from request data.

## TODO (cross-project, 2026-09-17)

- [ ] **Off-host backups of hc-data**: `sc-pg-backup` (`/var/backups/sensors`) and water-controller's `wc-pg-backup`
  (`/var/backups/water`) only write to the CT's own disk. Choose a second location (Proxmox vzdump storage, NAS,
  another host) and copy the dumps there.
- [x] **Data visualisation for hc-data**: `../hc-viz/` (Grafana on hc-data :3000 since 2026-10-07).
- [ ] `server/install.sh` was changed locally (2026-09-15) to assemble Mosquitto passwd/acl from fragments shared
  with water-controller; keep it that way when committing.
- [ ] **ESP-IDF 6 readiness (2.1.0)**: replace the vendored legacy-RMT 1-Wire components with
  `espressif/onewire_bus` + `espressif/ds18b20`, verifying the sensor ids byte for byte on sensors-controller-2
  first. See `firmware/docs/IDF6_MIGRATION.md`.
- [x] README with screenshots, as the other controllers have (2026-09-22).
- [ ] **More pipe sensors on hc-2 for the hydraulics check** (owner, 2026-10-19; heatronic-controller
  `docs/PROGRAMS.md`, Hydraulics). **Firmware first**: `MAX_DEVICES` is 8 and hc-2 has 7, so raise it (16) and
  `MQTT_OUTBOX_LIMIT_BYTES` with it, release, OTA, before any new sensor goes on the bus (2.4.0 built 2026-10-19, OTA pending). Then, strapped to the pipe
  and insulated like the others:
  - now (3 sensors at hand): **G-R7 living room** supply + return at M-RAD outlet 2 (alu-pex 16), and the **M-RAD
    supply bar** before outlet 1 (copper);
  - to buy: **G-F1 kitchen floor loop** supply + return at M-FLOOR (right loop), **G-F3 bathroom loop** pair, and the
    floor supply after the injection valve (M-FLOOR supply bar);
  - each new id: a `sensor` row (location, system_type, direction), the table in `Sensor positions` and in
    heatronic `INSTALLATION.md`, the hc-viz *Bilans hydrauliczny* panels;
  - with it: the owner reads the M-FLOOR loop flow meters (L/min, pump running) once, for loop power = flow × ΔT.

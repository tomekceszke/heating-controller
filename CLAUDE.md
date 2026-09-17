# heating-controller

IoT heating monitor: ESP32 firmware reads DS18B20 temperature sensors and publishes readings over MQTT to a local
Mosquitto + PostgreSQL backend (Proxmox CT `hc-data`, 192.168.11.16).

## Repository structure

```
firmware/   # ESP32 firmware (ESP-IDF / C)
server/     # hc-data backend: Mosquitto, PostgreSQL, MQTT→DB ingest, deploy + history migration
gcp/        # LEGACY: old Cloud Functions + service account keys (gitignored); delete after GCP shutdown
```

## Firmware

### Toolchain

**ESP-IDF v5.4.2** (same as `../floor-heating-controller`). Activate before any `idf.py` command:
```sh
source ~/.espressif/tools/activate_idf_v5.4.2.sh
```

### Setup (required before building)

1. **Credentials** — copy and fill in:
   ```sh
   cp firmware/main/config/credentials-example.h firmware/main/config/credentials.h
   # Set WIFI_SSID, WIFI_PASS, HEADER_AUTHORIZATION_VALUE, MQTT_PASS (= MQTT_DEVICE_PASS in server/secrets.env)
   ```
   Empty `HEADER_AUTHORIZATION_VALUE` locks all authenticated endpoints.

2. **Certificates** — `firmware/certs/ota_server_cert_15.pem` (gitignored) must exist: self-signed cert of the
   local OTA HTTPS server (embedded as its TLS trust anchor).

3. **1-Wire drivers** — vendored in `components/`: `esp32-owb` (RMT) @ 60d977e and `esp32-ds18b20` @ d677f09
   (DavidAntliff), with small local IDF 5 fixes. They use the legacy RMT driver (deprecation warning is
   expected; must migrate before ESP-IDF 6). Do **not** switch to `esp-idf-lib/ds18x20` (bit-banged, as in
   floor-heating-controller): on heating-controller-2's bus it found only 1 of 7 sensors (2026-09-11).
   ESP-IDF 6.x plan (driver replacement, ID checks, rollback image): `firmware/docs/IDF6_MIGRATION.md`.

### Commands

```sh
cd firmware

idf.py build                          # compile
idf.py size                           # app must fit the 1 MB OTA slot (partitions_two_ota)
idf.py -p /dev/ttyUSB0 flash         # flash to device
idf.py -p /dev/ttyUSB0 monitor       # serial console
idf.py -p /dev/ttyUSB0 flash monitor # flash + monitor in one step
idf.py menuconfig                     # interactive SDK configuration
```

Devices (DHCP reservations, ESP32 range 192.168.11.240–254); both run this firmware and OTA from the same `heating-controller.bin`:

| Name | IP | MAC | Sensors |
|---|---|---|---|
| heating-controller-1 | `192.168.11.248` | `24:62:AB:F2:00:DC` | 3, incl. outdoor `9b00000009029f28` |
| heating-controller-2 | `192.168.11.249` | `EC:62:60:83:A6:6C` | 7 |

Target chip: **ESP32** (`CONFIG_IDF_TARGET="esp32"`). `sdkconfig` is committed; never run `idf.py set-target` (resets it).

### Editor (Zed / clangd)

- `firmware/.clangd` (tracked) points clangd at `build/compile_commands.json` and strips GCC-only flags.
  Run `idf.py build` once (and again after adding source files) to refresh the database.
- Use Espressif's clangd (`~/.espressif/tools/esp-clang/<ver>/esp-clang/bin/clangd`) with
  `--query-driver=<xtensa-esp-elf bin>/xtensa-esp32-elf-*`; Apple/Zed clangd do not know the Xtensa target.
- `.zed/settings.json` + `.zed/tasks.json` (gitignored, machine paths): clangd binary, C as `.h` language,
  format-on-save off (no `.clang-format`), tasks `idf: build|size|flash|monitor|flash + monitor|menuconfig|fullclean`.
- The activation script defines `idf.py` as an alias, which a one-line `source … && idf.py` does not see.
  Tasks call `"$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py"` instead.

### Layout

```
firmware/
├── CMakeLists.txt
├── sdkconfig               # active build config (committed)
├── components/             # esp32-owb + esp32-ds18b20 (vendored)
├── certs/                  # OTA server TLS cert (NEVER committed)
├── http_examples/          # REST API requests (auth in *.private.env.json, gitignored)
└── main/
    ├── CMakeLists.txt      # explicit SRCS + PRIV_REQUIRES
    ├── *.c / *.h           # one header per module
    └── config/
        ├── config.h               # non-secret configuration (committed)
        ├── credentials.h          # secrets (NEVER committed)
        └── credentials-example.h  # template
```

### Module breakdown

| File | Responsibility |
|------|---------------|
| `main/main.c` | Boot sequence: WiFi (+NVS) → log dispatch → OTA → API → NTP → metrics (MQTT) → sensors → monitoring task |
| `main/wifi.c` | WiFi STA, exponential backoff reconnect (max `WIFI_RETRY_DELAY_S`), `wifi_is_connected()`. Boot waits max 60 s, then continues; no reboot on reconnect |
| `main/log_dispatch.c` | `esp_log_set_vprintf` hook: rewrites uptime to `(HH:MM:SS)` once NTP synced, prints to serial, forwards via `log_udp` |
| `main/log_udp.c` | UDP socket to `LOG_UDP_IP:LOG_UDP_PORT` |
| `main/ntp.c` | SNTP sync (readings are published only once synced), TZ CET/CEST, `boot_time`, `ntp_time_synced()` |
| `main/ota.c` | HTTPS OTA from `OTA_URL`; on success deletes bin from server and reboots |
| `main/api.c` | HTTP server (port 80), see API table |
| `main/sensor.c` | DS18B20 via esp32-owb RMT; scan (retries until found) + periodic sampling; mutex-guarded 1-Wire bus; publishes every `SAMPLE_PERIOD_S` valid reads once time is synced (WiFi down → buffered in MQTT outbox) |
| `main/metrics.c` | esp-mqtt client to `MQTT_BROKER_URI`; `metrics_publish()` enqueues QoS1 `heating/<mac>/temp/<sensor_id>` `{"ts":<unix>,"value":<float>}`; LWT/retained `heating/<mac>/status` `online`/`offline`; outbox bounded by `MQTT_OUTBOX_LIMIT_BYTES` and `CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS` (1 h) |

Sensor IDs are 16 lowercase hex chars, ROM bytes 7→0 (e.g. `9b00000009029f28`), from `owb_string_from_rom_code()`.
They are keys in PostgreSQL `temperature_raw` (history since 2023) — do not change the format.

### HTTP API

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | `/sensor?sensor_id=<id>` | no | `{"value":21.5}`; `-273` on error/unknown id |
| GET | `/admin/hw-status` | no | JSON: `up_since`, `free_mem_kb` |
| POST | `/admin/su` | yes | Trigger OTA update |
| POST | `/admin/reboot` | yes | Restart device |

Auth = `Authorization` header equal to `HEADER_AUTHORIZATION_VALUE`; otherwise 401.

### Key config (`firmware/main/config/config.h`)

| Constant | Value | Notes |
|----------|-------|-------|
| `WIFI_RETRY_DELAY_S` | 60 | max reconnect backoff / boot wait |
| `NTP_MAX_ATTEMPTS` / `NTP_RETRY_DELAY_S` | 20 / 10 | boot-time sync wait; SNTP keeps polling after |
| `OTA_URL` | `https://192.168.11.15:8070/…` | local HTTPS server for OTA binary |
| `LOG_UDP_IP` / `LOG_UDP_PORT` | `192.168.11.15` / 1340 | UDP log destination (`nc -ul 1340`) |
| `HTTPD_PORT` | 80 | on-device API port |
| `MQTT_BROKER_URI` | `mqtt://192.168.11.16:1883` | Mosquitto on hc-data (user `heating-controller`, pass in `credentials.h`) |
| `MQTT_TOPIC_PREFIX` | `heating` | topic root |
| `MQTT_OUTBOX_LIMIT_BYTES` | 24576 | max buffered readings (~40 min for 7 sensors) |
| `GPIO_DS18B20_0` | `GPIO_NUM_4` | 1-Wire data pin |
| `MAX_DEVICES` | 8 | max sensors on bus |
| `TEMP_SENSOR_SCAN_RETRY_S` | 30 | rescan interval when no sensor found |
| `SAMPLE_PERIOD_S` | 60 | seconds between sensor reads |
| `INVALID_TEMPERATURE_INDICATOR` | -273 | sentinel for read failure |

### Git & security rules

- **NEVER** commit or stage: `firmware/certs/*`, `main/config/credentials.h`, `gcp/keys/`, `*.private.env.json`,
  `server/secrets.env`, `server/migrate/out/`.
- Verify against `.gitignore` before any `git add`.
- Use `snprintf`, never `sprintf`; no VLAs sized from request data.

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
ESP32
  └─ DS18B20 sensors (1-Wire, GPIO4)
  └─ WiFi → NTP (clock sync)
  └─ MQTT QoS1 heating/<mac>/temp/<sensor_id> → Mosquitto (hc-data :1883)
                                                   └─ hc-ingest → PostgreSQL heating.temperature_raw
```

## TODO (cross-project, 2026-09-15)

- [ ] **Put the project on GitHub** (repository + first commit): owner decides when, not yet.
- [ ] **CI (urgent)**: GitHub Actions build of the firmware on every push (see `../water-controller/.github/workflows/firmware.yml`: `espressif/esp-idf-ci-action`, placeholder credentials and certificate). Needs the GitHub repository first.
- [ ] **Off-host backups of hc-data**: `hc-pg-backup` (`/var/backups/heating`) and water-controller's `wc-pg-backup`
  (`/var/backups/water`) only write to the CT's own disk. Choose a second location (Proxmox vzdump storage, NAS, another host) and copy the dumps there.
- [ ] **Data visualisation for hc-data** (shared with water-controller): dashboards over the `heating` and `water`
  databases, probably Grafana with a PostgreSQL data source on the read-only roles (`hc_read`, `wc_read`). Decide where
  it runs (hc-data CT or a separate one), then build dashboards (temperatures, outdoor, power; water flow, events, alerts).
- [ ] `server/install.sh` was changed locally (2026-09-15) to assemble Mosquitto passwd/acl from fragments shared with
  water-controller; keep it that way when committing.
- [ ] Move the copied modules (wifi, log, ntp, ota, notify, health, auth, api) to the shared framework **home-idf** (`~/dev/home-idf`, public, pinned by tag in `main/idf_component.yml`), including `hi_secret` obfuscated credentials (`tools/obfuscate.py`) and the Wi-Fi reconnect fix (no `vTaskDelay` in the event handler).
- [ ] When a web UI is added: use the unified sign-in page from home-idf (planned).

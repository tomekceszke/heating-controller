# Migration to firmware 2.x (bootloader and partition table)

## Why

| | Legacy firmware (production until the migration) | Firmware 2.x |
|---|---|---|
| Framework | own copies of wifi / log / ntp / ota / header auth | home-idf v0.1.12 (`hi_*`), shared sign-in page and app shell |
| Bootloader | ESP-IDF 5.4.x (the config says 5.4.1; the migrator check records the real one) | ESP-IDF 5.4.2, native app rollback |
| Table | stock `partitions_two_ota`: factory 1M @0x10000, ota_0 1M @0x110000, ota_1 1M @0x210000 | ota_0 2M @0x10000, ota_1 1.875M @0x210000, coredump 64K @0x3F0000 |
| Image | 899 KB (88 % of a slot) | 907 KB: 89 % of a legacy slot, 47 % of ota_1 |

nvs 0x9000, otadata 0xd000 and phy 0xf000 keep their offsets; flash is DIO / 40 MHz / 4 MB in both. Same layout and
procedure as water-controller (2026-09-15), gate-controller (2026-09-15) and floor-heating-controller (2026-09-17).

The legacy bootloader would still boot 2.x (home-idf's `hi_health` rolls back in software), but the image would leave
almost nothing free in a 1 MB slot and the device would stay the only one without a coredump partition and native
rollback.

## How

`migrator/` is a one-shot image built against the legacy table (must fit 1 MB) with the 2.x bootloader and table
embedded (`home-idf/tools/build_migrator.sh`). heating-controller only reads sensors, so there is nothing to hold in
a safe state while it runs: the whole cost is the couple of minutes of missing readings, and the primary key
`(sensor_id, ts)` of `temperature_raw` makes that gap harmless.

1. The legacy firmware downloads `heating-controller.bin` at boot or on `POST /admin/su`; at that moment the file is
   the migrator. It deletes the file after installing it.
2. The migrator copies itself to 0x210000 if it does not run from there, and reboots.
3. `GET /migrator` shows the checks: blob sha256, flash settings of the bootloader in flash, **ESP-IDF version of the
   bootloader in flash**, table MD5, nvs/otadata/phy offsets, ota_1 = running image, last reset not a brownout.
   Nothing is written unless all pass (stage `ready`).
4. `POST /migrator/commit` (admin header): partition table, then bootloader, each verified with up to 3 retries and a
   restore of the old content on failure.
5. The new bootloader boots the migrator, which downloads firmware 2.x (same file name) into ota_0.
6. Firmware 2.x verifies itself (sensor task alive and WiFi within 300 s, at most 3 unverified resets), otherwise it
   rolls back to the migrator.

## Two boards, one file name

`heating-controller-1` and `heating-controller-2` run the same binary and poll the same
`https://192.168.11.15:8070/heating-controller.bin`, and the migrator deletes the file once it has it. The two
migrations therefore run **strictly one after the other**: publish, migrate and verify one board completely before
touching the other. While a migrator is published under that name, a board that happens to reboot would pick it up
and land in stage `ready`; that is recoverable (commit it, or reflash the legacy image) but reorders the evening.

## Risks (owner decision 2026-09-17: no spare-board rehearsal, both boards the same evening)

- The legacy firmware has no rollback: if the migrator image failed to boot, the board would loop until reflashed.
  The same migrator code (home-idf `hi_migrator`) runs on water, gate and floor-heating since 2026-09-15.
- A power cut during the table or bootloader write (well under a second) bricks the board.
- Recovery for both: USB, download mode IO0 + EN, `firmware/build.sh -p <port> flash` (writes bootloader, table and
  app). NVS survives.
- Do not open a serial monitor during the commit: DTR/RTS resets the board.
- Rollback to the legacy firmware: `firmware/releases/heating-controller-2026-09-11-mqtt.bin` is the known-good
  pre-migration image (sha256 `e579bede7e4f2f15…`). After the table has been rewritten it must be flashed over USB,
  not published for OTA.

## Production procedure (per board)

1. Credentials: `firmware/main/config/credentials.h` needs `AUTH_PASSWORD_*` (web sign-in) and `NTFY_TOPIC` /
   `NTFY_ERROR_TOPIC` in addition to the legacy values; the admin header stays the same, so the legacy `/admin/su`
   and the migrator commit use the same value.
2. `tools/build_release.sh`; record both sha256 values.
3. Preflight: `GET http://<ip>/admin/hw-status` and one `GET /sensor?sensor_id=<id>` on the legacy firmware; note the
   time and the newest `temperature_raw` row for that MAC. Start the OTA server on .15 and `nc -ul 1340` for the log.
4. `scp releases/heating-migrator.bin 192.168.11.15:apps/ota-server/builds/heating-controller.bin`.
5. Legacy `POST /admin/su` with the Authorization header (runs synchronously, the device restarts into the migrator).
6. `GET http://<ip>/migrator` until stage `ready`; read the bootloader version line. **Stop if any check fails.**
7. `scp releases/heating-controller.bin 192.168.11.15:apps/ota-server/builds/heating-controller.bin`
   **before** the commit.
8. `POST /migrator/commit` with the admin header; firmware 2.0.0 follows within a minute.
9. Verify: `/admin/hw-status` shows 2.0.0; app sign-in works; `system.partition = ota_0`,
   `bootloader_idf = v5.4.2`, `pending_verify` false after the health window; the right number of sensors with
   plausible readings; one row per sensor per minute in `temperature_raw`; "Started" on the ntfy topic.

## Production log

### 2026-09-17: heating-controller-2 migrated, then stuck in a reboot loop

Release built from commit `50210db` + home-idf v0.1.12 (local checkout, identical to the tag apart from a README
line): firmware 2.0.0 `68c88ecbbaeff157…` (928 768 B), migrator `3b3e82b30e3fd056…` (829 472 B).

| Time (CEST) | Step |
|---|---|
| 23:33 | Preflight: both boards up since 2026-09-11, all 10 sensors reading, one row per sensor per minute in `temperature_raw` |
| 23:34 | Migrator published as `heating-controller.bin`, OTA server started on .15 |
| 23:37:05 | hc-2 legacy `POST /admin/su` downloaded the migrator, sent DELETE at 23:37:20, rebooted |
| 23:38:38 | `GET /migrator` stage `ready` from 0x210000, every check ok. The bootloader in flash had **no description at all** (pre-ESP-IDF 5.1), older than floor-heating's v5.4.1 |
| 23:39:39 | Firmware 2.0.0 published |
| 23:39:52 | `POST /migrator/commit`: partition table written and verified (49 ms), bootloader written and verified (373 ms) |
| 23:40:15 | Migrator downloaded 2.0.0 into ota_0 and sent DELETE |
| 23:40:22 | 2.0.0 boots: `partition` ota_0, `idf` v5.4.2, `bootloader_idf` v5.4.2, all 7 sensors found with plausible readings and no errors |
| 23:41 → | **Reboot loop**: the board restarts roughly every 8 s, `reset_reason` "task watchdog" |

The migration itself worked: the table, the bootloader and the image are exactly what was intended, and the sensor
code reads all 7 sensors. The fault is in 2.0.0's sensor task, which subscribes to the task watchdog and then sleeps
a whole `SAMPLE_PERIOD_S` in one `xTaskDelayUntil`. That only works if the watchdog timeout is longer than the
sampling period, and it was not: **`CONFIG_ESP_TASK_WDT_TIMEOUT_S` is capped at 60 by Kconfig**, so the 90 in
`sdkconfig.defaults` was silently dropped and the build used the default of 5 s.

2.0.1 (commit `aa47c4c`) waits in 2 s slices and feeds the watchdog between them, with the timeout at 10 s.

**It cannot be delivered over the air.** The board is only up for about 8 s per boot; the OTA starts around 5 s in
(after the WiFi association) and is killed a second later — the log shows `esp_https_ota: Writing to <ota_1>
partition at offset 0x210000` at 23:47:28 and the next boot banner at 23:47:29. Those partial writes have
overwritten the migrator that was sitting in ota_1; that costs nothing but the fallback image, because otadata
still points at ota_0 and the board keeps booting.

Recovery is USB (owner decision 2026-09-17 23:50: leave it looping overnight, reflash the next day):

```sh
firmware/build.sh -p /dev/cu.usbserial-XXXX flash     # bootloader, table and app, 2.0.1
```

Download mode if it does not take: hold **IO0**, press and release **EN**, release **IO0**; press **EN** after
flashing. NVS survives. The 2.0.1 images are in `releases/` (firmware `90870d6f85d3dafd…`, migrator
`c289d40c91bd271e…`).

State left overnight: hc-2 loops and collects nothing; **hc-1 was never touched and still runs the legacy firmware**
with its 3 sensors. The OTA server on .15 is stopped and the published file renamed to
`heating-controller-2.0.1.bin.parked`, so a reboot of hc-1 cannot pull 2.x onto its legacy partition layout.

### Still to check once hc-2 runs 2.0.1

- MQTT never connected in the loop: `transport_base: Failed to open a new connection: 32772` about a second after
  boot, every boot. `metrics_start()` runs before `hi_wifi_wait_connected()`, so the first attempt is expected to
  fail and esp-mqtt retries after 10 s — which the board never reached. Water-controller starts its client the same
  way, so this is probably a symptom rather than a second fault, but confirm the broker connects and
  `temperature_raw` fills again before migrating hc-1.

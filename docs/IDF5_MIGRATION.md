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

_(filled in during the migration)_

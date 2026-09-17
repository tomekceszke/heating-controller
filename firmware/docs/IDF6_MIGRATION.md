# ESP-IDF 6.x migration notes

Status (2026-09-11): firmware runs on **ESP-IDF 5.4.2**. The only known 6.x blocker is the 1-Wire driver.

## Blocker: legacy RMT driver

`components/esp32-owb` (DavidAntliff/esp32-owb @ 60d977e + local IDF 5 fixes) uses the legacy RMT API
(`driver/rmt.h`). ESP-IDF 6.0 removes it:

> "The legacy RMT driver `driver/rmt.h` is deprecated since version 5.0 ... Starting from version 6.0,
> the legacy driver is completely removed."
> — [ESP-IDF 6.0 migration guide, peripherals](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/migration-guides/release-6.x/6.0/peripherals.html)

Upstream esp32-owb is not being ported (README: tested on IDF 4.4.4 and 5.0.1 only).

## Rejected option: `esp-idf-lib/ds18x20` (bit-banged GPIO)

Used by floor-heating-controller (1 sensor, short cable) and tried here on 2026-09-11.
On heating-controller-2 (7 sensors) the bus search found **only 1 of 7** devices: the driver aborts a bit
when the bus is not high within 10 µs and ignores that failure during search. esp32-owb's own README
says CPU-delay timing "proved to be too unreliable". Do not use it for this project.

## Recommended: Espressif `onewire_bus` + `ds18b20`

- [`espressif/onewire_bus`](https://components.espressif.com/components/espressif/onewire_bus) — new RMT
  driver backend (`onewire_new_bus_rmt`), also a UART backend (`onewire_new_bus_uart`) as fallback for long
  buses. Enumeration: `onewire_new_device_iter()`, `onewire_device_iter_get_next()`, `onewire_del_device_iter()`.
  Config: `onewire_bus_config_t.bus_gpio_num`, `.flags.en_pull_up`; `onewire_bus_rmt_config_t.max_rx_bytes`.
- [`espressif/ds18b20`](https://components.espressif.com/components/espressif/ds18b20) —
  `ds18b20_new_device_from_enumeration()`, `ds18b20_trigger_temperature_conversion_for_all()`,
  `ds18b20_get_temperature()`, `ds18b20_get_device_address()`.
- Maintained by Espressif, works on 5.4.2 too → migrate the driver **before** the IDF bump.
- [API reference](https://espressif.github.io/idf-extra-components/latest/onewire_bus/index.html)

## Plan

1. Keep a known-good image to roll back to (see below).
2. Rewrite only `main/sensor.c` on the new API. Keep the bus mutex, `MAX_DEVICES` bound,
   rescan-until-found, skip failed reads, per-sensor error counters.
3. **Sensor IDs must stay byte-identical** — they are keys in PostgreSQL `temperature_raw` on hc-data, with history back to 2023 (and `9b00000009029f28` is polled by
   `~/dev/ulanzi_tc001`). Current format: ROM bytes 7→0 as lowercase hex (`owb_string_from_rom_code`).
   The new API docs do not state the 64-bit layout of `onewire_device_address_t`; verify the new build prints
   exactly the known IDs below before trusting it.
4. Test on **heating-controller-2 first** (7-sensor bus = the hard case). Pass: 7/7 found, 0 read errors,
   per-sensor error counters flat over 24 h, readings arriving in hc-data (`temperature_raw`, one row per sensor per minute). Fail → OTA the known-good image back.
5. Then heating-controller-1, delete `components/esp32-owb` + `components/esp32-ds18b20`.
6. Only then bump ESP-IDF to 6.x (check the 6.0 migration guide for the rest: e.g. cJSON/`json` component
   moved to the registry, other legacy drivers removed — this project uses none of those).

## Known sensor IDs

| Unit | IP | Sensors |
|---|---|---|
| heating-controller-1 | 192.168.11.248 | `370000001287ce28`, `5900000008f0a828`, `9b00000009029f28` (outdoor) |
| heating-controller-2 | 192.168.11.249 | `2800000008efc028`, `7100000008ece828`, `39000000150fc228`, `b700000014e83928`, `77041469f284ff28`, `a60416586fb5ff28`, `f10316555c4bff28` |

## Known-good image

`heating-controller-2026-09-11-mqtt.bin` (sha256 prefix `e579bede7e4f2f15`), IDF 5.4.2 + esp32-owb RMT,
metrics over MQTT to hc-data. Stored in `firmware/releases/` (gitignored) and on the OTA host as
`~/apps/ota-server/builds/heating-controller-2026-09-11-mqtt.bin`. (The older `…-owb-rmt.bin` still posts
to GCP and is not a valid rollback once GCP is shut down.) Roll back: copy it to
`builds/heating-controller.bin`, then `POST /admin/su` with the Authorization header.

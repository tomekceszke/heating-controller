# hc-data backend

Local replacement for the former GCP backend (Cloud Function + BigQuery). Runs on the Proxmox CT
`hc-data` (192.168.11.16, Debian 13).

```
ESP32 ── MQTT QoS1 ──> mosquitto :1883 ──> hc-ingest (Python) ──> PostgreSQL "heating"
         heating/<mac>/temp/<sensor_id>  {"ts":<unix>,"value":<float>}
         heating/<mac>/status            online|offline (retained, LWT)
```

## Deploy

```sh
cp server/secrets.env.example server/secrets.env   # first time only; fill with openssl rand -base64 24 | tr -d '/+='
server/deploy.sh                                   # copies server/ to /opt/hc-server, runs install.sh (idempotent)
```

`MQTT_DEVICE_PASS` must equal `MQTT_PASS` in `firmware/main/config/credentials.h`.
`PG_READ_PASS` is the password of the read-only `hc_read` role (e.g. DataGrip: `192.168.11.16:5432/heating`).

## Files

| Path | Purpose |
|---|---|
| `install.sh` | apt packages, Mosquitto users/ACL, PostgreSQL roles + schema, venv, systemd units |
| `mosquitto/hc.conf`, `mosquitto/acl` | broker config (no anonymous, per-user topic ACL) |
| `db/schema.sql` | tables + views (ported from BigQuery), grants; safe to re-apply |
| `ingest/hc_ingest.py` | MQTT → PostgreSQL; persistent session, acks only after commit |
| `backup/hc-pg-backup.*` | nightly `pg_dump -Fc` to `/var/backups/heating`, 14 days |
| `migrate/bq_export.py`, `migrate/import.sh` | one-off BigQuery history export/import (done 2026-09-11) |

## Operations

```sh
ssh root@192.168.11.16 journalctl -u hc-ingest -f
ssh root@192.168.11.16 'runuser -u postgres -- psql heating -c "select * from temperature_last1h"'
ssh root@192.168.11.16 'mosquitto_sub -u hc-ingest -P … -t "heating/#" -v'
```

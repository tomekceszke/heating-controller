#!/usr/bin/env bash
# Loads server/migrate/out/ (from bq_export.py) into the heating DB on hc-data. Idempotent.
# Usage: server/migrate/import.sh [root@host]
set -euo pipefail
HOST=${1:-root@192.168.11.16}
OUT="$(cd "$(dirname "$0")" && pwd)/out"
[ -f "$OUT/temperature_raw.csv.gz" ] && [ -f "$OUT/sensor.csv" ] || { echo "run bq_export.py first" >&2; exit 1; }

ssh "$HOST" 'install -d -m 755 /tmp/hc-import'
scp -q "$OUT/temperature_raw.csv.gz" "$OUT/sensor.csv" "$HOST:/tmp/hc-import/"
ssh "$HOST" 'chmod 644 /tmp/hc-import/* && runuser -u postgres -- psql -d heating -v ON_ERROR_STOP=1' <<'SQL'
\timing on
BEGIN;
CREATE TEMP TABLE staging_sensor (LIKE sensor);
\copy staging_sensor FROM '/tmp/hc-import/sensor.csv' WITH (FORMAT csv, HEADER true)
INSERT INTO sensor SELECT * FROM staging_sensor
ON CONFLICT (sensor_id) DO UPDATE SET
    display_order = EXCLUDED.display_order, location = EXCLUDED.location,
    system_type = EXCLUDED.system_type, direction = EXCLUDED.direction, pipe_type = EXCLUDED.pipe_type;

CREATE TEMP TABLE staging_raw (ts timestamptz, sensor_id text, value double precision);
\copy staging_raw FROM PROGRAM 'gunzip -c /tmp/hc-import/temperature_raw.csv.gz' WITH (FORMAT csv, HEADER true)
SELECT count(*) AS staged,
       count(*) - count(DISTINCT (sensor_id, ts)) AS duplicates_in_export
FROM staging_raw;
INSERT INTO temperature_raw (ts, sensor_id, value)
SELECT ts, sensor_id, value FROM staging_raw
ON CONFLICT (sensor_id, ts) DO NOTHING;
COMMIT;
ANALYZE temperature_raw;
SELECT count(*) AS total_rows, count(*) FILTER (WHERE device IS NULL) AS history_rows,
       min(ts) AS first, max(ts) AS last FROM temperature_raw;
SQL
ssh "$HOST" 'rm -rf /tmp/hc-import'

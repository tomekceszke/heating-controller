-- heating-controller schema (PostgreSQL). Idempotent: safe to re-apply.
-- Ported from BigQuery heating-controller.heating_controller_ds.

CREATE TABLE IF NOT EXISTS sensor (
    sensor_id     text PRIMARY KEY CHECK (sensor_id ~ '^[0-9a-f]{16}$'),
    display_order int,
    location      text,
    system_type   text,
    direction     text,
    pipe_type     text
);

CREATE TABLE IF NOT EXISTS temperature_raw (
    ts        timestamptz NOT NULL,
    sensor_id text        NOT NULL,     -- no FK: readings from unknown sensors are still stored
    value     double precision,
    device    text,                     -- publisher MAC from the MQTT topic; NULL for BigQuery history
    PRIMARY KEY (sensor_id, ts)         -- also dedups QoS1 redeliveries and history re-imports
);

CREATE INDEX IF NOT EXISTS temperature_raw_ts_brin ON temperature_raw USING brin (ts);

-- Views keep BigQuery names and columns. "timestamp" is Europe/Warsaw local time (timestamp without tz).

CREATE OR REPLACE VIEW temperature AS
SELECT
    t.ts AT TIME ZONE 'Europe/Warsaw' AS "timestamp",
    -- display name, e.g. "10_MAIN_HEATER_POWER"
    trim(upper(
        d.display_order::text || '_' ||
        d.system_type || '_' ||
        d.location ||
        coalesce('_' || d.direction, '')
    )) AS sensor,
    d.location,
    d.system_type,
    d.direction,
    d.pipe_type,
    round(t.value::numeric, 1)::double precision AS value
FROM temperature_raw t
LEFT JOIN sensor d ON t.sensor_id = d.sensor_id
WHERE t.value BETWEEN -100 AND 100
ORDER BY "timestamp", sensor;

-- Recent-window views filter on raw ts (index-friendly, DST-safe).
-- The BigQuery originals compared Warsaw local time with UTC CURRENT_DATETIME(), and
-- last1h filtered sensor <> 'OUTDOOR', which never matched a display name; location is used instead.
CREATE OR REPLACE VIEW temperature_last1h AS
SELECT
    t.ts AT TIME ZONE 'Europe/Warsaw' AS "timestamp",
    trim(upper(d.display_order::text || '_' || d.system_type || '_' || d.location ||
               coalesce('_' || d.direction, ''))) AS sensor,
    d.location, d.system_type, d.direction, d.pipe_type,
    round(t.value::numeric, 1)::double precision AS value
FROM temperature_raw t
LEFT JOIN sensor d ON t.sensor_id = d.sensor_id
WHERE t.ts > now() - interval '1 hour'
  AND t.value BETWEEN -100 AND 100
  AND d.location IS DISTINCT FROM 'OUTDOOR'
ORDER BY "timestamp", sensor;

CREATE OR REPLACE VIEW temperature_last6h AS
SELECT
    t.ts AT TIME ZONE 'Europe/Warsaw' AS "timestamp",
    trim(upper(d.display_order::text || '_' || d.system_type || '_' || d.location ||
               coalesce('_' || d.direction, ''))) AS sensor,
    d.location, d.system_type, d.direction, d.pipe_type,
    round(t.value::numeric, 1)::double precision AS value
FROM temperature_raw t
LEFT JOIN sensor d ON t.sensor_id = d.sensor_id
WHERE t.ts > now() - interval '6 hours'
  AND t.value BETWEEN -100 AND 100
ORDER BY "timestamp", sensor;

CREATE OR REPLACE VIEW temperature_outdoor AS
SELECT
    t.ts AT TIME ZONE 'Europe/Warsaw' AS "timestamp",
    round(t.value::numeric, 1)::double precision AS value
FROM temperature_raw t
JOIN sensor d ON t.sensor_id = d.sensor_id
WHERE d.location = 'OUTDOOR'
  AND t.value BETWEEN -100 AND 100
ORDER BY "timestamp";

CREATE OR REPLACE VIEW temperature_outdoor_recent AS
SELECT
    t.ts AT TIME ZONE 'Europe/Warsaw' AS "timestamp",
    round(t.value::numeric, 1)::double precision AS value
FROM temperature_raw t
JOIN sensor d ON t.sensor_id = d.sensor_id
WHERE d.location = 'OUTDOOR'
  AND t.value BETWEEN -100 AND 100
ORDER BY t.ts DESC
LIMIT 1;

CREATE OR REPLACE VIEW temperature_power_recent AS
SELECT
    t.ts AT TIME ZONE 'Europe/Warsaw' AS "timestamp",
    round(t.value::numeric, 1)::double precision AS value
FROM temperature_raw t
JOIN sensor d ON t.sensor_id = d.sensor_id
WHERE d.location = 'HEATER'
  AND d.direction = 'POWER'
  AND t.value BETWEEN -100 AND 100
ORDER BY t.ts DESC
LIMIT 1;

-- Privileges (roles are created by install.sh)
GRANT SELECT ON sensor TO hc_ingest;
GRANT SELECT, INSERT ON temperature_raw TO hc_ingest;
GRANT SELECT ON ALL TABLES IN SCHEMA public TO hc_read;

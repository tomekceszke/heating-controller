#!/usr/bin/env python3
"""MQTT -> PostgreSQL bridge for sensors-controller (formerly heating-controller) temperature readings.

Subscribes to sensors/<device>/temp/<sensor_id> (and heatronic/<device>/temp/<sensor_id>: heatronic-controller took
over board 1's sensors at the boiler in 2026-10) with a persistent session and manual
acks: a reading is acked only after it is committed, so the broker redelivers anything
lost to a crash or DB outage. The (sensor_id, ts) primary key absorbs duplicates.
"""
import json
import logging
import math
import os
import re
import sys
import time

import paho.mqtt.client as mqtt
import psycopg

MQTT_HOST = os.environ.get("MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_USER = "sc-ingest"
MQTT_PASS = os.environ["MQTT_INGEST_PASS"]
PG_DSN = os.environ.get("PG_DSN", "dbname=sensors")   # peer auth over the local socket

# Same payload on both; the device column tells the boards apart. (heating/ was this board's prefix before 2.3.0.)
TEMP_TOPICS = ("sensors/+/temp/+", "heatronic/+/temp/+")
STATUS_TOPICS = ("sensors/+/status",)
SENSOR_ID_RE = re.compile(r"^[0-9a-f]{16}$")
MIN_TS = 1577836800                                    # 2020-01-01: older means clock not synced
MAX_FUTURE_S = 86400
VALUE_MIN, VALUE_MAX = -55.0, 125.0                    # DS18B20 range; the CHECK on temperature_raw.value

INSERT_SQL = """
    INSERT INTO temperature_raw (ts, sensor_id, value, device)
    VALUES (to_timestamp(%s), %s, %s, %s)
    ON CONFLICT (sensor_id, ts) DO NOTHING
"""

log = logging.getLogger("sc-ingest")


def parse_reading(topic, payload):
    """Returns (ts, sensor_id, value, device) or raises ValueError."""
    parts = topic.split("/")
    if len(parts) != 4:
        raise ValueError("unexpected topic")
    device, sensor_id = parts[1], parts[3]
    if not SENSOR_ID_RE.match(sensor_id):
        raise ValueError("bad sensor_id")
    data = json.loads(payload)
    ts, value = data.get("ts"), data.get("value")
    if not isinstance(ts, int) or isinstance(ts, bool):
        raise ValueError("ts not an int")
    if ts < MIN_TS or ts > time.time() + MAX_FUTURE_S:
        raise ValueError(f"ts out of range: {ts}")
    if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value):
        raise ValueError("value not a finite number")
    if not VALUE_MIN <= value <= VALUE_MAX:
        # Must stay in step with the table's CHECK, or the insert fails and the message is redelivered forever
        raise ValueError(f"value out of sensor range: {value}")
    return ts, sensor_id, float(value), device


class Ingest:
    def __init__(self):
        self.db = psycopg.connect(PG_DSN, autocommit=True)
        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id="sc-ingest",
            clean_session=False,
            protocol=mqtt.MQTTv311,
            manual_ack=True,
        )
        self.client.username_pw_set(MQTT_USER, MQTT_PASS)
        self.client.reconnect_delay_set(min_delay=1, max_delay=60)
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message = self.on_message

    def on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            log.error("MQTT connect failed: %s", reason_code)
            return
        log.info("MQTT connected (session present: %s)", flags.session_present)
        client.subscribe([(t, 1) for t in TEMP_TOPICS + STATUS_TOPICS])

    def on_disconnect(self, client, userdata, flags, reason_code, properties):
        log.warning("MQTT disconnected: %s", reason_code)

    def on_message(self, client, userdata, msg):
        if msg.topic.endswith("/status"):
            log.info("%s: %s", msg.topic, msg.payload.decode(errors="replace"))
            client.ack(msg.mid, msg.qos)
            return
        try:
            reading = parse_reading(msg.topic, msg.payload)
        except ValueError as e:     # json.JSONDecodeError is a ValueError
            log.warning("Dropping %s %r: %s", msg.topic, msg.payload[:100], e)
            client.ack(msg.mid, msg.qos)
            return
        try:
            self.db.execute(INSERT_SQL, reading)
        except psycopg.Error as e:
            # Leave unacked; exit so systemd restarts us and the broker redelivers.
            log.error("DB insert failed, exiting: %s", e)
            client.disconnect()
            sys.exit(1)
        client.ack(msg.mid, msg.qos)
        log.debug("Stored %s", reading)

    def run(self):
        self.client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
        self.client.loop_forever(retry_first_connection=True)


def main():
    logging.basicConfig(
        level=os.environ.get("LOG_LEVEL", "INFO"),
        format="%(levelname)s %(message)s",     # journald adds timestamps
    )
    Ingest().run()


if __name__ == "__main__":
    main()

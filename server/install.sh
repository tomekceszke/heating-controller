#!/usr/bin/env bash
# Idempotent provisioning of hc-data (Debian 13) for sensors-controller. Run as root from /opt/sc-server (deploy.sh does
# this). Mosquitto and PostgreSQL are only restarted when their own config changes (a fresh host); otherwise reloaded,
# because every controller shares them.
set -euo pipefail
cd "$(dirname "$0")"

SECRETS=/etc/sc-server/secrets.env
[ -f "$SECRETS" ] || { echo "$SECRETS missing" >&2; exit 1; }
set -a; . "$SECRETS"; set +a
: "${MQTT_DEVICE_PASS:?}" "${MQTT_INGEST_PASS:?}" "${PG_READ_PASS:?}"

echo "== packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
# CT has no system locale: make sure the cluster is created UTF-8, not SQL_ASCII
apt-get install -y -q postgresql-common
grep -q '^initdb_options' /etc/postgresql-common/createcluster.conf ||
    echo "initdb_options = '--encoding=UTF8 --locale=C.UTF-8'" >> /etc/postgresql-common/createcluster.conf
apt-get install -y -q mosquitto mosquitto-clients postgresql python3-venv

echo "== mosquitto"
MQ_RESTART=0
# Comments do not count: only a change in the settings themselves needs a restart
cmp -s <(grep -v '^#' mosquitto/hc.conf) <(grep -vs '^#' /etc/mosquitto/conf.d/hc.conf) || MQ_RESTART=1
install -m 644 mosquitto/hc.conf /etc/mosquitto/conf.d/hc.conf
# passwd and acl are assembled from per-project fragments (water-controller shares this broker)
install -d -m 750 -o root -g mosquitto /etc/mosquitto/passwd.d /etc/mosquitto/acl.d
install -m 640 -o root -g mosquitto mosquitto/sensors.acl /etc/mosquitto/acl.d/sensors.acl
PASSWD_TMP=$(mktemp)
mosquitto_passwd -c -b "$PASSWD_TMP" sensors-controller "$MQTT_DEVICE_PASS"
mosquitto_passwd -b "$PASSWD_TMP" sc-ingest "$MQTT_INGEST_PASS"
install -m 640 -o root -g mosquitto "$PASSWD_TMP" /etc/mosquitto/passwd.d/sensors
rm -f "$PASSWD_TMP"
cat /etc/mosquitto/passwd.d/* > /etc/mosquitto/passwd.new
cat /etc/mosquitto/acl.d/*.acl > /etc/mosquitto/acl.new
chown root:mosquitto /etc/mosquitto/passwd.new /etc/mosquitto/acl.new
chmod 640 /etc/mosquitto/passwd.new /etc/mosquitto/acl.new
mv /etc/mosquitto/passwd.new /etc/mosquitto/passwd
mv /etc/mosquitto/acl.new /etc/mosquitto/acl
systemctl enable -q mosquitto
if [ "$MQ_RESTART" = 1 ]; then systemctl restart mosquitto; else systemctl reload mosquitto; fi

echo "== postgresql"
PG_VER=$(ls /etc/postgresql | sort -V | tail -1)
PG_CONF=/etc/postgresql/$PG_VER/main
PG_RESTART=0
grep -qxF "listen_addresses = '*'" "$PG_CONF/conf.d/hc.conf" 2>/dev/null || PG_RESTART=1
install -m 644 -o postgres -g postgres /dev/stdin "$PG_CONF/conf.d/hc.conf" <<<"listen_addresses = '*'"
HBA_LINE="host    sensors         sc_read         192.168.11.0/24         scram-sha-256"
grep -qxF "$HBA_LINE" "$PG_CONF/pg_hba.conf" || echo "$HBA_LINE" >> "$PG_CONF/pg_hba.conf"
systemctl enable -q postgresql
if [ "$PG_RESTART" = 1 ]; then systemctl restart postgresql; else systemctl reload postgresql; fi

psql_admin() { runuser -u postgres -- psql -v ON_ERROR_STOP=1 -q "$@"; }
psql_admin -v read_pass="$PG_READ_PASS" <<'SQL'
SELECT 'CREATE ROLE sc_ingest LOGIN' WHERE NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'sc_ingest') \gexec
SELECT 'CREATE ROLE sc_read LOGIN'   WHERE NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'sc_read') \gexec
ALTER ROLE sc_read PASSWORD :'read_pass';
SELECT 'CREATE DATABASE sensors' WHERE NOT EXISTS (SELECT FROM pg_database WHERE datname = 'sensors') \gexec
SQL
psql_admin -d sensors -f db/schema.sql

echo "== ingest"
id sc_ingest >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin sc_ingest
[ -x venv/bin/python ] || python3 -m venv venv
venv/bin/pip install -q --disable-pip-version-check -r ingest/requirements.txt
install -m 644 ingest/sc-ingest.service /etc/systemd/system/sc-ingest.service

echo "== backups"
install -d -m 750 -o postgres -g postgres /var/backups/sensors
install -m 644 backup/sc-pg-backup.service backup/sc-pg-backup.timer /etc/systemd/system/

systemctl daemon-reload
systemctl enable -q --now sc-pg-backup.timer
systemctl enable -q sc-ingest
systemctl restart sc-ingest

echo "== status"
systemctl is-active mosquitto postgresql sc-ingest sc-pg-backup.timer

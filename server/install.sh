#!/usr/bin/env bash
# Idempotent provisioning of hc-data (Debian 13). Run as root from /opt/hc-server (deploy.sh does this).
set -euo pipefail
cd "$(dirname "$0")"

SECRETS=/etc/hc-server/secrets.env
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
install -m 644 mosquitto/hc.conf /etc/mosquitto/conf.d/hc.conf
# passwd and acl are assembled from per-project fragments (water-controller shares this broker)
install -d -m 750 -o root -g mosquitto /etc/mosquitto/passwd.d /etc/mosquitto/acl.d
install -m 640 -o root -g mosquitto mosquitto/acl /etc/mosquitto/acl.d/heating.acl
PASSWD_TMP=$(mktemp)
mosquitto_passwd -c -b "$PASSWD_TMP" heating-controller "$MQTT_DEVICE_PASS"
mosquitto_passwd -b "$PASSWD_TMP" hc-ingest "$MQTT_INGEST_PASS"
install -m 640 -o root -g mosquitto "$PASSWD_TMP" /etc/mosquitto/passwd.d/heating
rm -f "$PASSWD_TMP"
cat /etc/mosquitto/passwd.d/* > /etc/mosquitto/passwd.new
cat /etc/mosquitto/acl.d/*.acl > /etc/mosquitto/acl.new
chown root:mosquitto /etc/mosquitto/passwd.new /etc/mosquitto/acl.new
chmod 640 /etc/mosquitto/passwd.new /etc/mosquitto/acl.new
mv /etc/mosquitto/passwd.new /etc/mosquitto/passwd
mv /etc/mosquitto/acl.new /etc/mosquitto/acl
systemctl enable -q mosquitto
systemctl restart mosquitto

echo "== postgresql"
PG_VER=$(ls /etc/postgresql | sort -V | tail -1)
PG_CONF=/etc/postgresql/$PG_VER/main
install -m 644 -o postgres -g postgres /dev/stdin "$PG_CONF/conf.d/hc.conf" <<<"listen_addresses = '*'"
HBA_LINE="host    heating         hc_read         192.168.11.0/24         scram-sha-256"
grep -qxF "$HBA_LINE" "$PG_CONF/pg_hba.conf" || echo "$HBA_LINE" >> "$PG_CONF/pg_hba.conf"
systemctl enable -q postgresql
systemctl restart postgresql

psql_admin() { runuser -u postgres -- psql -v ON_ERROR_STOP=1 -q "$@"; }
psql_admin -v read_pass="$PG_READ_PASS" <<'SQL'
SELECT 'CREATE ROLE hc_ingest LOGIN' WHERE NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'hc_ingest') \gexec
SELECT 'CREATE ROLE hc_read LOGIN'   WHERE NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'hc_read') \gexec
ALTER ROLE hc_read PASSWORD :'read_pass';
SELECT 'CREATE DATABASE heating' WHERE NOT EXISTS (SELECT FROM pg_database WHERE datname = 'heating') \gexec
SQL
psql_admin -d heating -f db/schema.sql

echo "== ingest"
id hc_ingest >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin hc_ingest
[ -x venv/bin/python ] || python3 -m venv venv
venv/bin/pip install -q --disable-pip-version-check -r ingest/requirements.txt
install -m 644 ingest/hc-ingest.service /etc/systemd/system/hc-ingest.service

echo "== backups"
install -d -m 750 -o postgres -g postgres /var/backups/heating
install -m 644 backup/hc-pg-backup.service backup/hc-pg-backup.timer /etc/systemd/system/

systemctl daemon-reload
systemctl enable -q --now hc-pg-backup.timer
systemctl enable -q hc-ingest
systemctl restart hc-ingest

echo "== status"
systemctl is-active mosquitto postgresql hc-ingest hc-pg-backup.timer

#!/usr/bin/env bash
# Deploys server/ to hc-data and provisions it. Usage: server/deploy.sh [root@host]
set -euo pipefail
HOST=${1:-root@192.168.11.16}
cd "$(dirname "$0")"
[ -f secrets.env ] || { echo "server/secrets.env missing (see secrets.env.example)" >&2; exit 1; }

# Replace everything under /opt/sc-server except the venv
ssh "$HOST" 'install -d -m 755 /opt/sc-server && install -d -m 700 /etc/sc-server &&
             find /opt/sc-server -mindepth 1 -maxdepth 1 ! -name venv -exec rm -rf {} +'
COPYFILE_DISABLE=1 tar -czf - --exclude secrets.env --exclude migrate/out --exclude __pycache__ . |
    ssh "$HOST" 'tar -xzf - --no-same-owner -C /opt/sc-server'
ssh "$HOST" 'umask 077 && cat > /etc/sc-server/secrets.env' < secrets.env
ssh "$HOST" 'bash /opt/sc-server/install.sh'

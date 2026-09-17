#!/usr/bin/env bash
# Deploys server/ to hc-data and provisions it. Usage: server/deploy.sh [root@host]
set -euo pipefail
HOST=${1:-root@192.168.11.16}
cd "$(dirname "$0")"
[ -f secrets.env ] || { echo "server/secrets.env missing (see secrets.env.example)" >&2; exit 1; }

# Replace everything under /opt/hc-server except the venv
ssh "$HOST" 'install -d -m 755 /opt/hc-server && install -d -m 700 /etc/hc-server &&
             find /opt/hc-server -mindepth 1 -maxdepth 1 ! -name venv -exec rm -rf {} +'
COPYFILE_DISABLE=1 tar -czf - --exclude secrets.env --exclude migrate/out --exclude __pycache__ . |
    ssh "$HOST" 'tar -xzf - --no-same-owner -C /opt/hc-server'
ssh "$HOST" 'umask 077 && cat > /etc/hc-server/secrets.env' < secrets.env
ssh "$HOST" 'bash /opt/hc-server/install.sh'

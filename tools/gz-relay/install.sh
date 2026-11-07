#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
# Installs the shadPS4 P2P relay as a service on Ubuntu (e.g. an Oracle Cloud free VM).
# Usage: curl -fsSL https://raw.githubusercontent.com/dattard98-beep/shadPS4/godzilla-rudp/tools/gz-relay/install.sh | sudo bash
set -euo pipefail

PORT="${RELAY_PORT:-3659}"
SRC="https://raw.githubusercontent.com/dattard98-beep/shadPS4/godzilla-rudp/tools/gz-relay/gz_relay.py"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo." >&2
    exit 1
fi
command -v python3 >/dev/null || { apt-get update -q && apt-get install -y -q python3; }

echo "Downloading relay..."
install -d /opt/gz-relay
curl -fsSL "$SRC" -o /opt/gz-relay/gz_relay.py
id gzrelay >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin gzrelay

cat > /etc/systemd/system/gz-relay.service <<UNIT
[Unit]
Description=shadPS4 P2P relay
After=network-online.target
Wants=network-online.target

[Service]
User=gzrelay
ExecStart=/usr/bin/python3 /opt/gz-relay/gz_relay.py --port ${PORT}
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
UNIT

echo "Opening UDP port ${PORT} in the VM firewall..."
if command -v iptables >/dev/null; then
    iptables -C INPUT -p udp --dport "${PORT}" -j ACCEPT 2>/dev/null ||
        iptables -I INPUT -p udp --dport "${PORT}" -j ACCEPT
    if command -v netfilter-persistent >/dev/null; then
        netfilter-persistent save >/dev/null 2>&1 || true
    fi
fi
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
    ufw allow "${PORT}/udp" >/dev/null
fi

systemctl daemon-reload
systemctl enable --now gz-relay >/dev/null
systemctl restart gz-relay
sleep 1

IP="$(curl -fsS -4 https://ifconfig.me 2>/dev/null || hostname -I | awk '{print $1}')"
if systemctl is-active --quiet gz-relay; then
    echo
    echo "Relay running on ${IP}:${PORT}"
    echo "In shadPS4 config.json, Network section:  \"relay_server\": \"${IP}:${PORT}\","
    echo "Also make sure UDP ${PORT} is allowed in the Oracle security list (ingress rule)."
    echo "Logs: sudo journalctl -u gz-relay -f"
else
    echo "Relay failed to start. Logs:" >&2
    journalctl -u gz-relay -n 30 --no-pager >&2
    exit 1
fi

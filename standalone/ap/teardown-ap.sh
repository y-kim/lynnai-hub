#!/bin/sh
# teardown-ap.sh -- AP 원복. SSH(wlan0) 복구용 안전 롤백.
sudo pkill hostapd 2>/dev/null || true
sudo pkill -f dnsmasq-uap0 2>/dev/null || true
sudo ip link set uap0 down 2>/dev/null || true
sudo iw dev uap0 del 2>/dev/null || true
sudo iptables -t nat -F POSTROUTING 2>/dev/null || true
sudo iptables -F FORWARD 2>/dev/null || true
echo "uap0 제거, wlan0만 남음."
ip -br addr | grep -E "wlan0|uap0" || true

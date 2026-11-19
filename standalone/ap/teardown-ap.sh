#!/bin/sh
# teardown-ap.sh -- AP 원복. SSH(wlan0) 복구용 안전 롤백.
sudo pkill hostapd 2>/dev/null || true
sudo pkill -f dnsmasq-uap0 2>/dev/null || true
sudo ip link set uap0 down 2>/dev/null || true
sudo iw dev uap0 del 2>/dev/null || true
# setup-ap.sh 가 안내한 규칙만 지운다(체인을 통째로 비우면 도커·방화벽 등 다른 규칙까지 날아감). 여러 번 넣었으면 다 지움
del_rule() { while sudo iptables "$@" 2>/dev/null; do :; done; }
del_rule -t nat -D POSTROUTING -o wlan0 -j MASQUERADE
del_rule -D FORWARD -i uap0 -o wlan0 -j ACCEPT
del_rule -D FORWARD -i wlan0 -o uap0 -m state --state RELATED,ESTABLISHED -j ACCEPT
echo "uap0 제거, wlan0만 남음."
ip -br addr | grep -E "wlan0|uap0" || true

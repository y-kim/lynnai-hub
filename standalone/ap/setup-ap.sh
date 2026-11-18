#!/bin/sh
# setup-ap.sh -- uap0 가상 AP 올리기 (wlan0 STA/SSH 유지). ★아직 실행하지 말 것★
# 이 스크립트는 "코드만" 단계의 참고용. 실제 실행은 사용자가 승인 후.
#
# 전제: iw, hostapd, dnsmasq 설치됨.
# 롤백: ap/teardown-ap.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "[1] 집 공유기 채널 확인:"
iw dev wlan0 link | grep -i freq || true

echo "[2] uap0 가상 AP 인터페이스 생성"
sudo iw dev wlan0 interface add uap0 type __ap
sudo ip addr add 192.168.50.1/24 dev uap0
sudo ip link set uap0 up

echo "[3] hostapd (AP) 가동"
sudo hostapd -B "$HERE/hostapd.conf"

echo "[4] dnsmasq (DHCP) 가동"
sudo dnsmasq -C "$HERE/dnsmasq-uap0.conf"

echo "[5] (standalone이 아니라 MITM이면) uap0→wlan0 NAT로 인터넷 중계:"
echo "    sudo sysctl -w net.ipv4.ip_forward=1"
echo "    sudo iptables -t nat -A POSTROUTING -o wlan0 -j MASQUERADE"
echo "    sudo iptables -A FORWARD -i uap0 -o wlan0 -j ACCEPT"
echo "    sudo iptables -A FORWARD -i wlan0 -o uap0 -m state --state RELATED,ESTABLISHED -j ACCEPT"

echo "완료. 이제 상위폴더에서: sudo python3 server_pi3.py --config config.json --gate-test"

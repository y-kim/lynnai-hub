#!/bin/sh
# install.sh -- 새 Raspberry Pi OS(bookworm, NetworkManager)에 LYNNAI 허브 설치. 여러 번 실행해도 됨.
#
#   sudo sh install.sh            설치 + 부팅 시 자동 시작(enable). 끝나면 재부팅하면 허브가 뜬다.
#
# 하는 일
#   1) hostapd / dnsmasq / iw / nftables 설치. 이 패키지들의 '시스템 서비스'는 끄고 막는다
#      (허브가 자기 설정으로 직접 띄움 — 기본 dnsmasq 가 53/67 을 먼저 잡으면 충돌).
#   2) 코드를 /opt/lynnai-hub 로 복사.
#   3) NetworkManager 가 AP 인터페이스(uap0)를 건드리지 않게 설정
#      (안 하면 uap0 를 만드는 순간 NM 이 가져가 AP 가 죽고 같은 칩의 wlan0 까지 흔들림).
#   4) systemd 유닛 등록 + enable.
# 상태(기기 ID·암호 등)는 /var/lib/lynnai/device.json — 첫 부팅 때 생성되고 공장초기화 때 지워진다.
set -eu
[ "$(id -u)" = 0 ] || exec sudo sh "$0" "$@"
SRC="$(cd "$(dirname "$0")" && pwd)"
DST=/opt/lynnai-hub

echo "[1/4] 패키지"
apt-get update -q
DEBIAN_FRONTEND=noninteractive apt-get install -y -q hostapd dnsmasq iw nftables python3
for s in hostapd dnsmasq; do
    systemctl disable --now "$s" >/dev/null 2>&1 || true
    systemctl mask "$s" >/dev/null 2>&1 || true
done

echo "[2/4] 코드 -> $DST"
if [ "$SRC" != "$DST" ]; then
    rm -rf "$DST.new"
    mkdir -p "$DST.new"
    (cd "$SRC" && tar cf - --exclude=.git --exclude=__pycache__ --exclude='*.pyc' .) | (cd "$DST.new" && tar xf -)
    rm -rf "$DST"
    mv "$DST.new" "$DST"
fi
chmod +x "$DST/appliance/main.py" "$DST/appliance/apctl.py"

echo "[3/4] NetworkManager 가 uap0 를 관리하지 않게"
mkdir -p /etc/NetworkManager/conf.d
printf '[keyfile]\nunmanaged-devices=interface-name:uap0\n' > /etc/NetworkManager/conf.d/lynnai-uap0.conf
chmod 600 /etc/NetworkManager/conf.d/lynnai-uap0.conf
nmcli general reload conf >/dev/null 2>&1 || true

echo "[4/4] systemd 서비스"
install -m 644 "$DST/appliance/lynnai-hub.service" /etc/systemd/system/lynnai-hub.service
systemctl daemon-reload
systemctl enable lynnai-hub >/dev/null

echo
echo "설치 완료. 재부팅하면 설정용 Wi-Fi(LYNNAI-SETUP-xxxxxx)가 뜹니다:  sudo reboot"
echo "상태 보기:  systemctl status lynnai-hub   /   journalctl -u lynnai-hub -f"

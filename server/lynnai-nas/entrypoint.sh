#!/bin/sh
# lynnai-nas 진입점: 인증서 없으면 만들고, 환경변수로 config.json 을 만들어 standalone 서버를 띄운다.
# 로직은 전부 레포의 standalone/ 코드 (server_pi3.py / broker.py / mqtt_proto.py / dns_hijack.py). 여기는 포장만.
set -e

DATA="${DATA_DIR:-/data}"
APP=/app/standalone
mkdir -p "$DATA/certs"

# 인증서(한 번만) — standalone/gencert.sh 와 같은 self-signed
if [ ! -f "$DATA/certs/server.crt" ]; then
  echo "자체 인증서 생성 (CN=rkiot.net)"
  openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$DATA/certs/server.key" -out "$DATA/certs/server.crt" \
    -days 3650 -sha256 -subj "/C=KR/O=rkiot/CN=rkiot.net" \
    -addext "subjectAltName=DNS:rkiot.net,DNS:*.rkiot.net,DNS:www.rkiot.net" 2>/dev/null
fi

# 환경변수 → config.json. NAS_IP 는 이 컨테이너의 LAN 주소(DNS 가 린나이 기기에게 답할 값).
#   ONLY_SRC : 등록된 린나이 기기 IP(쉼표). 비우면 모든 기기의 rkiot 질의를 가로챈다(권장: 채워 둘 것).
#   FORWARD  : 목록 밖 기기·다른 도메인을 넘길 상위 DNS(기본 공유기). "none" 이면 안 넘김(완전 독립).
: "${NAS_IP:?NAS_IP(이 컨테이너의 LAN 주소)를 지정하세요}"
HIJACK_JSON=$(printf '%s' "${HIJACK_NAMES:-rkiot.net}" | awk -F, '{for(i=1;i<=NF;i++){gsub(/^ +| +$/,"",$i);printf "%s\"%s\"",(i>1?",":""),$i}}')
SRC_JSON=$(printf '%s' "${ONLY_SRC:-}" | awk -F, '{for(i=1;i<=NF;i++){gsub(/^ +| +$/,"",$i);if($i!="")printf "%s\"%s\"",(c++?",":""),$i}}')
if [ "${FORWARD:-}" = "none" ]; then
  FWD_JSON=null
else
  FWD=${FORWARD:-$(ip route 2>/dev/null | awk '/default/{print $3; exit}')}
  FWD_JSON="[\"${FWD:-8.8.8.8}\", 53]"
fi

cat > "$DATA/config.json" <<EOF
{
  "bind_ip": "0.0.0.0",
  "mqtt_port": ${MQTT_PORT:-8883},
  "certfile": "$DATA/certs/server.crt",
  "keyfile": "$DATA/certs/server.key",
  "dns": {
    "enable": ${DNS_ENABLE:-true},
    "listen_ip": "0.0.0.0",
    "answer_ip": "$NAS_IP",
    "hijack": [${HIJACK_JSON}],
    "only_src": [${SRC_JSON}],
    "port": ${DNS_PORT:-53},
    "forward": ${FWD_JSON},
    "catch_all": false
  }
}
EOF

echo "== lynnai-nas 설정 (MODE=${MODE:-proxy}) =="
sed 's/^/  /' "$DATA/config.json"

# 공용 환경(두 모드 다 자체 인증서로 기기 TLS 를 받는다)
export NAS_CERT="$DATA/certs/server.crt" NAS_KEY="$DATA/certs/server.key"

term() { kill 0 2>/dev/null; }
trap term INT TERM

if [ "${MODE:-proxy}" = "proxy" ]; then
  # ESP32 허브와 동일(LAN 전용): DNS + /key/server(:80) + MQTT 중계(:8883) + LAN 통로(:8484).
  # 공용 코어 하나를 nas_hub.py 가 조립한다(standalone/ 의 dns_hijack 재사용). 린나이 앱 그대로.
  echo "== proxy 모드 (nas_hub) =="
  export PYTHONPATH="/app/standalone:/app/lynnai-nas"
  cd /app/lynnai-nas
  exec python3 -u nas_hub.py
else
  # standalone: 클라우드 없이 NAS 가 직접 브로커(server_pi3.py 가 DNS 까지 같이 돌린다)
  echo "== standalone 모드: DNS + 브로커(:${MQTT_PORT:-8883}) =="
  cd "$APP"
  exec python3 -u server_pi3.py --config "$DATA/config.json" ${GATE_TEST:+--gate-test}
fi

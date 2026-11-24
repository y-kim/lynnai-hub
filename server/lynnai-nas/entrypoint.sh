#!/bin/sh
# lynnai-nas 진입점: 인증서 없으면 만들고, 환경변수로 config.json(DNS 설정)을 만들어 nas_hub.py 를 띄운다.
set -e
umask 077                       # 여기서 만드는 키·설정·캐시 파일은 소유자만 읽게

DATA="${DATA_DIR:-/data}"
mkdir -p "$DATA/certs"

# 인증서(한 번만) — 기기 TLS 를 받을 self-signed
if [ ! -f "$DATA/certs/server.crt" ]; then
  echo "자체 인증서 생성 (CN=rkiot.net)"
  openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$DATA/certs/server.key" -out "$DATA/certs/server.crt" \
    -days 3650 -sha256 -subj "/C=KR/O=rkiot/CN=rkiot.net" \
    -addext "subjectAltName=DNS:rkiot.net,DNS:*.rkiot.net,DNS:www.rkiot.net" 2>/dev/null
fi

# 환경변수 → config.json. NAS_IP 는 이 컨테이너의 LAN 주소(DNS 가 린나이 기기에게 답할 값).
#   RINNAI_IPS : 고정 등록할 린나이 기기 IP(쉼표) — 선택. 비우면 고정 등록 없이 뜬다(후보를 /devices 에서 승인하거나
#                DEVICE_PAIR=1). any 면 모든 출발지를 기기로 받는다(경고). IPv4 가 아닌 항목이 있으면 nas_hub 가 시작을 거부한다.
#   ONLY_SRC   : DNS 가로채기 대상만 따로 줄 때. 비우면 등록된 기기 전부(RINNAI_IPS + 승인한 기기 — nas_hub 가 질의마다 판정).
#                모든 기기의 질의를 가로채는 것은 RINNAI_IPS=any 일 때만.
#   FORWARD    : 목록 밖 기기·다른 도메인을 넘길 상위 DNS(기본 공유기). "none" 이면 안 넘김(완전 독립).
: "${NAS_IP:?NAS_IP(이 컨테이너의 LAN 주소)를 지정하세요}"
SRC=$(printf '%s' "${ONLY_SRC:-}" | tr ';' ',')                 # 파이썬 쪽처럼 ';' 도 구분자로
if [ "$(printf '%s' "$SRC" | tr -d ' ' | tr 'A-Z' 'a-z')" = "any" ]; then
  SRC=""                        # 빈 목록 — nas_hub 는 등록된 기기만(RINNAI_IPS=any 일 때만 제한 없음)
fi
HIJACK_JSON=$(printf '%s' "${HIJACK_NAMES:-rkiot.net}" | awk -F, '{for(i=1;i<=NF;i++){gsub(/^ +| +$/,"",$i);printf "%s\"%s\"",(i>1?",":""),$i}}')
SRC_JSON=$(printf '%s' "$SRC" | awk -F, '{for(i=1;i<=NF;i++){gsub(/^ +| +$/,"",$i);if($i!="")printf "%s\"%s\"",(c++?",":""),$i}}')
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

# 두 모드 모두 nas_hub.py(ESP32 허브와 동일, LAN 전용): DNS + /key/server(:80) + MQTT(:8883) + LAN 통로(:8484).
#   MODE=proxy      린나이 클라우드로 중계(린나이 앱 그대로)
#   MODE=standalone 클라우드 없이 허브가 MQTT 상대(상태 요청·시간 응답을 허브가 대신)
echo "== ${MODE:-proxy} 모드 (nas_hub) =="
export MODE="${MODE:-proxy}" PYTHONPATH="/app/lynnai-nas"
cd /app/lynnai-nas
exec python3 -u nas_hub.py

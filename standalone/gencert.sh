#!/bin/sh
# self-signed 인증서 생성 (rkiot.net 사칭용). CN/SAN에 rkiot 도메인 박음.
# 핀닝 테스트: 보일러가 CA/핀 검증하면 이 인증서를 거부한다.
set -e
DIR="$(dirname "$0")/certs"
mkdir -p "$DIR"

openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "$DIR/server.key" -out "$DIR/server.crt" \
  -days 3650 -sha256 \
  -subj "/C=KR/O=rkiot/CN=rkiot.net" \
  -addext "subjectAltName=DNS:rkiot.net,DNS:*.rkiot.net,DNS:www.rkiot.net"

echo "생성됨: $DIR/server.crt , $DIR/server.key"
openssl x509 -in "$DIR/server.crt" -noout -subject -ext subjectAltName

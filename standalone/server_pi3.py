#!/usr/bin/env python3
"""
server_pi3.py  --  Pi3 글루: TLS 리스너 + MQTT 브로커 + DNS 하이재킹 (select 루프)

이 파일만 Pi3(CPython) 전용. 로직은 broker.py / mqtt_proto.py / dns_hijack.py.
Pico 포팅 시 이 파일의 ssl/소켓 부분만 MicroPython ssl로 갈아끼운다.

모드:
  --gate-test   : TLS 핸드셰이크 성사/거부만 본다 (cert pinning 판별).
                  보일러가 우리 self-signed를 거부하면 handshake 실패 로그.
  (기본)         : 브로커까지 돌려 CONNECT/SUBSCRIBE/PUBLISH를 받고 응답.

사용:
  python3 server_pi3.py --config config.json
  python3 server_pi3.py --config config.json --gate-test
"""

import argparse
import json
import select
import socket
import ssl
import sys
import time

import mqtt_proto as M
from broker import Broker
from dns_hijack import DnsHijack


def log(msg):
    print("%s %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def make_tls_context(certfile, keyfile):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=certfile, keyfile=keyfile)
    # 보일러가 우리를 어떻게 검증하든 우리는 클라 인증 요구 안 함
    ctx.verify_mode = ssl.CERT_NONE
    # IoT 모듈은 구버전 TLS일 수 있음 — 최대 호환
    try:
        ctx.minimum_version = ssl.TLSVersion.TLSv1
    except Exception:
        pass
    try:
        ctx.set_ciphers("ALL:@SECLEVEL=0")
    except Exception:
        pass
    return ctx


def on_connect(session, info):
    # rkiot 흉내: 접속 수락 후 필요한 "환영" 메시지가 있으면 여기서 publish_to.
    # 아직 프로토콜 미파악이라 비워둠. 관측 로그로 보일러가 뭘 기대하는지 본다.
    pass


def on_publish(session, msg):
    # 보일러가 올린 상태(J07)를 여기서 파싱/기록. 명령 내릴 땐 broker.publish_to.
    topic = msg["topic"]
    data = msg["payload"]
    # 원시 바이트 덤프(프로토콜 학습용)
    log("    payload hex: %s" % data[:64].hex())


def run(cfg, gate_test=False):
    host = cfg.get("bind_ip", "0.0.0.0")
    mqtt_port = cfg.get("mqtt_port", 8883)
    dns_cfg = cfg.get("dns")

    ctx = make_tls_context(cfg["certfile"], cfg["keyfile"])

    # --- MQTT 리스닝 소켓 ---
    lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind((host, mqtt_port))
    lsock.listen(8)
    lsock.setblocking(False)
    log("[mqtt] TLS listening %s:%d  (gate_test=%s)" % (host, mqtt_port, gate_test))

    broker = Broker(logger=log, on_connect=on_connect, on_publish=on_publish)

    # --- DNS(선택) ---
    dns = None
    if dns_cfg and dns_cfg.get("enable"):
        dns = DnsHijack(
            listen_ip=dns_cfg.get("listen_ip", "0.0.0.0"),
            answer_ip=dns_cfg["answer_ip"],
            hijack_names=dns_cfg.get("hijack", ["rkiot.net"]),
            logger=log,
            forward=tuple(dns_cfg["forward"]) if dns_cfg.get("forward") else None,
            catch_all=dns_cfg.get("catch_all", False),
            only_src=dns_cfg.get("only_src"),
        )
        dns.open(dns_cfg.get("port", 53))

    # fd -> state
    clients = {}        # rawsock.fileno() : {"sock","session","stage"}
    handshaking = {}    # fileno : sslsock  (논블로킹 핸드셰이크 진행 중)

    def close_client(fno, why=""):
        st = clients.pop(fno, None)
        handshaking.pop(fno, None)
        if st:
            if st.get("session"):
                broker.drop(st["session"])
            try:
                st["sock"].close()
            except Exception:
                pass
            log("[conn] closed %s %s" % (st.get("peer", fno), why))

    while True:
        rlist = [lsock]
        wlist = []
        if dns:
            rlist.append(dns.sock)
        for fno, st in clients.items():
            rlist.append(st["sock"])
            if st["session"] and st["session"].outbox:
                wlist.append(st["sock"])
        for fno, ssock in handshaking.items():
            # 핸드셰이크 중엔 읽기/쓰기 둘 다 관심
            rlist.append(ssock); wlist.append(ssock)

        rr, ww, _ = select.select(rlist, wlist, [], 1.0)

        # 새 연결
        if lsock in rr:
            try:
                raw, peer = lsock.accept()
                raw.setblocking(False)
                ssock = ctx.wrap_socket(raw, server_side=True,
                                        do_handshake_on_connect=False)
                handshaking[ssock.fileno()] = ssock
                clients[ssock.fileno()] = {"sock": ssock, "session": None,
                                           "peer": "%s:%d" % peer, "stage": "tls"}
                log("[conn] accept %s:%d  TLS 핸드셰이크 시작" % peer)
            except Exception as e:
                log("[conn] accept err: %s" % e)

        # DNS
        if dns and dns.sock in rr:
            try:
                dns.handle_once()
            except Exception as e:
                log("[dns] err: %s" % e)

        # 진행 중 핸드셰이크 밀기
        done = []
        for fno, ssock in list(handshaking.items()):
            if ssock in rr or ssock in ww:
                try:
                    ssock.do_handshake()
                    done.append(fno)
                    st = clients[fno]
                    st["stage"] = "mqtt"
                    log("[tls ] ✅ HANDSHAKE OK  %s  cipher=%s ver=%s" % (
                        st["peer"], ssock.cipher(), ssock.version()))
                    if gate_test:
                        log("    ➜ 핀닝 없음 추정: 보일러가 우리 self-signed 수락함")
                    st["session"] = broker.new_session(st["peer"])
                except ssl.SSLWantReadError:
                    pass
                except ssl.SSLWantWriteError:
                    pass
                except ssl.SSLError as e:
                    # 클라가 우리 인증서 거부 → cert pinning 강력 의심
                    log("[tls ] ❌ HANDSHAKE FAIL %s : %s" % (clients[fno]["peer"], e))
                    if gate_test:
                        log("    ➜ 핀닝/검증 의심: 이 경로(AP+DNS+프록시 MITM) 막힘. 버스직결로.")
                    done.append(fno)
                    close_client(fno, "tls-fail")
                except Exception as e:
                    log("[tls ] err %s : %s" % (clients[fno]["peer"], e))
                    done.append(fno); close_client(fno, "tls-err")
        for fno in done:
            handshaking.pop(fno, None)

        # 데이터 읽기
        for fno, st in list(clients.items()):
            if st["stage"] != "mqtt" or st["session"] is None:
                continue
            ssock = st["sock"]
            if ssock in rr:
                try:
                    data = ssock.recv(4096)
                    if not data:
                        close_client(fno, "eof"); continue
                    st["session"].feed(data)
                except ssl.SSLWantReadError:
                    pass
                except Exception as e:
                    close_client(fno, "recv:%s" % e); continue

        # 데이터 쓰기
        for fno, st in list(clients.items()):
            sess = st.get("session")
            if sess and sess.outbox:
                try:
                    out = sess.drain()
                    st["sock"].sendall(out)
                except ssl.SSLWantWriteError:
                    pass
                except Exception as e:
                    close_client(fno, "send:%s" % e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="config.json")
    ap.add_argument("--gate-test", action="store_true",
                    help="TLS 핸드셰이크 성사/거부만 확인 (cert pinning 판별)")
    args = ap.parse_args()
    with open(args.config) as f:
        cfg = json.load(f)
    try:
        run(cfg, gate_test=args.gate_test)
    except KeyboardInterrupt:
        log("bye")
        sys.exit(0)


if __name__ == "__main__":
    main()

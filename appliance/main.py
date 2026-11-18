#!/usr/bin/env python3
"""main.py -- LYNNAI 허브 기동 진입점 (systemd: lynnai-hub.service).

  1) 첫 부팅 개별화: 상태 파일이 없으면 CPU 시리얼 + HW RNG 로 기기별 ID·비밀 생성
  2) apctl up      : 상태대로 AP + DHCP/DNS (미설정 = 개방 설정 AP + 캡티브)
  3) 설정 포털     : 허브 IP :80. 설정/AP 변경 시 apctl 을 훅으로 불러 AP 를 다시 적용

    sudo python3 main.py [--state /var/lib/lynnai/device.json]
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, "portal")]
import apctl   # noqa: E402
import portal  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--state", default=apctl.DEFAULT_STATE)
    ap.add_argument("--port", type=int, default=80)
    a = ap.parse_args()
    st = apctl.load_state(a.state)
    print("[hub] 기기 %s · %s" % (st.data["device_id"],
                                 "설정 완료" if st.is_provisioned() else "미설정(설정 AP)"), flush=True)
    apctl.up(st)
    os.environ["LYNNAI_STATE"] = os.path.abspath(a.state)     # 훅(apctl)이 같은 상태 파일을 읽도록
    portal.serve(apctl.AP_IP, a.port, a.state, apply_cmd=os.path.join(HERE, "apctl.py"))


if __name__ == "__main__":
    main()

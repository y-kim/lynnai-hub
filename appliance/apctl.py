#!/usr/bin/env python3
"""apctl.py -- 허브의 Wi-Fi AP(hostapd) + DHCP/DNS(dnsmasq) 를 device.json 상태대로 올린다.

  미프로비저닝 : 설정용 AP  setup_ssid (개방; setup_pass 가 있으면 WPA2)
                 + 캡티브 DNS (모든 이름 → 허브) → 접속하면 설정 페이지가 자동으로 뜬다
  프로비저닝됨 : 전용 AP    ap_ssid / ap_pass (WPA2)
                 + 린나이 클라우드 이름만 허브로 (standalone: 기기가 허브 브로커에 붙음)

  apctl.py up   [--state P]   상태대로 (재)적용. 포털의 on_apply_ap 훅으로도 쓰인다('apctl.py up' — SSID/암호는
                              인자로 받지 않고 상태 파일을 다시 읽는다. 인자로 받으면 'down'·'--state=…' 가 옵션이 됨)
  apctl.py down [--state P]   AP/DNS 내림

root 로 실행. 채널은 wlan0 가 붙은 공유기 채널을 따른다(brcmfmac: AP+STA 단일 채널 제약).
생성되는 hostapd.conf / dnsmasq.conf 는 상태 파일 옆(0600)에 둔다.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [os.path.join(HERE, "core"), os.path.join(HERE, "config")]
import defaults as cfgmod  # noqa: E402
import state as state_mod  # noqa: E402

AP_IF = "uap0"
STA_IF = "wlan0"
AP_IP = "192.168.50.1"
CLOUD_NAMES = ("rkiot.net", "ckpush.com")      # standalone: 기기가 찾는 클라우드 이름 → 허브
DEFAULT_STATE = "/var/lib/lynnai/device.json"


def sh(*args, check=False):
    return subprocess.run(args, check=check, capture_output=True, text=True)


def channel():
    for line in sh("iw", "dev", STA_IF, "info").stdout.splitlines():
        p = line.split()
        if p and p[0] == "channel":
            return p[1]
    return "11"


NM_CONF = "/etc/NetworkManager/conf.d/lynnai-uap0.conf"


def nm_unmanage():
    """NetworkManager 가 uap0 를 '자기 장치'로 가져가지 않게 한다(생성되는 순간부터).
    안 하면 NM 이 새 uap0 를 건드려 AP 가 내려가고, 같은 칩을 쓰는 wlan0(집 Wi-Fi/SSH)까지 흔들린다."""
    if not shutil.which("nmcli"):
        return
    want = "[keyfile]\nunmanaged-devices=interface-name:%s\n" % AP_IF
    try:
        with open(NM_CONF) as f:
            cur = f.read()
    except OSError:
        cur = ""
    if cur != want:
        write(NM_CONF, want)
        sh("nmcli", "general", "reload", "conf")


def ensure_if():
    """uap0 가상 AP 인터페이스가 없으면 만들고 허브 IP 를 붙인다(실패하면 재시도 없이 종료)."""
    nm_unmanage()
    if sh("ip", "link", "show", AP_IF).returncode != 0:
        r = sh("iw", "dev", STA_IF, "interface", "add", AP_IF, "type", "__ap")
        if r.returncode != 0:
            sys.exit("apctl: %s 생성 실패: %s" % (AP_IF, (r.stderr or r.stdout).strip()))
        for _ in range(50):                     # 커널이 장치를 올릴 때까지 최대 5초
            if sh("ip", "link", "show", AP_IF).returncode == 0:
                break
            time.sleep(0.1)
        else:
            sys.exit("apctl: %s 가 생기지 않음" % AP_IF)
    if shutil.which("nmcli"):
        sh("nmcli", "device", "set", AP_IF, "managed", "no")
    if AP_IP + "/" not in sh("ip", "-4", "addr", "show", AP_IF).stdout:
        sh("ip", "addr", "add", AP_IP + "/24", "dev", AP_IF)
    sh("ip", "link", "set", AP_IF, "up")


def write(path, text):
    """원자적 교체 + 0600 (WPA 암호가 들어 있다)."""
    old = os.umask(0o077)
    try:
        with open(path + ".tmp", "w") as f:
            f.write(text)
        os.replace(path + ".tmp", path)
    finally:
        os.umask(old)


def hostapd_conf(ssid, pw, ch):
    s = ("interface=%s\ndriver=nl80211\nssid=%s\nhw_mode=g\nchannel=%s\n"
         "auth_algs=1\nignore_broadcast_ssid=0\n" % (AP_IF, ssid, ch))
    if pw:
        s += "wpa=2\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\nwpa_passphrase=%s\n" % pw
    return s


def dnsmasq_conf(captive):
    s = ("interface=%s\nbind-interfaces\nexcept-interface=%s\nno-hosts\nno-resolv\n"
         "dhcp-range=192.168.50.50,192.168.50.150,255.255.255.0,12h\n"
         "dhcp-option=3,%s\ndhcp-option=6,%s\n" % (AP_IF, STA_IF, AP_IP, AP_IP))
    if captive:
        s += "address=/#/%s\n" % AP_IP
    else:
        s += "".join("address=/%s/%s\n" % (n, AP_IP) for n in CLOUD_NAMES)
    return s


NFT_TABLE = "lynnai"


def firewall(captive):
    """허브 AP 클라이언트 격리(공개 Wi-Fi 캡티브처럼 빠르게 감지되도록).
    - 포워딩은 즉시 거절: 허브 망엔 인터넷이 없다(설정망이 공짜 핫스팟이 되지도 않음).
      drop 이 아니라 reject 라서 폰의 바깥 연결이 타임아웃까지 매달리지 않는다.
    - 설정 모드면 443/853 도 즉시 RST: 폰의 HTTPS·Private DNS 확인이 곧바로 실패해야
      HTTP 프로브의 302 로 '로그인 필요'를 바로 판정한다(다른 서비스가 443 을 열어둬도)."""
    rules = ['table inet %s' % NFT_TABLE, 'delete table inet %s' % NFT_TABLE,
             'table inet %s {' % NFT_TABLE,
             '  chain forward { type filter hook forward priority 0; policy accept;',
             '    iifname "%s" reject' % AP_IF,
             '  }']
    if captive:
        rules += ['  chain input { type filter hook input priority 0; policy accept;',
                  '    iifname "%s" tcp dport { 443, 853 } reject with tcp reset' % AP_IF,
                  '  }']
    rules.append('}')
    r = subprocess.run(["nft", "-f", "-"], input="\n".join(rules) + "\n",
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("apctl: nft 실패(격리 없이 계속): %s" % r.stderr.strip(), flush=True)


def restart(prog, conf, args):
    sh("pkill", "-f", "%s.*%s" % (prog, conf))
    time.sleep(1)
    r = sh(prog, *args)
    if r.returncode != 0:
        sys.exit("apctl: %s 실패: %s" % (prog, (r.stderr or r.stdout).strip()))


def paths(st):
    d = os.path.dirname(os.path.abspath(st.path))
    return os.path.join(d, "hostapd.conf"), os.path.join(d, "dnsmasq.conf"), os.path.join(d, "dnsmasq.pid")


def up(st):
    d = st.data
    if st.is_provisioned():
        ssid, pw, captive = d["ap_ssid"], d["ap_pass"] or "", False
    else:
        ssid, pw, captive = d["setup_ssid"], d.get("setup_pass") or "", True
    # 설정 파일에 그대로 들어가므로 줄바꿈 등 제어문자는 거부(설정 주입 방지). 저장할 때와 같은 검증(state.py)
    try:
        state_mod.check_ssid(ssid)
        if pw:
            state_mod.check_ap_pass(pw)
    except ValueError as e:
        sys.exit("apctl: %s" % e)
    ch = channel()
    ensure_if()
    hc, dc, pid = paths(st)
    write(hc, hostapd_conf(ssid, pw, ch))
    write(dc, dnsmasq_conf(captive))
    firewall(captive)
    restart("hostapd", hc, ["-B", hc])
    restart("dnsmasq", dc, ["-C", dc, "--pid-file=" + pid])
    print("apctl: %s AP '%s' ch%s %s" % ("설정용" if captive else "전용", ssid, ch,
                                         "WPA2" if pw else "개방"), flush=True)


def down(st):
    hc, dc, _ = paths(st)
    sh("pkill", "-f", "hostapd.*" + hc)
    sh("pkill", "-f", "dnsmasq.*" + dc)
    sh("nft", "delete", "table", "inet", NFT_TABLE)
    print("apctl: AP/DNS/격리규칙 내림", flush=True)


def load_state(path):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    st = state_mod.DeviceState(path)
    st.first_boot(cfgmod.load())            # 없으면 개별화 생성, 있으면 로드
    return st


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", nargs="?", default="up", choices=("up", "down"), help="up(기본) | down")
    ap.add_argument("--state", default=os.environ.get("LYNNAI_STATE", DEFAULT_STATE))
    a = ap.parse_args()                      # 모르는 인자는 오류(옛 훅의 <ssid> <pass> 가 옵션으로 읽히지 않게)
    st = load_state(a.state)
    down(st) if a.cmd == "down" else up(st)


if __name__ == "__main__":
    main()

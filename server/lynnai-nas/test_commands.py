#!/usr/bin/env python3
"""명령 확인·재전송과 온수 예열 자동 끄기 시험(HubCore 만, 네트워크 없음).

    python3 test_commands.py

확인하는 것: 반영되면 다시 안 보냄 / 2.5초 안에 반영이 없으면 다시 보내고 3번에서 포기 / 무관한 보고는 답으로 안 침 /
이미 그 값이면 지켜보지 않음 / 허용 안 된 코드는 버림 / 자동 끄기 설정 저장·알림 / 켜진 지 정한 시간이 지나면 b6=0.
"""
import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hub_rinnai                                            # noqa: E402

BASE = "rinnai/RK/01/RK/f06/23/34:5F:45:A1:B2:C3"
F06 = "10110000003c035001700fff0000055000000000000000000000000000000000"   # 전원·난방·온수 켜짐, 예열 꺼짐
fails = 0


def check(ok, what):
    global fails
    print(("  OK   " if ok else "  FAIL ") + what)
    fails += 0 if ok else 1


def report(core, ln, hexblob):
    core.on_device_publish(ln, BASE + "/inf", b'{"rpc":[{"id":"70","data":"%s"}]}' % hexblob.encode())


def with_nib(hexblob, pos, v):
    return hexblob[:pos] + v + hexblob[pos + 1:]


def setup(tmp):
    sent, emitted = [], []
    core = hub_rinnai.HubCore(log=lambda *a: None, data_dir=tmp)
    core.add_sender(lambda c, d: emitted.append((c, d)))
    ln = core.link_open(lambda pkt: sent.append(pkt))
    core.on_device_subscribe(ln, BASE + "/set")
    report(core, ln, F06)
    return core, ln, sent, emitted


def wait(sec):
    end = time.monotonic() + sec
    while time.monotonic() < end:
        time.sleep(0.05)


def main():
    tmp = tempfile.mkdtemp(prefix="lynnai-nas-cmd-")
    hub_rinnai.PEND_WAIT_S = 0.5                     # 시험은 빨리

    print("[1] 반영되면 한 번만")
    core, ln, sent, _ = setup(tmp)
    core.command("f06@a1b2c3 b6=1")
    check(len(sent) == 1 and b'"b6","data":"1"' in sent[0], "명령을 한 번 보냄")
    report(core, ln, with_nib(F06, 5, "1"))
    wait(1.2)
    check(len(sent) == 1 and core.cmd_info()["pending"] == 0, "예열 칸이 1 이 되면 다시 안 보냄")

    print("[2] 답이 없으면 다시, 3번에서 포기 — 무관한 보고는 답으로 안 침")
    sent.clear()
    core.command("f06@a1b2c3 b6=0")
    wait(0.3)
    report(core, ln, with_nib(with_nib(F06, 5, "1"), 11, "d"))    # 다른 칸(온수 설정)만 바뀐 보고
    wait(1.6)
    info = core.cmd_info()
    check(len(sent) == 3, f"모두 3번 보냄({len(sent)}번)")
    check(info["failed"] == 1 and info["pending"] == 0, "포기하고 실패로 셈")

    print("[3] 두 번째에 반영")
    sent.clear()
    core.command("f06@a1b2c3 d1=37,df=0")
    wait(0.7)
    report(core, ln, with_nib(with_nib(F06, 10, "3"), 11, "7"))
    wait(1.0)
    check(len(sent) == 2 and core.cmd_info()["pending"] == 0, "다시 보낸 뒤 반영되면 멈춤")

    print("[4] 이미 그 값 / 허용 안 된 코드")
    sent.clear()
    before = core.cmd_info()["tracked"]
    core.command("f06@a1b2c3 80=1")
    check(len(sent) == 1 and core.cmd_info()["tracked"] == before, "이미 켜짐 → 보내기만 하고 지켜보지 않음")
    sent.clear()
    core.command("f06@a1b2c3 ff=1")
    check(not sent, "허용 안 된 코드는 버림")

    print("[5] 각방: 방 번호(a0)의 칸을 본다")
    sent.clear()
    core.command("f09@a1b2c3 a0=2,b1=1")
    check(not sent, "f09 연결이 없으면 버림")

    print("[6] 온수 예열 자동 끄기")
    _, _, _, emitted = setup(tmp)
    core2, ln2, sent2, emitted2 = setup(tmp)
    core2.command("hub preoff=30")
    check(emitted2[-1] == (0x0C, bytes([30, 0, 0])), "예열이 꺼져 있을 때 바꾸면 기본값: 0x0c [30, 모드 0, 꺼짐 0]")
    check(json.load(open(os.path.join(tmp, "boiler.json")))["preheat_off_min"] == 30, "boiler.json 에 저장")
    check(hub_rinnai.HubCore(log=lambda *a: None, data_dir=tmp).preheat_off == 30, "다시 시작해도 30")
    core2.command("hub preoff=99")
    check(core2.preheat_off == 30 and emitted2[-1] == (0x0C, bytes([30, 0, 0])), "없는 값은 무시하고 지금 값을 다시 알림")
    report(core2, ln2, with_nib(F06, 5, "1"))                        # 예열 켜짐 → 기본값 30분
    wait(0.4)
    check(emitted2[-1] == (0x0C, bytes([30, 0, 30])), "켜지면 바로 남은 분(30)을 알림")
    core2.command("hub preoff=5")
    check(core2.preheat_off == 30 and emitted2[-1] == (0x0C, bytes([30, 0, 5])), "켜진 동안 고르면 이번만 5분(기본값은 30 그대로)")
    key = (6, bytes.fromhex("a1b2c3"))
    core2.pre_off_at[key] -= 5 * 60 + 1                                # 5분이 지난 것처럼
    sent2.clear()
    wait(0.6)
    check(any(b'"b6","data":"0"' in p for p in sent2), "정한 시간이 지나면 b6=0 을 보냄")
    core2.command("hub preoff=0")
    check(emitted2[-1] == (0x0C, bytes([30, 0, 255])), "켜진 동안 무제한을 고르면 255")
    report(core2, ln2, F06)
    wait(0.4)
    check(emitted2[-1] == (0x0C, bytes([30, 0, 0])), "꺼지면 바로 0(꺼짐)을 알림")
    core2.command("refresh")
    check(any(c == 0x0C for c, _ in emitted2[-4:]), "새로고침 때도 설정을 알림")

    print("[7] 보일러 모드(BOILER_MODE)")
    core3 = hub_rinnai.HubCore(log=lambda *a: None, data_dir=tmp, boiler_mode="water")
    out3 = []
    core3.add_sender(lambda c, d: out3.append((c, d)))
    core3.command("refresh")
    check((0x0C, bytes([30, 1, 0])) in out3, "water → 0x0c 둘째 바이트 1")
    check(hub_rinnai.HubCore(log=lambda *a: None, boiler_mode="room").boiler_mode == 2, "room → 2")
    check(hub_rinnai.HubCore(log=lambda *a: None, boiler_mode="x").boiler_mode == 0, "모르는 값 → 정하지 않음(0)")

    print("[8] 각방 시간 난방 분(da) — 방 슬롯 6번째 바이트로 확인")
    core4 = hub_rinnai.HubCore(log=lambda *a: None, data_dir=tmp)
    sent4 = []
    ln4 = core4.link_open(lambda pkt: sent4.append(pkt))
    base9 = "rinnai/RK/01/RK/f09/03/34:5F:45:D4:E5:F6"
    core4.on_device_subscribe(ln4, base9 + "/set")
    rooms = bytearray(54)
    rooms[0], rooms[3], rooms[5] = 0x47, 20, 10                       # 방1 켜짐 + 시간 모드, 10분
    core4.on_device_publish(ln4, base9 + "/inf", b'{"rpc":[{"id":"70","data":"%s"}]}' % rooms.hex().encode())
    core4.command("f09@d4e5f6 a0=1,da=1e")
    check(len(sent4) == 1 and b'"da","data":"1e"' in sent4[0], "da 를 보냄(허용 코드)")
    check(core4.cmd_info()["pending"] == 1, "반영될 때까지 지켜봄")
    rooms[5] = 30
    core4.on_device_publish(ln4, base9 + "/inf", b'{"rpc":[{"id":"70","data":"%s"}]}' % rooms.hex().encode())
    check(core4.cmd_info()["pending"] == 0, "슬롯 6번째 바이트가 30 이 되면 끝")

    print(f"\n== {'실패 %d' % fails if fails else '모두 통과'} ==")
    sys.exit(1 if fails else 0)


main()

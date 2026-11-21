#!/usr/bin/env python3
"""SmartThings 상세 화면 카드 그림(docs/screen-*.svg)을 만든다. README 의 '스크린샷' 절에 쓴다.

실제 캡처가 아니라 앱 화면을 따라 그린 그림이다. 드라이버의 프로필·화면 구성(edge-driver/profiles, presentations)을
바꾸면 아래 SCREENS 를 같이 고치고 다시 돌린다:  python3 tools/make_card_svgs.py
"""
import math
import os

W = 360                      # 화면 너비
PAD = 14                     # 화면 안쪽 여백
CW = W - 2 * PAD             # 카드 너비

STYLE = """
    .screen { fill: #f1f1f4; stroke: #d5d8de; }
    .card   { fill: #fcfcfd; }
    .idle   { fill: #e4e4e7; }
    .on     { fill: #3f83f1; }
    .btn    { fill: #f4f4f6; }
    .btn-on { fill: #ffffff; }
    .rule   { stroke: #e3e5ea; stroke-width: 1; }
    .track  { stroke: #e2e3e7; stroke-width: 6; stroke-linecap: round; }
    .heat   { stroke: #dc3b36; stroke-width: 6; stroke-linecap: round; }
    .cool   { stroke: #3f83f1; stroke-width: 6; stroke-linecap: round; }
    .knob-h { fill: #dc3b36; }
    .knob-c { fill: #3f83f1; }
    .dim    { opacity: .35; }
    .ink    { fill: #14161a; }
    .muted  { fill: #6b7078; }
    .icon   { fill: none; stroke: #14161a; stroke-width: 2; stroke-linecap: round; }
    .icon-m { fill: #6b7078; }
    text    { font-family: system-ui, -apple-system, "Segoe UI", "Apple SD Gothic Neo", "Malgun Gothic", "Noto Sans KR", sans-serif; fill: #14161a; }
    .t-title { font-size: 17px; font-weight: 700; }
    .t-sub   { font-size: 12.5px; fill: #6b7078; }
    .t-sec   { font-size: 13px; fill: #6b7078; }
    .t-name  { font-size: 14.5px; }
    .t-val   { font-size: 22px; font-weight: 700; }
    .t-unit  { font-size: 14px; font-weight: 600; }
    .t-pill  { font-size: 21px; font-weight: 700; }
    .t-on    { fill: #ffffff; }
    .t-pm    { font-size: 15px; font-weight: 700; fill: #ffffff; text-anchor: middle; }
    .t-col   { font-size: 11.5px; text-anchor: middle; }
    .t-cval  { font-size: 19px; font-weight: 700; text-anchor: middle; }
    .t-cunit { font-size: 12px; font-weight: 600; }
    .g1 { stop-color: #ebe8fb; } .g2 { stop-color: #cfe1f8; } .g3 { stop-color: #dcdbee; }
    .edge    { fill: none; stroke: #d5d8de; }
    .ic-box  { fill: #3d4046; }
    .ic-yel  { fill: #f5c518; }
    .ic-off  { fill: #b9bdc6; }
    .ic-grn  { fill: none; stroke: #86d993; stroke-width: 1.800; stroke-linecap: round; }
    .t-dev   { font-size: 14px; font-weight: 700; }
    .t-st    { font-size: 12.5px; fill: #6b7078; }
    .t-tab   { font-size: 14.5px; fill: #6b7078; }
    .t-tab-on { font-size: 14.5px; font-weight: 700; }
    .off-card { opacity: .55; }
    @media (prefers-color-scheme: dark) {
      .screen { fill: #000000; stroke: #33363c; }
      .card   { fill: #1c1d21; }
      .idle   { fill: #2a2b30; }
      .on     { fill: #3a78dc; }
      .btn    { fill: #3a3c42; }
      .rule   { stroke: #2c2e33; }
      .track  { stroke: #3a3d44; }
      .heat   { stroke: #e0524d; }
      .cool   { stroke: #3a78dc; }
      .knob-h { fill: #e0524d; }
      .knob-c { fill: #3a78dc; }
      .icon   { stroke: #f2f3f5; }
      .icon-m { fill: #9aa0a8; }
      text    { fill: #f2f3f5; }
      .t-sub, .t-sec, .t-st, .t-tab { fill: #9aa0a8; }
      .g1 { stop-color: #1b1a2b; } .g2 { stop-color: #14202e; } .g3 { stop-color: #1a1a24; }
      .edge   { stroke: #33363c; }
      .ic-box { fill: #4a4d55; }
    }
"""


class Screen:
    def __init__(self, title, subtitle):
        self.o = []
        self.y = 26
        self.o.append(f'<text class="t-title" x="{PAD + 34}" y="{self.y + 8}">{title}</text>')
        self.o.append(f'<text class="t-sub" x="{PAD + 34}" y="{self.y + 26}">{subtitle}</text>')
        self.o.append(f'<path class="icon" d="M{PAD + 14} {self.y + 4} l-7 8 l7 8"/>')
        self.y += 44

    def section(self, name):
        self.y += 8
        self.o.append(f'<text class="t-sec" x="{PAD + 8}" y="{self.y + 10}">{name}</text>')
        x = W - PAD - 20                    # 연필 아이콘
        self.o.append(f'<path class="icon-m" transform="translate({x} {self.y - 2})" '
                      'd="M0 11.500V14h2.500l7.400-7.400-2.500-2.500L0 11.500Zm11.800-6.800a.660.660 0 0 0 0-.900L10.200 2.200a.660.660 0 0 0-.900 0L8.100 3.400l2.500 2.500 1.200-1.200Z"/>')
        self.y += 22

    def pill(self, on):
        h = 56
        self.o.append(f'<rect class="{"on" if on else "idle"}" x="{PAD}" y="{self.y}" width="{CW}" height="{h}" rx="28"/>')
        self.o.append(f'<text class="t-pill{" t-on" if on else ""}" x="{PAD + 22}" y="{self.y + 35}">{"켜짐" if on else "꺼짐"}</text>')
        cx, cy = W - PAD - 30, self.y + 28
        self.o.append(f'<circle class="{"btn-on" if on else "btn"}" cx="{cx}" cy="{cy}" r="19"/>')
        icon_style = ' style="stroke:#14161a"' if on else ''
        self.o.append(f'<path class="icon"{icon_style} d="M{cx} {cy - 8}v8M{cx - 4.800} {cy - 5.200}a7 7 0 1 0 9.600 0"/>')
        self.y += h + 10

    def tile(self, rows, blue=False):
        """rows: (종류, 제목, 값, 단위, 비율) — 종류: list | value | meter | plain | heat | cool | cols
        cols 는 값 자리에 [(칸 제목, 값, 단위), …] 를 준다. blue=True 면 파란 카드(구역의 요약 카드)."""
        heights = {"list": 74, "value": 74, "meter": 74, "plain": 74, "heat": 108, "cool": 78, "cols": 100}
        h = sum(heights[r[0]] for r in rows)
        self.o.append(f'<rect class="{"on" if blue else "card"}" x="{PAD}" y="{self.y}" width="{CW}" height="{h}" rx="22"/>')
        on = " t-on" if blue else ""
        y = self.y
        x0, x1 = PAD + 20, W - PAD - 20
        for i, (kind, name, val, unit, frac) in enumerate(rows):
            if i:
                self.o.append(f'<path class="rule" d="M{x0} {y}H{x1}"/>')
            self.o.append(f'<text class="t-name{on}" x="{x0}" y="{y + 27}">{name}</text>')
            if kind == "cols":
                n = len(val)
                for c, (cname, cval, cunit) in enumerate(val):
                    cx = x0 + (x1 - x0) * (c + 0.500) / n
                    self.o.append(f'<text class="t-col{on}" x="{cx:.0f}" y="{y + 52}">{cname}</text>')
                    self.o.append(f'<text class="t-cval{on}" x="{cx:.0f}" y="{y + 80}">{cval}<tspan class="t-cunit" dx="3">{cunit}</tspan></text>')
            elif kind in ("meter", "plain"):
                self.o.append(_value(x0, y + 57, val, unit))
                if kind == "meter":         # 표준 계량기 카드: 설정(톱니)과 그래프 아이콘
                    self.o.append(f'<g class="icon-m" transform="translate({x1 - 18} {y + 14})"><rect x="0" y="6" width="3" height="9" rx="1"/>'
                                  '<rect x="5" y="0" width="3" height="15" rx="1"/><rect x="10" y="4" width="3" height="11" rx="1"/>'
                                  '<rect x="15" y="7" width="3" height="8" rx="1"/></g>')
                    self.o.append(f'<path class="icon-m" fill-rule="evenodd" d="{_gear(x1 - 44, y + 21, 0.800)}"/>')
            elif kind == "list":
                self.o.append(f'<text class="t-val" x="{x0}" y="{y + 57}">{val}</text>')
                vx = x0 + 22 * len(val) + 10
                self.o.append(f'<path class="ink" d="M{vx} {y + 45}h9l-4.500 6z"/>')
            elif kind == "value":
                self.o.append(_value(x0, y + 57, val, unit))
                self.o.append(f'<g class="icon-m" transform="translate({x1 - 18} {y + 14})"><rect x="0" y="6" width="3" height="9" rx="1"/>'
                              '<rect x="5" y="0" width="3" height="15" rx="1"/><rect x="10" y="4" width="3" height="11" rx="1"/>'
                              '<rect x="15" y="7" width="3" height="8" rx="1"/></g>')
            elif kind == "heat":        # 표준 온도 설정: 숫자 한 줄, 그 아래 −/+ 와 빨간 슬라이더
                self.o.append(_value(x0, y + 57, val, unit))
                sy = y + 84
                a, b = x0 + 30, x1 - 30
                k = a + (b - a) * frac
                self.o.append(f'<circle class="knob-h" cx="{x0 + 10}" cy="{sy}" r="10"/><text class="t-pm" x="{x0 + 10}" y="{sy + 5}">−</text>')
                self.o.append(f'<path class="track" d="M{a} {sy}H{b}"/><path class="heat" d="M{a} {sy}H{k:.0f}"/><circle class="knob-h" cx="{k:.0f}" cy="{sy}" r="12"/>')
                dim = ' dim' if frac >= 1 else ''
                self.o.append(f'<g class="{dim.strip()}"><circle class="knob-h" cx="{x1 - 10}" cy="{sy}" r="10"/><text class="t-pm" x="{x1 - 10}" y="{sy + 5}">+</text></g>')
            elif kind == "cool":        # 우리가 만든 슬라이더: 숫자 옆에 파란 슬라이더
                self.o.append(_value(x0, y + 59, val, unit))
                a, b = x0 + 96, x1 - 8
                k = a + (b - a) * frac
                sy = y + 51
                self.o.append(f'<path class="track" d="M{a} {sy}H{b}"/><path class="cool" d="M{a} {sy}H{k:.0f}"/><circle class="knob-c" cx="{k:.0f}" cy="{sy}" r="12"/>')
            y += heights[kind]
        self.y += h + 10

    def svg(self, label):
        height = self.y + 12
        body = "\n  ".join(self.o)
        return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {height}" width="{W}" role="img" aria-label="{label}">\n'
                f'  <title>{label}</title>\n  <style>{STYLE}  </style>\n'
                f'  <rect class="screen" x="0.5" y="0.5" width="{W - 1}" height="{height - 1}" rx="26"/>\n  {body}\n</svg>\n')


def _gear(cx, cy, s=1.0, teeth=8, R=9.500, r=7.200, hole=3.300, w_root=14, w_tip=8.500):
    """톱니바퀴(설정) 아이콘의 path. 가운데 구멍은 fill-rule=evenodd 로 뚫는다."""
    pts = []
    for k in range(teeth):
        th = k * 360 / teeth
        for ang, rad in ((th - w_root, r), (th - w_tip, R), (th + w_tip, R), (th + w_root, r)):
            a = math.radians(ang - 90)
            pts.append((cx + s * rad * math.cos(a), cy + s * rad * math.sin(a)))
    d = "M" + " L".join("%.2f %.2f" % p for p in pts) + "Z"
    h = s * hole
    return d + " M%.2f %.2f a%.2f %.2f 0 1 0 %.2f 0 a%.2f %.2f 0 1 0 %.2f 0Z" % (cx + h, cy, h, h, -2 * h, h, h, 2 * h)


def _value(x, y, val, unit):
    w = sum(12.500 if c.isdigit() else 6 for c in val)        # 숫자 폭 어림
    return (f'<text class="t-val" x="{x}" y="{y}">{val}</text>'
            f'<text class="t-unit" x="{x + w + 6:.0f}" y="{y}">{unit}</text>')


def boiler():
    s = Screen("보일러 컨트롤러", "집")
    s.section("전원"); s.pill(True)
    s.section("난방"); s.pill(True)
    s.tile([("list", "온도조절 모드", "온돌", "", 0), ("heat", "난방 온도 설정", "70.0", "°C", 0.700)])
    s.section("외출"); s.pill(False)
    s.section("온수"); s.pill(True)
    s.tile([("heat", "수온 설정", "60.0", "°C", 1.0)])
    s.section("온수 예열"); s.pill(False)
    return s.svg("보일러 컨트롤러의 SmartThings 화면 그림: 전원 켜짐. 난방 켜짐, 온도조절 모드 온돌, 난방 온도 설정 70도. "
                 "외출 꺼짐. 온수 켜짐, 수온 설정 60도. 온수 예열 꺼짐")


def thermostat():
    s = Screen("각방 온도조절기", "집 - 거실")
    s.section("난방"); s.pill(True)
    s.tile([("value", "실내 온도", "21.0", "°C", 0)])
    s.tile([("heat", "난방 온도 설정", "23.0", "°C", 0.650)])
    s.section("외출"); s.pill(False)
    s.section("예약"); s.pill(False)
    s.tile([("cool", "예약 시간", "5", "시간", 0.170)])
    return s.svg("각방 온도조절기의 SmartThings 화면 그림: 난방 켜짐, 실내 온도 21도, 난방 온도 설정 23도. 외출 꺼짐. "
                 "예약 꺼짐, 예약 시간 5시간")


def gas():
    s = Screen("가스 계량기", "집")
    s.y += 8
    s.tile([("cols", "사용량", [("24시간 사용량", "1.54", "m³"), ("1시간 사용량", "1.13", "m³"), ("이번달 사용량", "5.19", "m³")], "", 0)], blue=True)
    s.tile([("meter", "계량기 지침", "1523.4", "m³", 0),
            ("plain", "유량", "0.6", "m³/h", 0),
            ("cols", "요금", [("이번달 예상 금액", "6840", "원"), ("지난 24시간 요금", "1622", "원")], "", 0)])
    return s.svg("가스 계량기의 SmartThings 화면 그림: 사용량 카드에 24시간·1시간·이번달 사용량. 계량기 지침, 유량, "
                 "그리고 요금 카드에 이번달 예상 금액과 지난 24시간 요금")


def _dev_icon(kind, x, y, off=False):
    """기기 카드 오른쪽의 아이콘(어두운 네모 안의 그림). x, y = 네모의 왼쪽 위."""
    o = [] if kind == "station" else [f'<rect class="ic-box" x="{x}" y="{y}" width="26" height="26" rx="7"/>']
    c = "ic-off" if off else "ic-yel"
    if kind == "thermo":
        o.append(f'<rect class="{c}" x="{x + 11.500}" y="{y + 5}" width="3" height="11" rx="1.500"/><circle class="{c}" cx="{x + 13}" cy="{y + 18}" r="3.500"/>')
    elif kind == "flame":
        o.append(f'<path class="{c}" transform="translate({x + 1} {y + 1})" d="M12.500 3.500c.600 3.200 5 4.700 5 9.300a5.500 5.500 0 0 1-11 0c0-2 .900-3.300 2-4.600.300 1.300 1 2.100 1.900 2.300-.300-2.700.500-5.200 2.100-7Z"/>')
    elif kind == "temphum":
        o.append(f'<rect class="{c}" x="{x + 6.500}" y="{y + 5}" width="3" height="11" rx="1.500"/><circle class="{c}" cx="{x + 8}" cy="{y + 18}" r="3.500"/>')
        for dy in (5, 14):
            o.append(f'<path class="{c}" d="M{x + 18} {y + dy}c1.500 2.200 2.800 3.500 2.800 5a2.800 2.800 0 0 1-5.600 0c0-1.500 1.300-2.800 2.800-5Z"/>')
    elif kind == "station":            # SmartThings Station(허브): 어두운 바탕에 초록 테두리 네모 + 오른쪽 아래 작은 허브 표시
        o.append(f'<rect class="ic-box" x="{x + 2}" y="{y + 2}" width="19" height="19" rx="5.500"/>'
                 f'<rect class="ic-grn" x="{x + 2}" y="{y + 2}" width="19" height="19" rx="5.500"/><circle class="ic-grn" style="fill:#86d993" cx="{x + 16.500}" cy="{y + 6.500}" r="1"/>')
        bx, by = x + 16, y + 16
        o.append(f'<rect class="ic-box" x="{bx}" y="{by}" width="12" height="12" rx="3.500"/>'
                 f'<circle cx="{bx + 6}" cy="{by + 6}" r="1.400" fill="#ffffff"/>'
                 + "".join(f'<circle cx="{bx + 6 + dx}" cy="{by + 6 + dy}" r="1" fill="#ffffff"/>' for dx, dy in ((-3, -3), (3, -3), (-3, 3), (3, 3))))
    elif kind == "hub":
        cx, cy = x + 13, y + 13
        o.append(f'<circle class="ic-grn" cx="{cx}" cy="{cy}" r="2.500"/>')
        for dx, dy in ((-6, -6), (6, -6), (-6, 6), (6, 6)):
            o.append(f'<circle class="ic-grn" cx="{cx + dx}" cy="{cy + dy}" r="2"/>'
                     f'<path class="ic-grn" d="M{cx + dx * 0.300} {cy + dy * 0.300}L{cx + dx * 0.700} {cy + dy * 0.700}"/>')
    return "".join(o)


def devices():
    """SmartThings 의 '기기' 목록 화면: 방마다 기기 카드(이름 · 요약 글자 · 아이콘)."""
    rooms = [
        ("거실", [("거실", "23°C · 예약 5시간", "thermo", False), ("SmartThings Station", "충전 대기 중", "station", False)]),
        ("안방", [("안방", "24°C · 난방 25°C", "thermo", False)]),
        ("서재", [("서재", "18°C · 꺼짐", "thermo", True)]),
        ("다용도실", [("LYNNAI Hub", "연결됨", "hub", False), ("보일러 컨트롤러", "온돌 75°C · 온수 60°C", "flame", False),
                    ("가스 계량기", "1D 0.78m³ · 1H 0.52m³", "flame", False), ("온습도계", "21.6 °C  42 %", "temphum", False)]),
    ]
    o, clips = [], []
    # 맨 위: 장소 고르기 · 더하기/메뉴
    hx = PAD + 12
    o.append("".join(f'<circle class="icon" style="stroke-width:1.600" cx="{hx + dx}" cy="{26 + dy}" r="2.200"/>' for dx, dy in ((0, 0), (10, 0), (0, 10))))
    o.append(f'<path class="ink" d="M{hx + 5} 44v-7l7.500-6.500 7.500 6.500v7Z"/>')
    o.append(f'<text class="t-title" x="{PAD + 46}" y="42">집</text><path class="ink" d="M{PAD + 72} 33h10l-5 6.500z"/>')
    rx = W - PAD - 76
    o.append(f'<path class="icon" d="M{rx + 12} 36h18M{rx + 21} 27v18"/>')
    o.append("".join(f'<circle class="ink" cx="{rx + 58}" cy="{28 + i * 8}" r="1.800"/>' for i in range(3)))
    # 방 고르기 줄
    o.append(f'<circle class="icon" cx="{PAD + 22}" cy="89" r="6.500"/><path class="icon" d="M{PAD + 27} 94l5 5"/>')
    o.append(f'<text class="t-tab-on" x="{PAD + 52}" y="95">모든 기기</text>')
    for i, name in enumerate(("거실", "안방", "서재", "다용도실")):
        o.append(f'<text class="t-tab" x="{PAD + 134 + i * 50}" y="95">{name}</text>')
    y = 126
    cw = (CW - 10) / 2
    n = 0
    for room, cards in rooms:
        o.append(f'<text class="t-sec" x="{PAD + 8}" y="{y + 12}">{room}</text>')
        o.append(f'<path class="icon" style="stroke-width:1.500" d="M{W - PAD - 24} {y + 12}l6-6 6 6"/>')
        y += 24
        for i, (name, status, icon, off) in enumerate(cards):
            x = PAD + (i % 2) * (cw + 10)
            cy = y + (i // 2) * 80
            n += 1
            clips.append(f'<clipPath id="c{n}"><rect x="{x}" y="{cy}" width="{cw - 52}" height="70"/></clipPath>')
            g = ' class="off-card"' if off else ''
            o.append(f'<g{g}><rect class="card" x="{x}" y="{cy}" width="{cw}" height="70" rx="30"/>'
                     f'<g clip-path="url(#c{n})"><text class="t-dev" x="{x + 16}" y="{cy + 31}">{name}</text>'
                     f'<text class="t-st" x="{x + 16}" y="{cy + 50}">{status}</text></g>'
                     f'{_dev_icon(icon, x + cw - 42, cy + 22, off)}</g>')
        y += ((len(cards) + 1) // 2) * 80 + 6
    height = int(y + 8)
    label = ("SmartThings 기기 목록 화면 그림: 거실·안방·서재에 각방 온도조절기(현재 온도와 난방·예약·꺼짐 상태), 거실에 SmartThings Station, "
             "다용도실에 LYNNAI Hub, 보일러 컨트롤러(온돌 75도 · 온수 60도), 가스 계량기(1D · 1H 사용량), 온습도계")
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {height}" width="{W}" role="img" aria-label="{label}">\n'
            f'  <title>{label}</title>\n  <style>{STYLE}  </style>\n'
            f'  <defs><linearGradient id="bg" x1="0" y1="0" x2="1" y2="1"><stop class="g1" offset="0"/><stop class="g2" offset=".55"/>'
            f'<stop class="g3" offset="1"/></linearGradient>{"".join(clips)}</defs>\n'
            f'  <rect fill="url(#bg)" x="0.5" y="0.5" width="{W - 1}" height="{height - 1}" rx="26"/>'
            f'<rect class="edge" x="0.5" y="0.5" width="{W - 1}" height="{height - 1}" rx="26"/>\n  ' + "\n  ".join(o) + '\n</svg>\n')


if __name__ == "__main__":
    docs = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs")
    for name, fn in (("screen-boiler.svg", boiler), ("screen-thermostat.svg", thermostat), ("screen-gas.svg", gas),
                     ("screen-devices.svg", devices)):
        with open(os.path.join(docs, name), "w", encoding="utf-8") as f:
            f.write(fn())
        print("wrote docs/" + name)

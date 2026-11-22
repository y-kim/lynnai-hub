#!/usr/bin/env python3
"""NAS 허브 구성도(docs/overview-nas.svg, 영어판 overview-nas.en.svg)를 만든다.

배치: 왼쪽 린나이 기기, 가운데 위 스마트폰 · 공유기 · 그 아래 NAS(DNS 서버 + LYNNAI 허브), 오른쪽 인터넷 · SmartThings.
실선 = 물리 연결, 색 점선 = 트래픽(꺾이는 곳은 둥글게). 좌표를 고치면 이 파일을 다시 돌린다:
    python3 tools/make_overview_nas_svg.py
"""
import os

T = {
    "ko": dict(
        aria="NAS 허브 구성도: 린나이 기기(보일러 제어기·각방 온도 조절기)와 스마트폰은 공유기에 붙고, 공유기의 DNS 설정은 NAS 를 가리킨다. NAS 안에 DNS 서버와 LYNNAI 허브가 Docker 로 돈다. 린나이 기기가 묻는 rkiot.net 은 공유기의 DNS 를 거쳐 NAS 의 DNS 서버로 가고, DNS 서버가 NAS 주소로 답해 LYNNAI 허브로 간다. 스마트폰이 묻는 rkiot.net 과 google.com 은 같은 DNS 서버를 거치지만 진짜 주소를 받아 공유기를 통해 인터넷으로 나간다. LYNNAI 허브는 프록시 모드에서 공유기를 통해 린나이 클라우드로 이어지고, 공유기는 LAN 으로 SmartThings 허브와 앱에 이어진다",
        title="lynnai-hub 구성도 (NAS 허브)",
        rinnai="린나이 기기", boiler="보일러 제어기", boiler_s="WF-S6000 등", thermo="각방 온도 조절기", thermo_s="RICH-WCR(N) 등",
        phone="스마트폰", phone_s="린나이 앱 · 웹", wifi="집 Wi-Fi", router="공유기", dns="DNS", dns_s="→ NAS",
        nas="NAS", wired="유선 LAN", nasdns="DNS 서버", nasdns_s="린나이 기기만 NAS 로", hub="LYNNAI 허브",
        hub_s="클라우드 대신 받음", proxy="프록시 모드", internet="인터넷", cloud="린나이 클라우드", lan="LAN",
        st_hub="SmartThings 허브", st_app="SmartThings 앱",
        lg_a="린나이 기기의 rkiot.net → LYNNAI 허브", lg_b="스마트폰의 rkiot.net → 린나이 클라우드", lg_c="스마트폰의 google.com → 인터넷"),
    "en": dict(
        aria="NAS hub overview: Rinnai devices (boiler controller, room thermostat) and a smartphone connect to the router, whose DNS setting points to the NAS. The NAS runs a DNS server and the LYNNAI hub in Docker. rkiot.net asked by Rinnai devices goes through the router's DNS to the DNS server on the NAS, which answers with the NAS address, so they reach the LYNNAI hub. rkiot.net and google.com asked by the smartphone go through the same DNS server but get the real address and leave through the router to the internet. In proxy mode the LYNNAI hub connects through the router to the Rinnai cloud, and the router links over the LAN to the SmartThings hub and app",
        title="lynnai-hub overview (NAS hub)",
        rinnai="Rinnai devices", boiler="Boiler controller", boiler_s="e.g. WF-S6000", thermo="Room thermostat", thermo_s="e.g. RICH-WCR(N)",
        phone="Smartphone", phone_s="Rinnai app · web", wifi="Home Wi-Fi", router="Router", dns="DNS", dns_s="→ NAS",
        nas="NAS", wired="Wired LAN", nasdns="DNS", nasdns_s="Rinnai devices → NAS", hub="LYNNAI hub",
        hub_s="stands in for the cloud", proxy="proxy mode", internet="Internet", cloud="Rinnai cloud", lan="LAN",
        st_hub="SmartThings hub", st_app="SmartThings app",
        lg_a="Rinnai devices: rkiot.net → LYNNAI hub", lg_b="Smartphone: rkiot.net → Rinnai cloud", lg_c="Smartphone: google.com → internet"),
}

STYLE = """
    .bg    { fill: #ffffff; stroke: #d0d7de; }
    .box   { fill: #f6f8fa; stroke: #57606a; stroke-width: 1.5; }
    .hub   { fill: #ddf4ff; stroke: #0969da; stroke-width: 2; }
    .dns   { fill: #ffffff; stroke: #57606a; stroke-width: 1.3; stroke-dasharray: 5 4; }
    .inner { fill: #ffffff; stroke: #57606a; stroke-width: 1.5; }
    .group { fill: none; stroke: #8c959f; stroke-width: 1.2; }
    .line  { fill: none; stroke: #57606a; stroke-width: 1.5; }
    .head  { fill: #57606a; }
    .bay   { fill: #d0d7de; }
    .led   { fill: #1a7f37; }
    .ta, .tb, .tc { fill: none; stroke-width: 2; stroke-dasharray: 7 4; stroke-linecap: round; }
    .ta    { stroke: #cf222e; } .ha { fill: #cf222e; }
    .tb    { stroke: #0969da; } .hb { fill: #0969da; }
    .tc    { stroke: #1a7f37; } .hc { fill: #1a7f37; }
    text   { font-family: system-ui, -apple-system, "Segoe UI", "Apple SD Gothic Neo", "Malgun Gothic", "Noto Sans KR", sans-serif;
             font-size: 14px; fill: #1f2328; text-anchor: middle; }
    .name  { font-weight: 600; }
    .sub   { font-size: 12px; fill: #57606a; }
    .via   { font-size: 12.5px; fill: #57606a; }
    .l     { text-anchor: start; }
    .r     { text-anchor: end; }
    .la    { font-size: 12px; fill: #cf222e; }
    .lb    { font-size: 12px; fill: #0969da; }
    .lc    { font-size: 12px; fill: #1a7f37; }
    @media (prefers-color-scheme: dark) {
      .bg   { fill: #0d1117; stroke: #30363d; }
      .box  { fill: #161b22; stroke: #8b949e; }
      .hub  { fill: #0c2d6b; stroke: #58a6ff; }
      .dns  { fill: #0d1117; stroke: #8b949e; }
      .inner { fill: #0d1117; stroke: #8b949e; }
      .group { stroke: #6e7681; }
      .line { stroke: #8b949e; }
      .head { fill: #8b949e; }
      .bay  { fill: #30363d; }
      .led  { fill: #3fb950; }
      .ta   { stroke: #ff7b72; } .ha { fill: #ff7b72; } .la { fill: #ff7b72; }
      .tb   { stroke: #58a6ff; } .hb { fill: #58a6ff; } .lb { fill: #58a6ff; }
      .tc   { stroke: #3fb950; } .hc { fill: #3fb950; } .lc { fill: #3fb950; }
      text  { fill: #e6edf3; }
      .sub, .via { fill: #8b949e; }
    }
"""


def rpath(pts, r=9):
    """가로·세로로만 가는 점들을 잇되 꺾이는 곳은 반지름 r 로 둥글게."""
    d = "M%g %g" % pts[0]
    for i in range(1, len(pts) - 1):
        (x0, y0), (x1, y1), (x2, y2) = pts[i - 1], pts[i], pts[i + 1]
        def toward(ax, ay, bx, by, k):
            n = max(abs(bx - ax), abs(by - ay))
            k = min(k, n / 2)
            return ax + (bx - ax) / n * k, ay + (by - ay) / n * k
        ax, ay = toward(x1, y1, x0, y0, r)
        bx, by = toward(x1, y1, x2, y2, r)
        d += " L%g %g Q%g %g %g %g" % (ax, ay, x1, y1, bx, by)
    d += " L%g %g" % pts[-1]
    return d


def build(t):
    o = []
    a = o.append
    def box(cls, x, y, w, h, rx=8): a('  <rect class="%s" x="%g" y="%g" width="%g" height="%g" rx="%g"/>' % (cls, x, y, w, h, rx))
    def text(cls, x, y, s, style=""): a('  <text class="%s" x="%g" y="%g"%s>%s</text>' % (cls, x, y, ' style="%s"' % style if style else "", s))
    def line(cls, pts, marker="arrow", start=False):
        a('  <path class="%s" d="%s"%s%s/>' % (cls, rpath(pts), ' marker-end="url(#%s)"' % marker if marker else "",
                                               ' marker-start="url(#arrow)"' if start else ""))

    a('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 960 506" width="960" role="img"\n     aria-label="%s">' % t["aria"])
    a('  <title>%s</title>\n  <style>%s  </style>' % (t["title"], STYLE))
    a('  <defs>')
    a('    <marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" orient="auto-start-reverse"><path class="head" d="M0 0 L10 5 L0 10 z"/></marker>')
    for k in "abc":
        a('    <marker id="a%s" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path class="h%s" d="M0 0 L10 5 L0 10 z"/></marker>' % (k, k))
    a('  </defs>\n')
    box("bg", 0.5, 0.5, 959, 505, 10)

    # 린나이 기기(묶음) — 빨간 선은 이 묶음에서 나간다
    box("group", 14, 92, 176, 162, 10)
    text("via", 102, 111, t["rinnai"])
    box("box", 24, 120, 156, 52); text("name", 102, 142, t["boiler"]); text("sub", 102, 160, t["boiler_s"])
    box("box", 24, 184, 156, 52); text("name", 102, 206, t["thermo"]); text("sub", 102, 224, t["thermo_s"])
    # 스마트폰
    box("box", 319, 22, 150, 50); text("name", 394, 43, t["phone"]); text("sub", 394, 61, t["phone_s"])
    # 공유기 + 그 안의 DNS 설정(점선) — NAS 를 가리킨다
    box("box", 310, 110, 190, 176)
    text("name l", 322, 134, t["router"])
    box("dns", 322, 190, 92, 56, 6)
    text("name", 346, 212, t["dns"], "font-size:13px"); text("sub", 346, 228, t["dns_s"], "font-size:11.5px")
    # NAS: 장치 그림 + 이름, 안에 Docker 둘. 바탕은 공유기와 같고, DNS 서버는 공유기 안의 DNS 와 같은 바탕
    box("box", 266, 330, 390, 160, 14)
    box("inner", 280, 342, 38, 30, 4)
    for i in range(4):
        a('  <rect class="bay" x="%g" y="347" width="5" height="14" rx="1"/>' % (285 + i * 7.5))
    a('  <circle class="led" cx="287" cy="367" r="1.6"/>')
    text("name l", 326, 363, t["nas"], "font-size:16px")
    # DNS 서버와 LYNNAI 허브는 같은 크기(164×68). DNS 서버는 선이 상자 안으로 들어와 돌아 나간다(화살표 없이 이어짐) —
    # 이름은 빨간 선(x=374) 왼쪽 칸의 가운데, 설명은 선들 아래에서 상자 가운데
    box("inner", 282, 400, 164, 68); text("name", 328, 431, t["nasdns"]); text("sub", 364, 455, t["nasdns_s"])
    box("hub", 478, 400, 164, 68, 10); text("name", 560, 431, t["hub"]); text("sub", 560, 455, t["hub_s"])
    # 인터넷
    # 인터넷: 두 상자 사이의 틈(y=165)으로 공유기의 인터넷 선이 들어온다. google.com 은 136, rkiot.net 은 191(스마트폰)·205(허브의 프록시 연결) 높이로 트래픽 선이 곧게 닿는다
    box("group", 744, 82, 196, 154, 10)
    text("via", 842, 102, t["internet"])
    box("box", 760, 114, 166, 44); text("name", 843, 141, "google.com")
    box("box", 760, 172, 166, 50); text("name", 843, 194, "rkiot.net"); text("sub", 843, 211, t["cloud"])
    # SmartThings
    box("box", 590, 244, 140, 48); text("name", 660, 273, t["st_hub"])
    box("box", 754, 244, 126, 48); text("name", 817, 273, t["st_app"])

    a('\n  <!-- physical links -->')
    line("line", [(180, 146), (308, 146)]); line("line", [(180, 206), (308, 206)])
    text("via", 246, 180, t["wifi"])
    line("line", [(340, 288), (340, 328)], start=True); text("via r", 332, 313, t["wired"])
    line("line", [(500, 165), (742, 165)]); text("via", 600, 158, t["internet"])
    line("line", [(500, 268), (588, 268)]); text("via", 545, 261, t["lan"])
    line("line", [(730, 268), (752, 268)])

    a('\n  <!-- traffic: Rinnai rkiot.net -> router DNS -> NAS DNS server -> LYNNAI hub -->')
    line("ta", [(190, 238), (374, 238), (374, 432), (476, 432)], "aa")
    text("la l", 196, 272, "rkiot.net")
    a('  <!-- traffic: smartphone -> router DNS -> NAS DNS server -> back out through the router -->')
    line("tb", [(388, 72), (388, 423), (434, 423), (434, 191), (758, 191)], "ab")
    line("tc", [(400, 72), (400, 414), (422, 414), (422, 136), (758, 136)], "ac")
    text("lb r", 382, 96, "rkiot.net"); text("lc l", 406, 96, "google.com")
    a('  <!-- proxy mode: hub -> router -> Rinnai cloud -->')
    line("ta", [(490, 400), (490, 205), (758, 205)], "aa")       # 린나이 기기의 트래픽이 이어지는 것이라 빨강
    text("la l", 498, 370, t["proxy"])

    a('\n  <!-- legend -->')
    for i, (cls, mk, key) in enumerate((("ta", "aa", "lg_a"), ("tb", "ab", "lg_b"), ("tc", "ac", "lg_c"))):
        y = 402 + i * 24
        line(cls, [(686, y), (716, y)], mk)
        text("l", 724, y + 4, t[key], "font-size:11.5px")
    a('</svg>')
    return "\n".join(o) + "\n"


if __name__ == "__main__":
    docs = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs")
    for lang, name in (("ko", "overview-nas.svg"), ("en", "overview-nas.en.svg")):
        with open(os.path.join(docs, name), "w", encoding="utf-8") as f:
            f.write(build(T[lang]))
        print("wrote", name)

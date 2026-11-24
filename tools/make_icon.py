#!/usr/bin/env python3
"""LYNNAI Hub 아이콘(images/icon.svg)을 만든다.

주황 바탕 위에 흰 선 하나가 육각형을 한 바퀴 돌고, 맨 아래 꼭짓점에서 안으로 꺾여 올라가 불꽃을 그린다.
SVG 하나가 그려지는 크기에 따라 선 굵기를 스스로 바꾼다(안의 @media):
    96px 이상  선 7   · 불꽃 끝이 안쪽으로 말려 들어감
    32~95px    선 11  · 같은 모양
    31px 이하  선 17  · 말림 없이 닫힌 불꽃(탭 아이콘, 허브 웹의 제목 옆)

    python3 tools/make_icon.py            images/icon.svg 를 다시 쓴다(허브 펌웨어가 이 파일을 그대로 담는다)
    python3 tools/make_icon.py <폴더>     크기를 고정한 세 벌(icon-large/medium/small.svg)도 그 폴더에 쓴다 — PNG 로 구울 때
                                          (Home Assistant 의 brand/icon.png 는 화면에 40~80px 로 나오므로 medium 으로 굽는다)
"""
import os
import sys

BG = "#f55600"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 불꽃은 100x130 칸에 그리고 S 배로 줄여 (OX, OY) 로 옮긴다 → 밑동이 (128, 178)
S, OX, OY = 0.8, 88, 74
FLAME = [(24, 130, 4, 110, 4, 84), (4, 62, 16, 48, 28, 36), (28, 52, 34, 62, 44, 64), (38, 42, 44, 18, 55, 0),
         (60, 28, 96, 46, 96, 84), (96, 106, 82, 122, 64, 124)]       # 밑동 → 왼쪽 → 끝 → 오른쪽으로 내려와 (64,124)
CURL = [(50, 126, 38, 116, 38, 102), (38, 90, 48, 84, 52, 72)]         # 이어서 안쪽으로 말려 들어가는 끝
CLOSE = [(58, 128, 54, 130, 50, 130)]                                   # 작은 크기: 말림 대신 밑동에서 닫는다
# 육각형: 오른쪽 아래 변의 끝(맨 아래 꼭짓점 조금 앞)에서 시작해 한 바퀴 돌고, 맨 아래에서 둥글게 꺾여 불꽃 밑동으로
HEX = "M150.5 203 204.2 172V84L128 40 51.8 84v88L110 205.6C124 213.7 128 200 128 178"


def pt(x, y):
    return f"{OX + x * S:g} {OY + y * S:g}"


def seg(ps):
    return "".join("C" + " ".join(pt(p[i], p[i + 1]) for i in (0, 2, 4)) for p in ps)


MAIN = HEX + seg(FLAME)
TAIL = {True: "M" + pt(64, 124) + seg(CURL), False: "M" + pt(64, 124) + seg(CLOSE)}


def adaptive():
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><title>LYNNAI Hub</title><style>'
            'path{fill:none;stroke:#fff;stroke-width:7;stroke-linecap:round;stroke-linejoin:round}.s{display:none}'
            '@media(max-width:95px){path{stroke-width:11}}'
            '@media(max-width:31px){path{stroke-width:17}.c{display:none}.s{display:inline}}'
            f'</style><rect width="256" height="256" rx="56" fill="{BG}"/>'
            f'<path d="{MAIN}"/><path class="c" d="{TAIL[True]}"/><path class="s" d="{TAIL[False]}"/></svg>\n')


def fixed(width, curl):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><rect width="256" height="256" rx="56" fill="{BG}"/>'
            f'<g fill="none" stroke="#fff" stroke-width="{width}" stroke-linecap="round" stroke-linejoin="round">'
            f'<path d="{MAIN}"/><path d="{TAIL[curl]}"/></g></svg>\n')


if __name__ == "__main__":
    with open(os.path.join(ROOT, "images", "icon.svg"), "w") as f:
        f.write(adaptive())
    if len(sys.argv) > 1:
        for name, width, curl in (("large", 7, True), ("medium", 11, True), ("small", 17, False)):
            with open(os.path.join(sys.argv[1], f"icon-{name}.svg"), "w") as f:
                f.write(fixed(width, curl))

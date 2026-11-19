#!/usr/bin/env python3
"""연결 경로 가상 모사 실행기. 실제 core.lua 를 SmartThings API 모의 위에서 돌린다(하드웨어 불필요).
  python3 run_sim.py      (lupa 필요: pip install lupa)
"""
import os, sys
from lupa import LuaRuntime
here = os.path.dirname(os.path.abspath(__file__))
L = LuaRuntime(unpack_returned_tuples=True)
L.execute('arg = {[0]=%r}' % os.path.join(here, "sim.lua"))
rc = 0
try:
    L.execute(open(os.path.join(here, "sim.lua")).read())
except Exception as e:
    print("LUA ERROR:", e); rc = 1
sys.exit(rc)

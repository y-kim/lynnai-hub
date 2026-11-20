-- sim.lua -- 연결 경로 가상 모사: 실제 core.lua 를 SmartThings API 모의 위에서 돌린다.
-- 하드웨어/재페어링 없이 보일러·써머스탯 여러 대·짝·단독을 시험한다.
--   lua sim.lua   또는   luav/bin/python -c "..."(lupa) 로 실행(run_sim.py 참고)
--
-- 허브가 보내는 메시지(펌웨어 hub_uplink.h / core.lua 맨 위)를 바이트로 만들어 core.on_message 에 먹이고,
-- core 가 만든 기기·보낸 값·기기로 보낸 명령을 모아 검사한다.

local HERE = (arg and arg[0] or "sim.lua"):gsub("[^/]*$", "")

----------------------------------------------------------------- 모의 SmartThings
local emitted = {}     -- {device_key, comp, cap, attr, value}
local created = {}     -- key -> {label, profile}
local deleted = {}     -- key -> true
local sent_cmds = {}   -- 허브로 보낸 명령 텍스트(순서대로)

-- capability 하나: 쓰는 생성자/이름만 흉내. ev = {capability={ID}, attribute={NAME}, value}
local function mk_cap(id, attrs, cmds)
  local c = { ID = id, commands = {}, attributes = {} }
  for _, a in ipairs(attrs or {}) do
    c[a] = function(v, _opts) return { capability = c, attribute = { NAME = a }, value = v } end
    c.attributes[a] = { NAME = a }
  end
  for _, cm in ipairs(cmds or {}) do
    c.commands[cm] = { NAME = cm }
  end
  -- switch.switch.on()/off() 처럼 한 겹 더 들어가는 것들
  return c
end

local capabilities = {}
local function cap(id, attrs, cmds) capabilities[id] = mk_cap(id, attrs, cmds); return capabilities[id] end

-- switch: switch.switch.on()/.off()
local sw = cap("switch", {}, {})
sw.switch = { ID = "switch",
  on = function() return { capability = sw, attribute = { NAME = "switch" }, value = "on" } end,
  off = function() return { capability = sw, attribute = { NAME = "switch" }, value = "off" } end }
sw.commands = { on = { NAME = "on" }, off = { NAME = "off" } }

local function nested(id, attr)
  local c = cap(id, {}, {})
  c[attr] = { ID = id, [attr] = function(v, o) return { capability = c, attribute = { NAME = attr }, value = v } end }
  -- 편의: capabilities[id].attr(v) 형태도 쓰므로 함수도 붙임
  return c
end

-- thermostatHeatingSetpoint: heatingSetpoint({value,unit}), heatingSetpointRange({value,unit}), commands.setHeatingSetpoint
local ths = cap("thermostatHeatingSetpoint", { "heatingSetpoint", "heatingSetpointRange" }, { "setHeatingSetpoint" })
-- mode: mode(x), supportedModes(list,opts), supportedArguments(list,opts), commands.setMode
local md = cap("mode", { "mode", "supportedModes", "supportedArguments" }, { "setMode" })
-- temperature / humidity / gas
cap("temperatureMeasurement", { "temperature" }, {})
cap("relativeHumidityMeasurement", { "humidity" }, {})
cap("gasMeter", { "gasMeterVolume" }, {})
cap("refresh", {}, { "refresh" })
-- 커스텀
cap("connectbook02461.boilerStatus", { "status" }, {})
cap("connectbook02461.roomStatus", { "roomStatus" }, {})
cap("connectbook02461.hotWaterSetpoint", { "setpoint" }, { "setSetpoint" })
local htc = cap("connectbook02461.heatType", { "heatType" }, { "setHeatType" })

package.loaded["st.capabilities"] = capabilities
package.loaded["log"] = { info = function() end, warn = function() end, error = function() end }

----------------------------------------------------------------- 기기/드라이버 모의
local driver
defer_timers, timers = false, {}
function run_timers() local t = timers; timers = {}; for _, fn in ipairs(t) do fn() end end
local function mk_device(key, label, profile, parent)
  local d = {
    id = key or "hub",
    parent_assigned_child_key = key,
    label = label,
    profile = { id = profile, components = setmetatable({}, { __index = function(_, c) return { id = c } end }) },
    _fields = {},
    _caps = {},     -- 켜진 능력(comp -> set). nil 이면 전부 지원으로 간주(초기)
  }
  function d:online() self._online = true end
  function d:offline() self._online = false end
  function d:emit_component_event(comp, ev)
    emitted[#emitted + 1] = { key = self.parent_assigned_child_key or "hub", comp = comp.id,
      cap = ev.capability.ID, attr = ev.attribute.NAME, value = ev.value }
  end
  function d:supports_capability_by_id(id, comp)
    local cc = self._caps[comp]
    if cc == nil then return true end     -- 메타데이터 적용 전엔 전부 지원
    return cc[id] == true
  end
  function d:get_parent_device() return parent end
  function d:get_child_by_parent_assigned_key(k) return driver._children[k] end
  function d:get_field(k) return self._fields[k] end
  function d:set_field(k, v, _o) self._fields[k] = v end
  function d:try_update_metadata(m)
    self.profile.id = m.profile
    self._caps = {}
    for _, c in ipairs(m.optional_component_capabilities or {}) do
      self._caps[c[1]] = {}
      for _, id in ipairs(c[2]) do self._caps[c[1]][id] = true end
    end
  end
  return d
end

local hub = mk_device(nil, "LYNNAI Hub", "lynnai-hub", nil)   -- reset() 가 매 시나리오 새 허브로 교체
driver = {
  _children = {},
  get_devices = function() local t = { hub } for _, c in pairs(driver._children) do t[#t + 1] = c end return t end,
  try_create_device = function(_, spec)
    local k = spec.parent_assigned_child_key
    created[k] = { label = spec.label, profile = spec.profile }
    driver._children[k] = mk_device(k, spec.label, spec.profile, hub)
  end,
  try_delete_device = function(_, id)
    for k, c in pairs(driver._children) do if c.id == id then deleted[k] = true; driver._children[k] = nil end end
  end,
  call_with_delay = function(_, _s, fn)              -- 보통은 즉시 실행(시뮬). defer_timers 면 모아 뒀다 run_timers() 로
    if defer_timers then timers[#timers + 1] = fn else fn() end
  end,
}
function hub:get_child_by_parent_assigned_key(k) return driver._children[k] end

----------------------------------------------------------------- core 로드
local core = dofile(HERE .. "../src/core.lua")
local all_cmds = {}            -- 시험 전체에서 드라이버가 보낸 명령(펌웨어 허용 목록 대조용)
core.setup(driver, { send = function(_, text) sent_cmds[#sent_cmds + 1] = text; all_cmds[#all_cmds + 1] = text end,
                     on_hub_init = function() end })

----------------------------------------------------------------- 메시지 빌더
local CMD = { STATE = 0x01, LINKS = 0x02, ROOM = 0x03, GAS = 0x04, PLUGINS = 0x05, EXPOSE = 0x06, PAIRS = 0x07 }
local function mac3(hex)      -- "a1b2c3" -> 3 bytes
  return string.char(tonumber(hex:sub(1, 2), 16), tonumber(hex:sub(3, 4), 16), tonumber(hex:sub(5, 6), 16))
end
local function msg(cmd, bytes) core.on_message(hub, cmd, bytes) end
local function state(kind, mac, blobhex)
  local blob = (blobhex:gsub("%x%x", function(x) return string.char(tonumber(x, 16)) end))
  msg(CMD.STATE, string.char(kind) .. mac3(mac) .. blob)
end
local function links(list)    -- {{6,"a1b2c3"},{9,"d4e5f6"}}
  local b = {} for _, e in ipairs(list) do b[#b + 1] = string.char(e[1]) .. mac3(e[2]) end
  msg(CMD.LINKS, table.concat(b))
end
local function pairs_msg(list) -- {{"d4e5f6","a1b2c3"}}  (f09->f06)
  local b = {} for _, e in ipairs(list) do b[#b + 1] = mac3(e[1]) .. mac3(e[2]) end
  msg(CMD.PAIRS, table.concat(b))
end

-- 실측 블롭(이 세션 캡처). F06 = 보일러 켜짐(전원·난방 on), F06_OFF = 전원·난방 off(깨우기 시험용)
local F06 = "10110000003c041001800fff0000055000000000000000000000000000000000"
local F06_OFF = "00010000003c041001800fff0000055000000000000000000000000000000000"
local F09 = "01000019190a00010000140f0a0000000000000000000000000000000000000000000000000000000000810000171105003c0410300000"

----------------------------------------------------------------- 검사 도구
local pass, fail = 0, 0
local function check(cond, label)
  if cond then pass = pass + 1; print("  OK   " .. label)
  else fail = fail + 1; print("  FAIL " .. label) end
end
local function last_cmd_has(sub)
  for i = #sent_cmds, 1, -1 do if sent_cmds[i]:find(sub, 1, true) then return true end end
  return false
end
local hub_n = 0
local function reset()
  emitted = {}; created = {}; deleted = {}; sent_cmds = {}; driver._children = {}
  hub_n = hub_n + 1
  hub = mk_device(nil, "LYNNAI Hub", "lynnai-hub", nil)   -- 새 허브(새 id) → core 내부 상태도 새로
  hub.id = "hub" .. hub_n
  hub.network_type = "DEVICE_ZIGBEE"
end

----------------------------------------------------------------- 시나리오
reset()
print("[1] 보일러 1 + 써머스탯 1 (암묵 연동)")
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06_OFF)
state(9, "d4e5f6", F09)
check(created["ba1b2c3"], "보일러 기기 ba1b2c3 생성")
check(created["td4e5f6_z1"] and created["td4e5f6_z3"], "써머스탯 존 방1(z1)/거실(z3) 생성")
check(not created["td4e5f6_z4"], "리포트 안 된 방3(z4)은 안 생김")
-- 거실(z3, a0=7) 켜기 → f09@d4e5f6 a0=7,b1=1 + 보일러 깨우기(암묵)
core.capability_handlers["switch"]["on"](nil, driver._children["td4e5f6_z3"], { component = "main" })
check(last_cmd_has("f09@d4e5f6 a0=7,b1=1"), "거실 켜기 → f09@d4e5f6 a0=7")
check(last_cmd_has("f06@a1b2c3"), "존 켜기 → 보일러(a1b2c3) 깨움(암묵)")

print("[2] 보일러 2대(a1b2c3,aabbcc) + 써머스탯 1 — 짝 없음: 깨움 안 함")
reset()
links({ { 6, "a1b2c3" }, { 6, "aabbcc" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06); state(6, "aabbcc", F06_OFF); state(9, "d4e5f6", F09)
check(created["baabbcc"], "둘째 보일러 baabbcc 생성")
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["td4e5f6_z1"], { component = "main" })
check(last_cmd_has("f09@d4e5f6 a0=1,b1=1"), "존 켜기 → f09@d4e5f6 a0=1")
check(not last_cmd_has("f06@"), "짝 모호(2대) → 보일러 깨움 없음")

print("[3] 같은 상태 + 짝 지정(d4e5f6→aabbcc): 그 보일러만 깨움")
pairs_msg({ { "d4e5f6", "aabbcc" } })
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["td4e5f6_z1"], { component = "main" })
check(last_cmd_has("f06@aabbcc"), "짝 지정 → 보일러 aabbcc 깨움")
check(not last_cmd_has("f06@a1b2c3"), "다른 보일러(a1b2c3)는 안 건드림")

print("[4] 단독 써머스탯(보일러 없음) → 가상 보일러 자동 생성")
reset()
links({ { 9, "d4e5f6" } })
state(9, "d4e5f6", F09)
check(created["td4e5f6_z1"], "단독 써머스탯 존 생성")
check(created["vd4e5f6"] and created["vd4e5f6"].profile == "lynnai-boiler", "가상 보일러 vd4e5f6 생성(보일러 프로필)")
-- 존 켜기: 보일러 없음(가상이라 f06 깨움은 안 보냄)
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["td4e5f6_z1"], { component = "main" })
check(last_cmd_has("f09@d4e5f6 a0=1"), "단독 존 켜기 → f09 명령")
check(not last_cmd_has("f06@"), "실보일러 없음 → f06 깨움 없음")
-- 가상 보일러로 전원·온수 명령 → 그 써머스탯의 f09 로 (f06 코드)
sent_cmds = {}
core.capability_handlers["switch"]["off"](nil, driver._children["vd4e5f6"], { component = "main" })  -- 전원 끄기
check(last_cmd_has("f09@d4e5f6 80=0"), "가상 보일러 전원끄기 → f09@d4e5f6 80=0")
sent_cmds = {}
core.capability_handlers["connectbook02461.hotWaterSetpoint"]["setSetpoint"](nil, driver._children["vd4e5f6"], { args = { setpoint = 55 } })
check(last_cmd_has("f09@d4e5f6 d1="), "가상 보일러 온수설정 → f09@d4e5f6 d1=")

print("[5] 단독→짝 지정으로 실보일러 연결되면 가상 보일러 제거")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06); state(9, "d4e5f6", F09)
-- 보일러 1대뿐이라 암묵 연동 → 단독 아님 → 가상 보일러 없어야
check(not created["vd4e5f6"], "실보일러 있으면 가상 보일러 안 생김")

print("[6] 실보일러 있어도 '단독(000000)' 지정하면 가상 보일러 생성")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06); state(9, "d4e5f6", F09)
check(not created["vd4e5f6"], "자동(기본)일 땐 가상 보일러 없음")
pairs_msg({ { "d4e5f6", "000000" } })      -- 단독 지정
check(created["vd4e5f6"], "단독 지정 → 가상 보일러 vd4e5f6 생성")
sent_cmds = {}
core.capability_handlers["connectbook02461.hotWaterSetpoint"]["setSetpoint"](nil, driver._children["vd4e5f6"], { args = { setpoint = 50 } })
check(last_cmd_has("f09@d4e5f6 d1="), "가상 보일러 온수 → f09@d4e5f6")

print("[7] 모든 방 꺼짐 → 보일러 난방 off (자동, 보일러가 안 해주는 방향)")
-- 존 stat 바이트(0=z1,7=z2,42=z3)에 on 비트(0x02)만 세팅한 54바이트 블롭
local function f09_rooms(z1, z2, z3)
  local t = {}; for i = 1, 54 do t[i] = 0 end
  t[1] = z1 and 0x83 or 0x81; t[8] = z2 and 0x83 or 0x81; t[43] = z3 and 0x83 or 0x81   -- 실기: 켜짐 0x83, 꺼짐 0x81
  t[4] = 20; t[11] = 21; t[46] = 22                                                     -- 현재 온도(방이 있다는 표시)
  local s = {}; for i = 1, 54 do s[i] = string.format("%02x", t[i]) end
  return table.concat(s)
end
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06)                        -- 보일러 전원·난방 on
state(9, "d4e5f6", f09_rooms(true, false, false))   -- 방 하나 켜짐(씨딩: 전부꺼짐 아님)
sent_cmds = {}
state(9, "d4e5f6", f09_rooms(false, false, false))  -- 마지막 방까지 꺼짐
check(last_cmd_has("f06@a1b2c3 b1=0"), "모든 방 꺼짐 → 보일러 난방 끔(f06@a1b2c3 b1=0)")
sent_cmds = {}
state(9, "d4e5f6", f09_rooms(false, false, false))  -- 전부 꺼진 상태 그대로 다시 옴
check(not last_cmd_has("b1=0"), "전부 꺼진 상태 유지 → 재전송 안 함")
-- 보일러 난방이 이미 꺼진 상태(F06_OFF)면, 방 전부 꺼져도 끄기 명령 안 보냄
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06_OFF)
state(9, "d4e5f6", f09_rooms(true, false, false))
sent_cmds = {}
state(9, "d4e5f6", f09_rooms(false, false, false))
check(not last_cmd_has("b1=0"), "보일러 난방이 이미 꺼짐 → 끄기 명령 없음")

print("[8] 방 켜짐 → 보일러 깸 (상태 기반: Rinnai 리모컨·스케줄 경로도)")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06_OFF)                          -- 보일러 전원·난방 off
state(9, "d4e5f6", f09_rooms(false, false, false))   -- 전부 꺼짐(씨딩)
sent_cmds = {}
state(9, "d4e5f6", f09_rooms(false, true, false))    -- 방2 켜짐(전부꺼짐→일부켜짐)
check(last_cmd_has("f06@a1b2c3 80=1"), "방 켜짐 → 보일러 전원 켬(80=1)")
check(last_cmd_has("b1=1"), "방 켜짐 → 보일러 난방 켬(b1=1)")
sent_cmds = {}
state(9, "d4e5f6", f09_rooms(true, true, false))     -- 다른 방도 켜짐(여전히 일부 켜짐)
check(not last_cmd_has("80=1"), "이미 일부 켜진 상태 유지 → 재깸 없음")

print("[9] 각방 '외출' 스위치 → 보일러 b3")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06); state(9, "d4e5f6", F09)
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["td4e5f6_z1"], { component = "outing" })
check(last_cmd_has("f09@d4e5f6 a0=1,b3=1"), "각방 외출 켬 → f09 a0=1,b3=1")
sent_cmds = {}
core.capability_handlers["switch"]["off"](nil, driver._children["td4e5f6_z1"], { component = "outing" })
check(last_cmd_has("f09@d4e5f6 a0=1,b3=0"), "각방 외출 끔 → f09 a0=1,b3=0")

print("[10] 보일러 예약 상태 표시(읽기전용)")
-- F06 에서 위치40(예약 켜짐)=1, 위치41(종류)=1(24시간) → ch(39)=ch(40)='1'
local F06_RES = "10110000003c041001800fff00000550" .. "00000001100000000000000000000000"
local function last_emit(key, cap, attr)
  for i = #emitted, 1, -1 do local e = emitted[i]
    if e.key == key and e.cap == cap and e.attr == attr then return e.value end
  end
end
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06_RES)
local st = last_emit("ba1b2c3", "connectbook02461.boilerStatus", "status") or ""
check(st:find("예약", 1, true) and st:find("24시간", 1, true), "예약 켜짐 → 상태에 '예약(24시간)' 표시")
-- 예약 꺼진 블롭이면 표시 없음
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06)
local st2 = last_emit("ba1b2c3", "connectbook02461.boilerStatus", "status") or ""
check(not st2:find("예약", 1, true), "예약 꺼짐 → 상태에 예약 표시 없음")

print("[11] 노출: 종류별 컨트롤 마스크 + 기기·방별 숨김(0x06 = u32 + [종류][mac3][숨김]*n)")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06); state(9, "d4e5f6", F09)
check(created["ba1b2c3"] and created["td4e5f6_z1"] and created["td4e5f6_z3"], "처음엔 다 보임")
function expose(mask, entries)
  local b = { string.pack("<I4", mask) }
  for _, e in ipairs(entries) do b[#b + 1] = string.char(e[1]) .. mac3(e[2]) .. string.char(e[3]) end
  msg(CMD.EXPOSE, table.concat(b))
end
-- 방 설정온도(9)·외출(10) 끔, 보일러 조절기 숨김(bit0), 거실(z3 → bit3) 숨김
expose(0x7FFFFFFF & ~(1 << 9) & ~(1 << 10), { { 6, "a1b2c3", 0x01 }, { 9, "d4e5f6", 0x08 } })
check(deleted["ba1b2c3"], "보일러 조절기 숨김 → 보일러 기기 빠짐")
check(deleted["td4e5f6_z3"], "거실(z3) 숨김 → 그 방 기기만 빠짐")
check(driver._children["td4e5f6_z1"] ~= nil, "방1 은 그대로")
local cz = driver._children["td4e5f6_z1"]._caps or {}
check(cz.main and cz.main["switch"] and not cz.main["thermostatHeatingSetpoint"] and not cz.outing,
  "방 컨트롤: 켜기만 보임(설정온도·외출 숨김)")
state(9, "d4e5f6", F09)
check(driver._children["td4e5f6_z3"] == nil, "상태가 다시 와도 숨긴 거실은 안 생김")
check(hub._fields["expose2"] and #hub._fields["expose2"] == 8 + 20, "노출을 저장(재시작 대비)")
-- 다시 보이게
created = {}
expose(0x7FFFFFFF, {})
check(created["ba1b2c3"] and created["td4e5f6_z3"], "숨김 풀면 보일러·거실 다시 생김")

print("[12] 노출: 가상 보일러도 보일러 컨트롤 마스크를 바로 따름")
reset()
links({ { 9, "d4e5f6" } })
state(9, "d4e5f6", F09)
local vb = driver._children["vd4e5f6"]
check(vb ~= nil, "단독 써머스탯 → 가상 보일러 있음")
expose(0x7FFFFFFF & ~(1 << 1) & ~(1 << 7), {})       -- 전원(1)·예열(7) 숨김
local vcaps = vb._caps or {}
check(vcaps.main == nil and vcaps.preheat == nil and vcaps.heat and vcaps.heat["mode"],
  "가상 보일러: 전원·예열 숨김, 난방 모드는 보임")

print("[13] 노출: 허브 칩 온도(비트 11) 끄고 켜기")
reset()
expose(0x7FFFFFFF & ~(1 << 11), {})
local hc = hub._caps or {}
check(hc.main ~= nil and not hc.main["temperatureMeasurement"], "비트 11 끔 → 허브 온도 숨김")
expose(0x7FFFFFFF, {})
hc = hub._caps or {}
check(hc.main and hc.main["temperatureMeasurement"], "비트 11 켬 → 허브 온도 보임")

print("[14] 각방 '예약' 스위치(f09 96/97)·상태(stat 0x08)")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
-- 방2(z2) stat 위치 7 = 0x0b(켜짐|예약), 다음 바이트 8 = 03(저장된 예약 값)
local F09R = F09:sub(1, 14) .. "0b03" .. F09:sub(19)
state(6, "a1b2c3", F06); state(9, "d4e5f6", F09R)
local function last_emit(key, comp, attr)
  for i = #emitted, 1, -1 do local e = emitted[i]
    if e.key == key and e.comp == comp and e.attr == attr then return e.value end end
end
check(last_emit("td4e5f6_z2", "reserve", "switch") == "on", "stat 0x08 → 방2 예약 스위치 켜짐")
local rs = last_emit("td4e5f6_z2", "main", "roomStatus")
check(rs and rs:find("예약", 1, true), "방2 상태 글에 '예약' (" .. tostring(rs) .. ")")
check(last_emit("td4e5f6_z1", "reserve", "switch") ~= "on", "방1 은 예약 아님")
sent_cmds = {}
core.capability_handlers["switch"]["off"](nil, driver._children["td4e5f6_z2"], { component = "reserve" })
check(last_cmd_has("f09@d4e5f6 a0=2,96=0"), "방2 예약 끔 → f09 a0=2,96=0")
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["td4e5f6_z2"], { component = "reserve" })
check(last_cmd_has("f09@d4e5f6 a0=2,96=1,97=03"), "방2 예약 켬 → 저장된 값 그대로 97=03")
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["td4e5f6_z3"], { component = "reserve" })
check(last_cmd_has("f09@d4e5f6 a0=7,96=1,97=01"), "거실(값 없음) 예약 켬 → a0=7,96=1,97=01")
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["td4e5f6_z2"], { component = "reserve" })
check(not table.concat(sent_cmds, " "):find("a0=2,b1=1", 1, true), "켜진 방2 는 켜기 명령 없이 예약만")
state(9, "d4e5f6", F09:sub(1, 14) .. "0303" .. F09:sub(19))     -- 예약 끈 뒤: 0x03, 값 03 은 남음
check(last_emit("td4e5f6_z2", "reserve", "switch") == "off", "0x08 꺼지면(값은 남아도) 예약 꺼짐")
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["td4e5f6_z1"], { component = "reserve" })
local joined = table.concat(sent_cmds, " | ")
check(joined:find("a0=1,b1=1", 1, true) and joined:find("a0=1,b1=1", 1, true) < joined:find("a0=1,96=1", 1, true),
  "꺼진 방1 예약 켬 → 먼저 켜고(b1=1) 예약 (" .. joined .. ")")
expose(0x7FFFFFFF & ~(1 << 12), {})
local zc = driver._children["td4e5f6_z1"]._caps or {}
check(zc.reserve == nil and zc.outing ~= nil, "노출 비트 12 끔 → 예약만 숨김(외출은 보임)")

print("[15] 예약 간격(97: 0=30분, n=n시간, 16진)")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06); state(9, "d4e5f6", F09:sub(1, 14) .. "0b02" .. F09:sub(19))   -- 방2 예약 2시간
local function lastv(key, comp, attr)
  for i = #emitted, 1, -1 do local e = emitted[i]
    if e.key == key and e.comp == comp and e.attr == attr then return e.value end end
end
check((lastv("td4e5f6_z2", "main", "roomStatus") or ""):find("예약 2시간", 1, true), "상태 글 '예약 2시간'")
check(lastv("td4e5f6_z2", "reserve", "mode") == "2시간", "간격 모드 = 2시간")
sent_cmds = {}
core.capability_handlers["mode"]["setMode"](nil, driver._children["td4e5f6_z2"], { component = "reserve", args = { mode = "12시간" } })
check(last_cmd_has("f09@d4e5f6 a0=2,96=1,97=0c"), "12시간 → 97=0c(16진)")
sent_cmds = {}
core.capability_handlers["mode"]["setMode"](nil, driver._children["td4e5f6_z2"], { component = "reserve", args = { mode = "30분" } })
check(last_cmd_has("f09@d4e5f6 a0=2,96=1,97=00"), "30분 → 97=00")
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["td4e5f6_z2"], { component = "reserve" })
check(last_cmd_has("a0=2,96=1,97=00"), "스위치 켬 → 마지막으로 고른 간격(30분) 사용")
sent_cmds = {}
core.capability_handlers["mode"]["setMode"](nil, driver._children["td4e5f6_z1"], { component = "reserve", args = { mode = "3시간" } })
local j1 = table.concat(sent_cmds, " | ")
check(j1:find("a0=1,b1=1", 1, true) and j1:find("a0=1,96=1,97=03", 1, true), "꺼진 방1 에 3시간 → 켜고 97=03")

print("[16] 재시작 직후: 연결된 조절기가 다 보고하기 전엔 자동화 판단 안 함")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" }, { 9, "eeff00" } })   -- 조절기 2대가 한 보일러(암묵)
state(6, "a1b2c3", F06_OFF)
sent_cmds = {}
state(9, "d4e5f6", f09_rooms(false, false, false))   -- 첫 조절기만: 전부 꺼짐처럼 보이지만 아직 판단 안 함
state(9, "eeff00", f09_rooms(true, false, false))    -- 둘째 도착(방 켜짐) — 여기서 처음 씨딩, 깨우지 않음
check(not last_cmd_has("80=1") and not last_cmd_has("b1=1"), "둘째 조절기 도착을 '방 켜짐'으로 오판하지 않음")
check(driver._children["vd4e5f6"] == nil and driver._children["veeff00"] == nil, "가상 보일러 만들었다 지우지 않음")
sent_cmds = {}
state(9, "eeff00", f09_rooms(false, false, false))
check(last_cmd_has("f06@a1b2c3") == false, "보일러 난방이 이미 꺼짐 → 명령 없음")

print("[17] 방이 한 번 안 보여도 기기 유지, 3번 연속이면 정리")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06); state(9, "d4e5f6", f09_rooms(true, true, false))
check(driver._children["td4e5f6_z2"] ~= nil, "방2 있음")
local gone = f09_rooms(true, false, false):sub(1, 14) .. "00000000" .. f09_rooms(true, false, false):sub(23)   -- 방2 슬롯 비움
state(9, "d4e5f6", gone)
check(driver._children["td4e5f6_z2"] ~= nil, "한 번 안 보임 → 그대로(기기 ID 유지)")
state(9, "d4e5f6", f09_rooms(true, true, false))
state(9, "d4e5f6", gone); state(9, "d4e5f6", gone)
check(driver._children["td4e5f6_z2"] ~= nil, "다시 보였다 사라지면 횟수 처음부터")
state(9, "d4e5f6", gone)
check(driver._children["td4e5f6_z2"] == nil, "3번 연속 안 보임 → 정리")

print("[18] 보일러 '외출' = 붙은 방마다 f09 a0,b3 (f06 b3 는 안 먹힘)")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06); state(9, "d4e5f6", f09_rooms(true, true, true))
sent_cmds = {}
core.capability_handlers["mode"]["setMode"](nil, driver._children["ba1b2c3"], { component = "heat", args = { mode = "외출" } })
local jo = table.concat(sent_cmds, " | ")
check(jo:find("f09@d4e5f6 a0=1,b3=1", 1, true) and jo:find("f09@d4e5f6 a0=2,b3=1", 1, true)
  and jo:find("f09@d4e5f6 a0=7,b3=1", 1, true), "외출 → 방1·방2·거실 각각 b3=1")
check(not jo:find("f06@a1b2c3 b3", 1, true), "f06 b3 는 안 보냄")
sent_cmds = {}
core.capability_handlers["mode"]["setMode"](nil, driver._children["ba1b2c3"], { component = "heat", args = { mode = "이상한값" } })
check(#sent_cmds == 0, "모르는 모드 → 무시(난방 끄지 않음)")
-- 모든 방이 외출이면 보일러 모드도 '외출'로 보임
local allout = f09_rooms(true, true, true):gsub("^(..)", "93")   -- 방1 stat 0x93(켜짐+외출)
local t2 = {}
for i = 1, #allout, 2 do t2[#t2 + 1] = allout:sub(i, i + 1) end
t2[8] = "93"; t2[43] = "93"
state(9, "d4e5f6", table.concat(t2))
check(lastv("ba1b2c3", "heat", "mode") == "외출", "모든 방 외출 → 보일러 모드 '외출'")
state(6, "a1b2c3", F06)                                        -- 보일러가 같은 상태를 다시 보고해도
check(lastv("ba1b2c3", "heat", "mode") == "외출", "보일러 보고가 와도 '외출' 표시 유지")
sent_cmds = {}
core.capability_handlers["mode"]["setMode"](nil, driver._children["ba1b2c3"], { component = "heat", args = { mode = "난방" } })
local jn = table.concat(sent_cmds, " | ")
check(jn:find("a0=1,b3=0", 1, true) and jn:find("f06@a1b2c3 b1=1", 1, true), "난방 → 방 외출 풀고 b1=1")

print("[19] 표 값 이벤트(설정 범위)도 중복 억제")
reset()
links({ { 6, "a1b2c3" }, { 9, "c0ffee" } })
state(6, "a1b2c3", F06); state(9, "c0ffee", F09)   -- 새 MAC(앞 시나리오의 중복 억제 기록과 안 겹치게)
local function count_range()
  local n = 0
  for _, e in ipairs(emitted) do if e.key == "tc0ffee_z1" and e.attr == "heatingSetpointRange" then n = n + 1 end end
  return n
end
local n1 = count_range()
core.on_message(hub, CMD.LINKS, string.char(6) .. mac3("a1b2c3") .. string.char(9) .. mac3("c0ffee"))
check(n1 >= 1 and count_range() == n1, "같은 설정 범위는 다시 안 보냄")

print("[20] 드라이버가 보내는 코드는 모두 펌웨어 허용 목록에 있다")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06); state(9, "d4e5f6", f09_rooms(true, true, true))
for _, comp in ipairs({ "main", "hotwater", "preheat" }) do
  for _, onoff in ipairs({ "on", "off" }) do
    core.capability_handlers["switch"][onoff](nil, driver._children["ba1b2c3"], { component = comp })
  end
end
local jsw = table.concat(sent_cmds, " | ")
check(jsw:find("f06@a1b2c3 d7=", 1, true) and jsw:find("f06@a1b2c3 b6=", 1, true), "온수(d7)·예열(b6) 스위치 명령을 보냄")
local allowed, src = {}, nil
do
  local dir = (arg and arg[0] or "sim.lua"):match("^(.*)[/\\]") or "."
  local f = io.open(dir .. "/../../firmware/esp32/main/hub_rinnai.c")
  if f then src = f:read("a"); f:close() end
end
local okline = src and src:match("code_allowed.-OK%[%]%s*=%s*(%b{})")
for c in (okline or ""):gmatch('"(%x%x)"') do allowed[c] = true end
check(okline ~= nil and allowed["80"], "펌웨어 허용 목록을 읽음(hub_rinnai.c code_allowed)")
local bad = {}
for _, text in ipairs(all_cmds) do
  for one in text:gmatch("[^;]+") do
    local body = one:match("^%s*f0[69]@?%x*%s+(.+)$")
    for code in (body or ""):gmatch("(%x%x)=") do
      if not allowed[code] then bad[code] = true end
    end
  end
end
local badlist = {}
for c in pairs(bad) do badlist[#badlist + 1] = c end
check(#all_cmds > 20 and #badlist == 0, "보낸 명령 " .. #all_cmds .. "개의 코드가 모두 허용됨" ..
  (#badlist > 0 and (" — 빠진 코드: " .. table.concat(badlist, ",")) or ""))

print("[21] 끊긴 조절기의 옛 상태로 판단하지 않는다")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })                       -- 연결: 보일러 + 조절기1. 조절기2(e7e8e9)는 끊겨 있음
pairs_msg({ { "d4e5f6", "a1b2c3" }, { "e7e8e9", "a1b2c3" } })
local F06_OFF = F06:gsub("^(..)", "00")                           -- 보일러 전원 꺼짐(byte0=0)
state(6, "a1b2c3", F06_OFF); state(9, "d4e5f6", f09_rooms(false, false, false))
sent_cmds = {}
state(9, "e7e8e9", f09_rooms(true, true, true))                   -- 끊긴 조절기의 옛 상태(방 켜짐)가 뒤늦게 옴
check(#sent_cmds == 0, "끊긴 조절기의 '켜짐'으로 보일러를 깨우지 않음")
check(driver._children["te7e8e9_z1"] == nil or driver._children["te7e8e9_z1"]._online ~= true, "끊긴 조절기의 방은 온라인으로 되돌리지 않음")
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" }, { 9, "e7e8e9" } })      -- 조절기2 가 실제로 붙음: 기준만 다시 잡는다
state(9, "e7e8e9", f09_rooms(true, true, true))
check(#sent_cmds == 0, "조절기가 새로 붙은 순간은 씨딩만(명령 없음)")
state(9, "e7e8e9", f09_rooms(false, false, false)); state(9, "e7e8e9", f09_rooms(true, false, false))
check(last_cmd_has("f06@a1b2c3 80=1"), "그 뒤 방이 켜지면 보일러 깨움")

print("[22] 3초 안에 돌아온 기기는 오프라인으로 남지 않는다 / 허브가 사라지면 자식 모두 오프라인")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06); state(9, "d4e5f6", f09_rooms(true, true, true))
core.on_message(hub, CMD.ROOM, string.pack("<i2i2", 2250, 4000))
defer_timers = true
links({ { 9, "d4e5f6" } })                                         -- 보일러 끊김 → 3초 뒤 오프라인 예약
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } }); state(6, "a1b2c3", F06)   -- 3초 안에 돌아옴
run_timers()
defer_timers = false
check(driver._children["ba1b2c3"]._online == true, "예약된 오프라인이 무름 → 온라인 유지")
local sensor
for k, c in pairs(driver._children) do if not k:match("^[btv]%x") then sensor = c end end
check(sensor ~= nil and sensor._online == true, "센서 자식 온라인")
core.hub_lost(hub)
local alloff = true
for _, c in pairs(driver._children) do if c._online ~= false then alloff = false end end
check(alloff and sensor._online == false, "허브 사라짐 → 보일러·방·센서 모두 오프라인")

print("[23] 붙은 방이 없는 보일러는 '외출'을 내놓지 않는다 / 상태를 못 보내는 기기는 2분 뒤 기다리지 않는다")
reset()
links({ { 6, "a1b2c3" } })
state(6, "a1b2c3", F06)
local sm = lastv("ba1b2c3", "heat", "supportedModes")
check(type(sm) == "table" and #sm == 2, "조절기 없는 보일러: 모드 2개(난방·끄기)")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" }, { 9, "e7e8e9" } })      -- e7e8e9 는 목록에만 있고 상태를 안 보냄
pairs_msg({ { "d4e5f6", "a1b2c3" }, { "e7e8e9", "a1b2c3" } })
state(6, "a1b2c3", F06); state(9, "d4e5f6", f09_rooms(true, true, true)); state(9, "d4e5f6", f09_rooms(false, false, false))
check(not last_cmd_has("f06@a1b2c3 b1=0"), "다 보고하기 전(2분 안)엔 판단 안 함")
local real_time = os.time
os.time = function() return real_time() + 200 end
state(9, "d4e5f6", f09_rooms(true, true, true)); state(9, "d4e5f6", f09_rooms(false, false, false))
os.time = real_time
check(last_cmd_has("f06@a1b2c3 b1=0"), "2분이 지나면 그 기기를 빼고 자동화 동작")

print("[24] 유일하게 켜진 방이 한 번 안 보여도(슬롯 0) 난방을 끄지 않는다")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06); state(9, "d4e5f6", f09_rooms(true, false, false)); state(9, "d4e5f6", f09_rooms(true, false, false))
sent_cmds = {}
local nz1 = string.rep("00", 7) .. f09_rooms(true, false, false):sub(15)   -- 방1 슬롯(7바이트)만 0
state(9, "d4e5f6", nz1)
check(not last_cmd_has("b1=0"), "방1 이 한 번 안 보임 → 판단 보류(난방 유지)")
state(9, "d4e5f6", f09_rooms(true, false, false))
check(#sent_cmds == 0, "방1 이 다시 보임 → 명령 없음")
state(9, "d4e5f6", f09_rooms(false, false, false))
check(last_cmd_has("f06@a1b2c3 b1=0"), "방1 을 실제로 끄면 난방 끔")

print(string.format("\n== %d통과 / %d실패 ==", pass, fail))
os.exit(fail == 0 and 0 or 1)

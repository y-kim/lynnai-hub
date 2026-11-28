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
    c[a] = function(v, opts) return { capability = c, attribute = { NAME = a }, value = v, state_change = opts and opts.state_change } end
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
cap("connectbook02461.reserveHours", { "hours" }, { "setHours" })
-- temperature / humidity / gas
cap("temperatureMeasurement", { "temperature" }, {})
cap("relativeHumidityMeasurement", { "humidity" }, {})
cap("gasMeter", { "gasMeterVolume" }, {})
cap("refresh", {}, { "refresh" })
-- 커스텀
cap("connectbook02461.boilerStatus", { "status" }, {})
cap("connectbook02461.roomStatus", { "roomStatus" }, {})
cap("thermostatWaterHeatingSetpoint", { "heatingSetpoint", "heatingSetpointRange" }, { "setHeatingSetpoint" })
local htc = cap("thermostatMode", { "thermostatMode", "supportedThermostatModes" }, { "setThermostatMode" })
cap("connectbook02461.gasBill", { "month", "today", "monthRange", "todayRange" }, {})
cap("connectbook02461.gasUsed", { "day", "hour", "period", "dayText", "hourText", "periodText" }, {})
cap("connectbook02461.gasFlowRate", { "flow", "flowText" }, {})
cap("connectbook02461.gasAlert", { "alert" }, {})
cap("connectbook02461.gasMeterSignal", { "signal" }, {})
cap("connectbook02461.boilerGasBill", { "usage", "usageText", "cost", "costRange" }, {})

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
      cap = ev.capability.ID, attr = ev.attribute.NAME, value = ev.value, state_change = ev.state_change }
  end
  function d:supports_capability_by_id(id, comp)
    -- 프로필에서 optional 이 아닌 것(상태 글자·상태 카드·온도·새로고침 등)은 늘 지원 — 실제 라이브러리와 같게
    local OPTIONAL = { switch = true, mode = true, thermostatHeatingSetpoint = true, ["connectbook02461.reserveHours"] = true,
      thermostatMode = true, thermostatWaterHeatingSetpoint = true, ["connectbook02461.boilerGasBill"] = true }
    if not OPTIONAL[id] then return true end
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
local CMD = { STATE = 0x01, LINKS = 0x02, ROOM = 0x03, GAS = 0x04, PLUGINS = 0x05, EXPOSE = 0x06, PAIRS = 0x07, TARIFF = 0x09, MODELS = 0x0d }
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
core.capability_handlers["thermostatWaterHeatingSetpoint"]["setHeatingSetpoint"](nil, driver._children["vd4e5f6"], { args = { setpoint = 55 } })
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
core.capability_handlers["thermostatWaterHeatingSetpoint"]["setHeatingSetpoint"](nil, driver._children["vd4e5f6"], { args = { setpoint = 50 } })
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
check(vcaps.main == nil and vcaps.preheat == nil and vcaps.heat and vcaps.heat["switch"],
  "가상 보일러: 전원·예열 숨김, 난방 스위치는 보임")

print("[13] 노출: 허브 칩 온도(비트 11) 끄고 켜기")
reset()
expose(0x7FFFFFFF & ~(1 << 11), {})
local hc = hub._caps or {}
check(hc.chip ~= nil and not hc.chip["temperatureMeasurement"], "비트 11 끔 → 허브 온도 숨김")
expose(0x7FFFFFFF, {})
hc = hub._caps or {}
check(hc.chip and hc.chip["temperatureMeasurement"], "비트 11 켬 → 허브 온도 보임")
-- 온도 값은 생존 신호(0x0a)에 실려 온다: [0][s16 1/100°C]
core.on_message(hub, 0x0a, string.char(0) .. string.pack("<i2", 3215))
local function count_chip() local n = 0
  for _, e in ipairs(emitted) do if e.key == "hub" and e.comp == "chip" then n = n + 1 end end
  return n end
local chip_ev
for _, e in ipairs(emitted) do if e.key == "hub" and e.comp == "chip" and e.attr == "temperature" then chip_ev = e.value end end
check(chip_ev and math.abs((type(chip_ev) == "table" and chip_ev.value or chip_ev) - 32.2) < 0.01, "생존 신호에 실린 허브 온도를 보임(32.15 → 32.2)")
local n_chip = count_chip()
core.on_message(hub, 0x0a, string.char(0) .. string.pack("<i2", 3230))
check(count_chip() == n_chip, "조금(0.5°C 미만) 바뀐 것은 다시 내지 않음")
core.on_message(hub, 0x0a, string.char(0) .. string.pack("<i2", 3400))
check(count_chip() == n_chip + 1, "0.5°C 넘게 바뀌면 냄")
core.on_message(hub, 0x0a, string.char(0))
check(count_chip() == n_chip + 1, "온도가 없는 생존 신호(옛 펌웨어·꺼 둠)는 그대로 받음")
check(not (hub._caps.main or {}).mode, "모드를 알려 주지 않는 펌웨어: 허브 기기에 상태·모드 칸 없음")

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
check(joined:find("a0=1,96=1,97=01", 1, true) and not joined:find("b1=", 1, true),
  "꺼진 방1 예약 켬 → 켜는 명령 없이 예약만(기기가 알아서 켠다) (" .. joined .. ")")
expose(0x7FFFFFFF & ~(1 << 12), {})
local zc = driver._children["td4e5f6_z1"]._caps or {}
check(zc.reserve == nil and zc.outing ~= nil, "노출 비트 12 끔 → 예약만 숨김(외출은 보임)")

print("[15] 예약 시간(97: 1~24시간, 16진 — 0시간 예약은 없다)")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06); state(9, "d4e5f6", F09:sub(1, 14) .. "0b02" .. F09:sub(19))   -- 방2 예약 2시간
local function lastv(key, comp, attr)
  for i = #emitted, 1, -1 do local e = emitted[i]
    if e.key == key and e.comp == comp and e.attr == attr then return e.value end end
end
check((lastv("td4e5f6_z2", "main", "roomStatus") or ""):find("예약 2시간", 1, true), "상태 글 '예약 2시간'")
check(lastv("td4e5f6_z2", "reserve", "hours").value == 2 and lastv("td4e5f6_z2", "reserve", "hours").unit == "시간", "예약 시간 슬라이더 = 2 시간")
sent_cmds = {}
core.capability_handlers["connectbook02461.reserveHours"]["setHours"](nil, driver._children["td4e5f6_z2"], { component = "reserve", args = { hours = 12 } })
check(last_cmd_has("f09@d4e5f6 a0=2,96=1,97=0c"), "12시간 → 96=1,97=0c(16진)")
sent_cmds = {}
core.capability_handlers["connectbook02461.reserveHours"]["setHours"](nil, driver._children["td4e5f6_z2"], { component = "reserve", args = { hours = 0 } })
check(last_cmd_has("f09@d4e5f6 a0=2,96=1,97=01"), "0 은 없는 값 → 1시간으로 보냄(97=00 은 보내지 않는다)")
sent_cmds = {}
core.capability_handlers["connectbook02461.reserveHours"]["setHours"](nil, driver._children["td4e5f6_z2"], { component = "reserve", args = { hours = 30 } })
check(last_cmd_has("f09@d4e5f6 a0=2,96=1,97=18"), "24 넘는 값 → 24시간(97=18)")
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["td4e5f6_z2"], { component = "reserve" })
check(last_cmd_has("a0=2,96=1,97=18"), "스위치 켬 → 마지막으로 고른 시간(24시간) 사용")
sent_cmds = {}
core.capability_handlers["connectbook02461.reserveHours"]["setHours"](nil, driver._children["td4e5f6_z1"], { component = "reserve", args = { hours = 3 } })
check(#sent_cmds == 0, "예약이 꺼진 방1 에서 시간을 바꾸면 아무것도 보내지 않음")
check(lastv("td4e5f6_z1", "reserve", "hours").value == 3, "고른 3시간은 슬라이더에 그대로(기억)")
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["td4e5f6_z1"], { component = "reserve" })
local j1 = table.concat(sent_cmds, " | ")
check(j1:find("a0=1,96=1,97=03", 1, true) and not j1:find("b1=", 1, true), "방1 예약을 켜면 기억한 3시간으로, 켜는 명령 없이 97=03")

print("[15b] 방 온도는 5~40도")
sent_cmds = {}
core.capability_handlers["thermostatHeatingSetpoint"]["setHeatingSetpoint"](nil, driver._children["td4e5f6_z1"], { component = "main", args = { setpoint = 3 } })
check(last_cmd_has("a0=1,d4=05"), "3도 → 5도(d4=05)")
core.capability_handlers["thermostatHeatingSetpoint"]["setHeatingSetpoint"](nil, driver._children["td4e5f6_z1"], { component = "main", args = { setpoint = 40 } })
check(last_cmd_has("a0=1,d4=28"), "40도 → d4=28")

print("[15c] 시간 모드: 외출 위의 '시간' 스위치 — 난방과 하나만 켜짐, 끄면 그냥 꺼짐")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06)
local tm = f09_rooms(true, false, true)
state(9, "d4e5f6", tm)
local zc = driver._children["td4e5f6_z1"]
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, zc, { component = "timemode" })
check(table.concat(sent_cmds, " | ") == "f09@d4e5f6 a0=1,b4=1", "켜진 방에서 시간 켬 → b4=1 만")
state(9, "d4e5f6", "87" .. tm:sub(3))                                -- 0x87 = 켜짐 + 시간 모드
check(lastv("td4e5f6_z1", "main", "switch") == "off" and lastv("td4e5f6_z1", "timemode", "switch") == "on",
  "시간 모드: 난방 스위치 꺼짐, 시간 스위치 켜짐")
check((lastv("td4e5f6_z1", "main", "roomStatus") or ""):find("시간 난방", 1, true), "요약 글 '시간 난방'")
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, zc, { component = "main" })
check(table.concat(sent_cmds, " | ") == "f09@d4e5f6 a0=1,b4=0", "시간 모드에서 난방 켬 → b4=0 만(온도로)")
sent_cmds = {}
core.capability_handlers["switch"]["off"](nil, zc, { component = "timemode" })
check(table.concat(sent_cmds, " | ") == "f09@d4e5f6 a0=1,b1=0", "시간 끔 → 그냥 방 끔(b1=0)")
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["td4e5f6_z2"], { component = "timemode" })
check(table.concat(sent_cmds, " | ") == "f09@d4e5f6 a0=2,b1=1;f09@d4e5f6 a0=2,b4=1", "꺼진 방2 에서 시간 켬 → 켜고(b1=1) 시간 모드(b4=1)")

print("[15d] 시간 모드의 분(1시간에 5·10·20…60분, f09 da) — 시간 모드가 아니면 기억만, 켤 때 보냄")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06)
local function with_tmin(hex, slot, v)                               -- 슬롯 6번째 바이트(분)
  local pos = slot * 7 + 5
  return hex:sub(1, pos * 2) .. string.format("%02x", v) .. hex:sub(pos * 2 + 3)
end
local base = with_tmin(f09_rooms(true, false, false), 0, 10)
state(9, "d4e5f6", base)
local zc = driver._children["td4e5f6_z1"]
-- (모의 기기는 시험마다 ID 를 다시 써서, 앞 시험에서 이미 보낸 값은 걸러진다 — 새 값으로 확인)
state(9, "d4e5f6", with_tmin(base, 1, 0x14))
check(lastv("td4e5f6_z2", "timemode", "mode") == "20분", "기기 값 14 → 20분")
sent_cmds = {}
core.capability_handlers["mode"]["setMode"](nil, zc, { component = "timemode", args = { mode = "30분" } })
check(#sent_cmds == 0, "시간 모드가 아닐 때 고르면 보내지 않음")
check(lastv("td4e5f6_z1", "timemode", "mode") == "30분", "고른 값(30분)을 보여 줌")
state(9, "d4e5f6", base)
check(lastv("td4e5f6_z1", "timemode", "mode") == "30분", "새 보고가 와도 기억한 30분 그대로")
core.capability_handlers["switch"]["on"](nil, zc, { component = "timemode" })
check(table.concat(sent_cmds, " | ") == "f09@d4e5f6 a0=1,b4=1;f09@d4e5f6 a0=1,da=1e", "시간 켬 → b4=1 다음 da=1e(30분)")
local tmode = with_tmin("87" .. base:sub(3), 0, 30)
state(9, "d4e5f6", tmode)
check(lastv("td4e5f6_z1", "timemode", "mode") == "30분", "시간 모드: 기기 값 30분")
check((lastv("td4e5f6_z1", "main", "roomStatus") or ""):find("시간 난방 30분", 1, true), "요약 글 '시간 난방 30분'")
sent_cmds = {}
core.capability_handlers["mode"]["setMode"](nil, zc, { component = "timemode", args = { mode = "5분" } })
check(table.concat(sent_cmds, " | ") == "f09@d4e5f6 a0=1,da=05", "시간 모드에서 고르면 바로 da=05")
sent_cmds = {}
core.capability_handlers["mode"]["setMode"](nil, zc, { component = "timemode", args = { mode = "15분" } })
check(#sent_cmds == 0, "목록에 없는 값은 무시")
state(9, "d4e5f6", with_tmin(tmode, 0, 5))
check(lastv("td4e5f6_z1", "timemode", "mode") == "5분", "기기 값을 따라감(5분)")
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, zc, { component = "main" })
state(9, "d4e5f6", with_tmin(base, 0, 5))
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, zc, { component = "timemode" })
check(table.concat(sent_cmds, " | ") == "f09@d4e5f6 a0=1,b4=1", "기억한 값이 없으면 시간 켬은 b4=1 만")
state(9, "d4e5f6", with_tmin(base, 0, 7))
check(lastv("td4e5f6_z1", "timemode", "mode") == "5분", "목록 밖 기기 값(7)은 가까운 5분으로")

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
core.capability_handlers["switch"]["on"](nil, driver._children["ba1b2c3"], { component = "outing" })
local jo = table.concat(sent_cmds, " | ")
check(jo:find("f09@d4e5f6 a0=1,b3=1", 1, true) and jo:find("f09@d4e5f6 a0=2,b3=1", 1, true)
  and jo:find("f09@d4e5f6 a0=7,b3=1", 1, true), "외출 → 방1·방2·거실 각각 b3=1")
check(not jo:find("f06@a1b2c3 b3", 1, true), "f06 b3 는 안 보냄")
check(not jo:find("b1=", 1, true), "외출 켜기는 보일러의 난방 비트를 건드리지 않음(풀면 난방으로 돌아오게)")
check(lastv("ba1b2c3", "heat", "switch") == "on" and lastv("ba1b2c3", "outing", "switch") == "off", "평소: 난방 스위치 켜짐 · 외출 꺼짐")
-- 모든 방이 외출이면 보일러 모드도 '외출'로 보임
local allout = f09_rooms(true, true, true):gsub("^(..)", "93")   -- 방1 stat 0x93(켜짐+외출)
local t2 = {}
for i = 1, #allout, 2 do t2[#t2 + 1] = allout:sub(i, i + 1) end
t2[8] = "93"; t2[43] = "93"
state(9, "d4e5f6", table.concat(t2))
check(lastv("ba1b2c3", "outing", "switch") == "on" and lastv("ba1b2c3", "heat", "switch") == "off", "모든 방 외출 → 외출 스위치 켜짐 · 난방 스위치는 꺼짐으로")
state(6, "a1b2c3", F06)                                        -- 보일러가 같은 상태를 다시 보고해도
check(lastv("ba1b2c3", "outing", "switch") == "on", "보일러 보고가 와도 '외출' 표시 유지")
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["ba1b2c3"], { component = "heat" })
local jn = table.concat(sent_cmds, " | ")
check(jn:find("a0=1,b3=0", 1, true) and jn:find("f06@a1b2c3 b1=1", 1, true), "난방 켜기 → 방 외출 풀고 b1=1")
sent_cmds = {}
core.capability_handlers["switch"]["off"](nil, driver._children["ba1b2c3"], { component = "outing" })
local jf = table.concat(sent_cmds, " | ")
check(jf:find("a0=1,b3=0", 1, true) and not jf:find("b1=", 1, true), "외출 끄기 → 방 외출만 풂(난방 비트는 그대로)")
sent_cmds = {}
core.capability_handlers["switch"]["off"](nil, driver._children["ba1b2c3"], { component = "heat" })
local jh = table.concat(sent_cmds, " | ")
check(jh:find("f06@a1b2c3 b1=0", 1, true) and not jh:find("b3=", 1, true), "난방 끄기 → b1=0 만(외출은 안 건드림)")

-- 꺼진 방은 외출을 걸지 않고 세지도 않는다(꺼진 방에 b3=1 을 보내면 켜짐+외출이 되고, 풀면 켜진 채 남는다)
state(9, "d4e5f6", f09_rooms(true, false, true))
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["ba1b2c3"], { component = "outing" })
local jp = table.concat(sent_cmds, " | ")
check(jp:find("a0=1,b3=1", 1, true) and jp:find("a0=7,b3=1", 1, true) and not jp:find("a0=2,", 1, true),
  "외출 → 켜진 방(방1·거실)에만 b3=1, 꺼진 방2 는 그대로")
local part = {}
for i = 1, 108, 2 do part[#part + 1] = f09_rooms(true, false, true):sub(i, i + 1) end
part[1] = "93"; part[43] = "93"                                 -- 방1·거실 켜짐+외출, 방2 꺼짐
state(9, "d4e5f6", table.concat(part))
check(lastv("ba1b2c3", "outing", "switch") == "on", "켜진 방이 모두 외출이면(꺼진 방이 있어도) 보일러 '외출' 켜짐")
state(9, "d4e5f6", f09_rooms(false, false, false))
check(lastv("ba1b2c3", "outing", "switch") == "off", "방이 모두 꺼져 있으면 외출 아님")
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["ba1b2c3"], { component = "outing" })
check(#sent_cmds == 0, "방이 모두 꺼져 있으면 외출 명령을 보내지 않음")
state(9, "d4e5f6", f09_rooms(true, true, true))
print("[18b] 온수 설정은 보일러가 받는 값(35~47·50·55·60)으로만")
sent_cmds = {}
core.capability_handlers["thermostatWaterHeatingSetpoint"]["setHeatingSetpoint"](nil, driver._children["ba1b2c3"], { component = "hotwater", args = { setpoint = 52 } })
local jw = table.concat(sent_cmds, " | ")
check(jw:find("d1=32", 1, true) or jw:find("d1=37", 1, true), "52도 → 50 이나 55 로(" .. jw .. ")")

print("[18c] 난방수 모드: 각방 조절기가 붙은 보일러는 온도조절 모드를 숨기고 '난방수'로 보인다")
reset()
links({ { 6, "a1b2c3" } })
state(6, "a1b2c3", F06)
local solo = driver._children["ba1b2c3"]
check(solo.profile.id == "lynnai-boiler" and solo._caps.heat and solo._caps.heat["thermostatMode"],
  "조절기 없는 보일러: 온돌/실내 모드 — 온도조절 모드가 보임")
check((last_emit("ba1b2c3", "main", "status") or ""):find("온돌", 1, true), "요약 글자는 '온돌 …'")
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(9, "d4e5f6", f09_rooms(true, true, true)); state(6, "a1b2c3", F06)
check(solo.profile.id == "lynnai-boiler-water" and solo._caps.heat and not solo._caps.heat["thermostatMode"]
  and solo._caps.heat["thermostatHeatingSetpoint"], "조절기가 붙으면 난방수 모드 — 온도조절 모드 숨김, 프로필 lynnai-boiler-water")
check((last_emit("ba1b2c3", "main", "status") or ""):find("난방수", 1, true), "요약 글자는 '난방수 …'")
links({ { 6, "a1b2c3" } })                                     -- 조절기 연결이 잠깐 끊겨도
state(6, "a1b2c3", F06)
check(solo.profile.id == "lynnai-boiler-water", "조절기 연결이 끊긴 동안에도 난방수 모드 그대로(화면이 오락가락하지 않게)")
check(hub._fields["water_a1b2c3"] == true, "난방수 모드를 기억(재시작 대비)")

print("[18e] 보일러 모드: 허브 설정(0x0c 둘째 바이트)이 먼저, 보일러가 '실내' 면 무조건 온돌·실내")
reset()
links({ { 6, "a1b2c3" } })
state(6, "a1b2c3", F06)
local bm = driver._children["ba1b2c3"]
check(bm.profile.id == "lynnai-boiler", "설정 없음 + 조절기 없음: 온돌·실내")
msg(0x0c, string.char(0, 1))
check(bm.profile.id == "lynnai-boiler-water" and not bm._caps.heat["thermostatMode"], "허브 설정 '난방수'(1): 조절기를 몰라도 난방수 모드")
check(hub._fields["bmode"] == 1, "설정을 기억(재시작 대비)")
state(6, "a1b2c3", F06:sub(1, 7) .. "1" .. F06:sub(9))
check(bm.profile.id == "lynnai-boiler" and bm._caps.heat["thermostatMode"], "보일러가 '실내' 로 알리면 설정과 상관없이 온돌·실내")
state(6, "a1b2c3", F06)
check(bm.profile.id == "lynnai-boiler-water", "다시 온돌로 알리면 설정대로 난방수")
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(9, "d4e5f6", f09_rooms(true, true, true))
msg(0x0c, string.char(0, 2))
check(bm.profile.id == "lynnai-boiler", "허브 설정 '온돌·실내'(2): 조절기가 붙어 있어도 온돌·실내")
msg(0x0c, string.char(0))
check(bm.profile.id == "lynnai-boiler-water", "옛 펌웨어(1바이트) = 정하지 않음 → 조절기 연결로 짐작(난방수)")

print("[18d] 온수 예열 자동 끄기: 허브가 알려 주면 온수 예열 구역에 '자동 끄기' 목록, 고르면 hub preoff=<분>")
reset()
links({ { 6, "a1b2c3" } })
state(6, "a1b2c3", F06)
local pb = driver._children["ba1b2c3"]
check(not (pb._caps.preheat and pb._caps.preheat["mode"]), "옛 펌웨어(설정을 안 알려 줌): 자동 끄기 칸 없음")
msg(0x0c, string.char(0))
check(pb._caps.preheat and pb._caps.preheat["mode"], "허브가 설정을 알려 주면 자동 끄기 칸이 생김")
state(6, "a1b2c3", F06)
check(lastv("ba1b2c3", "preheat", "mode") == "무제한", "0 = 무제한")
sent_cmds = {}
core.capability_handlers["mode"]["setMode"](nil, pb, { component = "preheat", args = { mode = "30분" } })
check(last_cmd_has("hub preoff=30"), "30분을 고르면 hub preoff=30")
msg(0x0c, string.char(30))
check(lastv("ba1b2c3", "preheat", "mode") == "30분", "허브가 30 을 알려 주면 30분으로 보임")
check(hub._fields["preoff"] == 30, "값을 기억(재시작 대비)")
msg(0x0c, string.char(30, 0, 14))
check(lastv("ba1b2c3", "preheat", "mode") == "14분", "예열이 켜져 있으면 허브가 알려 준 남은 분(14분)")
msg(0x0c, string.char(30, 0, 255))
check(lastv("ba1b2c3", "preheat", "mode") == "무제한", "켜짐·무제한(255) → 무제한")
msg(0x0c, string.char(30, 0, 0))
check(lastv("ba1b2c3", "preheat", "mode") == "30분", "예열이 꺼지면 기본값(30분)")
sent_cmds = {}
core.capability_handlers["mode"]["setMode"](nil, pb, { component = "preheat", args = { mode = "7분" } })
check(last_cmd_has("hub preoff=7"), "1~60분 아무 값이나: 7분 → hub preoff=7")
sent_cmds = {}
core.capability_handlers["mode"]["setMode"](nil, hub, { component = "main", args = { mode = "단독" } })
check(last_cmd_has("hub mode=standalone"), "허브 기기의 모드 목록은 그대로 동작 모드")

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
sent_cmds = {}
core.capability_handlers["switch"]["on"](nil, driver._children["ba1b2c3"], { component = "outing" })
check(#sent_cmds == 0, "조절기 없는 보일러: 외출을 걸 방이 없어 보내는 명령 없음")
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

print("[25] 허브가 고장으로 본 센서: 온습도계는 오프라인, 가스 '펄스 없음'은 경고(온라인 그대로), 풀리면 원래대로")
reset()
core.on_message(hub, CMD.ROOM, string.pack("<i2i2", 2250, 4000))
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4", 305, 0xFFFFFFFF, 0, 0, 0))
local room, gas = driver._children["room"], driver._children["gas"]
check(room and gas and room._online == true and gas._online == true, "센서 둘 다 온라인")
core.on_message(hub, CMD.PLUGINS, string.char(3))                  -- 옛 펌웨어(첫 바이트만): 고장 없음
check(room._online == true and gas._online == true, "옛 펌웨어 알림(1바이트) → 그대로 온라인")
core.on_message(hub, CMD.PLUGINS, string.char(3, 1))               -- 온습도계 응답 없음
check(room._online == false and gas._online == true, "온습도계 고장 → 온습도계만 오프라인")
check(lastv("gas", "main", "signal") == "normal", "계량기 신호 = 정상")
core.on_message(hub, CMD.PLUGINS, string.char(3, 3))               -- 가스도: 보일러를 쓰는데 펄스 없음
local function gas_warned() return (lastv("gas", "main", "status") or ""):find("^⚠ 계량기 펄스 없음 · 1D ") ~= nil end
check(gas._online == true, "가스 펄스 없음 → 경고일 뿐 온라인 그대로")
check(lastv("gas", "main", "signal") == "noPulse" and gas_warned(), "계량기 신호 = 펄스 없음(루틴 조건), 요약 글자 앞에 ⚠")
core.on_message(hub, CMD.PLUGINS, string.char(3, 3, 2))            -- 가스는 사용자가 멈춘 것(셋째 바이트)
check(driver._children["gas"]._online == false and lastv("gas", "main", "status") == "멈춤", "멈춘 센서 → 오프라인, 이유는 '멈춤'")
core.on_message(hub, CMD.PLUGINS, string.char(3, 3))               -- 다시 펄스 없음(멈춤 아님)
check(gas._online == true and gas_warned(), "멈춤이 풀리면 온라인, 경고는 그대로")
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4", 305, 0xFFFFFFFF, 0, 0, 0))
check(gas_warned() and lastv("gas", "main", "signal") == "noPulse", "경고 중 1분마다 오는 가스 값으로 경고가 지워지지 않음")
core.info_changed(nil, room)
check(room._online == false, "기기 정보가 바뀌어 다시 그려도 고장이면 오프라인")
core.on_message(hub, CMD.PLUGINS, string.char(3, 2))               -- 온습도계가 다시 응답(허브는 '정상' 을 값보다 먼저 보낸다)
core.on_message(hub, CMD.ROOM, string.pack("<i2i2", 2190, 3900))
check(room._online == true and lastv("gas", "main", "signal") == "noPulse", "온습도계 복구 → 온라인, 가스 경고는 그대로")
defer_timers = true
core.on_message(hub, CMD.PLUGINS, string.char(3, 3))               -- 3초 안에 풀리면 오프라인으로 남지 않는다
core.on_message(hub, CMD.PLUGINS, string.char(3, 2))
run_timers()
defer_timers = false
check(room._online == true, "오프라인 예약 뒤 3초 안에 풀림 → 온라인 유지")
core.on_message(hub, CMD.PLUGINS, string.char(3, 0))               -- 펄스가 다시 옴
check(gas._online == true and lastv("gas", "main", "signal") == "normal" and not gas_warned(), "펄스가 다시 옴 → 신호 정상, ⚠ 없어짐")
core.on_message(hub, CMD.PLUGINS, string.char(2, 1))               -- 온습도계 플러그인을 뺌: 기기 삭제(고장 비트는 의미 없음)
check(deleted["room"] and driver._children["gas"] ~= nil, "플러그인을 빼면 그 기기만 삭제")

print("[26] 가스 요금: 이번 달(청구일 리셋) + 지난 하루, 설정 기본값(서울)")
reset()
-- 수동 기본값(SD07 경기 중부1 코원): vcf 0.9939, 열량 42.576, 단가 22.6226, 기본료 1250, 부가세 10%
local function won(m3, with_base)
  local w = m3 * 0.9939 * 42.576 * 22.6226
  if with_base then w = w + 1250 end
  return math.floor(w * 1.1 + 0.5)
end
-- 첫 메시지: 누적 50㎥(reading=50000L), 지난 하루 5㎥. 이번 달 기준이 50 으로 잡혀 사용량 0.
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4", 1000, 50000, 0, 5000, 0))
core.info_changed(nil, driver._children["gas"])   -- 설정변경 경로로 중복 캐시 비우고 다시 계산(앞 시나리오 값과 겹침 방지)
check(lastv("gas", "main", "period").value == 0, "첫 값 → 이번 달 사용량 0(기준 설정)")
check(lastv("gas", "main", "month").value == won(0, true), "이번 달 = 기본료만 = " .. won(0, true) .. "원")
check(lastv("gas", "main", "today").value == won(5, false), "지난 하루(5㎥) = " .. won(5, false) .. "원")
check(lastv("gas", "main", "day").value == 5 and lastv("gas", "main", "hour").value == 0, "하루·한 시간 사용량은 숫자(㎥)로")
check(lastv("gas", "main", "flow").value == 0, "유량은 숫자(㎥/h)로 — 안 쓰면 0")
check(lastv("gas", "main", "dayText") == "5.00" and lastv("gas", "main", "hourText") == "0.00" and lastv("gas", "main", "flowText") == "0.00"
  and lastv("gas", "main", "periodText") == "0.00", "화면용 글자는 소수 둘째 자리까지(5.00)")
check(lastv("gas", "main", "month").unit == "원" and lastv("gas", "main", "monthRange").value.maximum == 50000
  and lastv("gas", "main", "todayRange").value.maximum == 10000, "경고 기준이 없으면 막대 끝값은 값이 들어가는 둥근 수(5만원 · 1만원)")
-- 둘째 메시지: 누적 53㎥ → 이번 달 3㎥, 지난 하루 2㎥
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4", 1300, 53000, 0, 2000, 0))       -- 300펄스 = 3㎥
check(lastv("gas", "main", "period").value == 3, "이번 달 사용량 = 53-50 = 3㎥")
check(lastv("gas", "main", "month").value == won(3, true), "이번 달 청구액 = 기본료+3㎥ = " .. won(3, true) .. "원")
check(lastv("gas", "main", "today").value == won(2, false), "지난 하루(2㎥)는 월 누적과 무관 = " .. won(2, false) .. "원")
-- 계량기 숫자 맞추기: 펄스는 그대로인데 지침만 크게 바뀜(관리 화면에서 실제 계량기 숫자를 넣음) → 이번 달 사용량은 이어진다
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4", 1300, 1694230, 0, 2000, 0))    -- 같은 펄스 수, 지침 1694.23 으로
check(lastv("gas", "main", "period").value == 3, "지침을 맞춰도 이번 달 사용량은 3㎥ 그대로(1644㎥ 가 되지 않음)")
check(lastv("gas", "main", "month").value == won(3, true), "이번달 예상 금액도 그대로")
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4", 1350, 1694730, 0, 2000, 0))    -- 그 뒤 50펄스(0.5㎥) 사용
check(lastv("gas", "main", "period").value == 3.5, "맞춘 뒤의 사용은 그대로 더해짐(3.5㎥)")
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4", 1350, 20000, 0, 2000, 0))      -- 지침을 낮춰 다시 맞춤
check(lastv("gas", "main", "period").value == 3.5, "낮춰 맞춰도 이번 달 사용량 유지(음수·0 이 되지 않음)")
-- "차이를 사용량에 넣음"으로 맞춘 경우: 허브가 펄스 수도 같이 늘려 보낸다 → 쓴 것으로 센다
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4I4", 1450, 21000, 0, 2000, 0, 10))  -- +100펄스, 지침 +1㎥
check(lastv("gas", "main", "period").value == 4.5, "펄스와 지침이 같이 늘면 사용량에 들어감(4.5㎥)")
-- 1펄스 = 100 L 인 계량기(여섯째 값): 2펄스에 0.2㎥ — 맞추기로 오해하지 않는다
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4I4", 1452, 21200, 0, 2000, 0, 100))
check(lastv("gas", "main", "period").value == 4.7, "1펄스 양이 달라도(100 L) 쓴 만큼만 더해짐(4.7㎥)")
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4", 1452, 20000, 0, 2000, 0))        -- 뒤 시험을 위해 10 L 로 되돌림(옛 형식)
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4", 1350, 20000, 0, 2000, 0))
-- 허브를 초기화해 펄스 수와 지침이 함께 0 근처로 돌아가면 이번 달을 새로 센다
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4", 0, 0xFFFFFFFF, 0, 0, 0))
check(lastv("gas", "main", "period").value == 0, "허브 초기화(펄스 0) → 기준 재설정, 사용량 0(음수 아님)")
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4", 0, 10000, 0, 0, 0))            -- 이후 시험의 기준: 지침 10㎥
check(lastv("gas", "main", "period").value == 0, "초기화 뒤 지침을 맞춤 → 사용량 0 그대로")
-- 사용량 경고: 이번달 예상 금액이 설정 기준(원) 이상이면 경고. 기준 0(기본) = 끔
check(lastv("gas", "main", "alert") == "normal", "경고 기준 없음(0) → 정상")
local gasdev = driver._children["gas"]
gasdev.preferences = gasdev.preferences or {}
gasdev.preferences.gasWarnCost = won(3, true)                 -- 기준 = 3㎥ 썼을 때의 금액
core.info_changed(nil, gasdev)
check(lastv("gas", "main", "alert") == "normal", "기준 아래(사용량 0) → 정상")
check(not (lastv("gas", "main", "status") or ""):find("경고", 1, true), "정상일 때 요약 글자에 경고 없음")
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4", 300, 13000, 0, 3000, 0))  -- 기준 10 → 300펄스 → 이번 달 3㎥
check(lastv("gas", "main", "alert") == "warning", "이번달 예상 금액이 기준에 닿음 → 경고")
check((lastv("gas", "main", "status") or ""):find("⚠ 사용량 경고 · 1D 3.00㎥", 1, true), "요약 글자 앞에 경고 표시 (" .. tostring(lastv("gas", "main", "status")) .. ")")
gasdev.preferences.gasWarnCost = won(3, true) + 1
core.info_changed(nil, gasdev)
check(lastv("gas", "main", "alert") == "normal", "기준을 올리면 → 정상으로 돌아옴")
check(lastv("gas", "main", "monthRange").value.maximum == won(3, true) + 1, "막대 끝값 = 경고 기준")
gasdev.preferences.gasWarnCost = 0
gasdev.preferences.gasWarnDayCost = won(3, false)             -- 24시간 요금 기준: 지금 하루치(3㎥)와 같음
core.info_changed(nil, gasdev)
check(lastv("gas", "main", "alert") == "warning", "지난 24시간 요금이 기준에 닿음 → 경고")
check(lastv("gas", "main", "todayRange").value.maximum == won(3, false), "24시간 요금 막대 끝값 = 그 기준")
gasdev.preferences.gasWarnDayCost = won(3, false) - 100       -- 기준보다 많이 씀 → 끝값은 값에 맞춤
core.info_changed(nil, gasdev)
check(lastv("gas", "main", "todayRange").value.maximum == won(3, false), "기준을 넘으면 막대 끝값 = 지금 값")
gasdev.preferences.gasWarnDayCost = 0
core.info_changed(nil, gasdev)
check(lastv("gas", "main", "alert") == "normal", "24시간 기준을 끄면 → 정상")

print("[27] 허브가 중계한 요금 요소(CMD 0x09, 펌웨어 hub_tariff)로 계산")
reset()
local function won2(m3, with_base)      -- 중계값: 보정 0.99, 열량 43.0, 단가 20.0, 기본 1000
  local w = m3 * 0.99 * 43.0 * 20.0
  if with_base then w = w + 1000 end
  return math.floor(w * 1.1 + 0.5)
end
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4", 0, 100000, 0, 10000, 0))   -- 누적 100㎥, 하루 10㎥, 가스 기기 생성
core.on_message(hub, CMD.TARIFF, string.pack("<I4I4I4I4", 990000, 43000000, 20000000, 1000))  -- ×1e6, 기본료 원
core.info_changed(nil, driver._children["gas"])     -- 중복 캐시 비우고 중계값으로 다시 그림
check(lastv("gas", "main", "month").value == won2(0, true), "중계값으로 이번 달 = 기본료 " .. won2(0, true) .. "원(수동 1375 아님)")
check(lastv("gas", "main", "today").value == won2(10, false), "중계값으로 지난 하루(10㎥) = " .. won2(10, false) .. "원")
check(driver._children["gas"]._fields["api_heat"] == 43.0, "열량 중계값 캐시됨(42.576 수동 아님)")

print("[28] 보일러의 난방 방식·수온 설정은 표준 항목(thermostatMode · thermostatWaterHeatingSetpoint)")
reset()
links({ { 6, "a1b2c3" } })
state(6, "a1b2c3", F06)
state(6, "a1b2c3", F06:sub(1, 7) .. "1" .. F06:sub(9))          -- 난방 방식 = 실내(8번째 16진 글자)
check(lastv("ba1b2c3", "heat", "thermostatMode") == "heat", "난방 방식 실내 = 표준 thermostatMode 'heat'")
state(6, "a1b2c3", F06)
check(lastv("ba1b2c3", "heat", "thermostatMode") == "ondol", "난방 방식 온돌 = 표준 thermostatMode 'ondol'")
check(type(lastv("ba1b2c3", "hotwater", "heatingSetpoint")) == "table" and lastv("ba1b2c3", "hotwater", "heatingSetpoint").unit == "C",
  "수온 설정 = 표준 thermostatWaterHeatingSetpoint")
sent_cmds = {}
core.capability_handlers["thermostatMode"]["setThermostatMode"](nil, driver._children["ba1b2c3"], { component = "heat", args = { mode = "heat" } })
check(table.concat(sent_cmds, " | "):find("f06@a1b2c3 b4=1", 1, true), "난방 방식을 실내로 → b4=1")
sent_cmds = {}
core.capability_handlers["thermostatMode"]["setThermostatMode"](nil, driver._children["ba1b2c3"], { component = "heat", args = { mode = "cool" } })
check(#sent_cmds == 0, "표준 목록의 다른 값(cool 등)은 무시")

print("[29] 온도·습도는 값이 그대로여도 20분마다 다시 보낸다(그래프가 끊기지 않게)")
reset()
local function room_events(attr)
  local n, forced = 0, 0
  for _, e in ipairs(emitted) do
    if e.key == "room" and e.attr == attr then n = n + 1; if e.state_change then forced = forced + 1 end end
  end
  return n, forced
end
local t0 = os.time()
local real_time2 = os.time
os.time = function() return t0 end
core.on_message(hub, CMD.ROOM, string.pack("<i2i2", 2250, 4000))
core.info_changed(nil, driver._children["room"])                   -- 앞 시나리오의 중복 캐시를 비우고 지금 값으로
local n0 = room_events("humidity")
os.time = function() return t0 + 19 * 60 end
core.on_message(hub, CMD.ROOM, string.pack("<i2i2", 2250, 4000))
check(room_events("humidity") == n0, "19분 뒤 같은 값 → 다시 보내지 않음")
os.time = function() return t0 + 21 * 60 end
core.on_message(hub, CMD.ROOM, string.pack("<i2i2", 2250, 4000))
local n1, f1 = room_events("humidity")
check(n1 == n0 + 1 and f1 == 1, "21분 뒤 같은 값 → 한 번 다시 보냄(state_change)")
local _, ft = room_events("temperature")
check(ft == 1, "온도도 같이 다시 보냄")
core.on_message(hub, CMD.ROOM, string.pack("<i2i2", 2250, 4000))
check(room_events("humidity") == n1, "바로 다음 값은 다시 보내지 않음(20분을 새로 센다)")
os.time = function() return t0 + 30 * 60 end
local nt = room_events("temperature")
core.on_message(hub, CMD.ROOM, string.pack("<i2i2", 2250, 4100))
local n2, f2 = room_events("humidity")
check(n2 == n1 + 1 and f2 == 1, "값이 바뀌면 바로 보냄(보통 이벤트)")
local nt2, ft2 = room_events("temperature")
check(nt2 == nt + 1 and ft2 == 2, "습도만 바뀌어도 온도를 같이 보냄(state_change)")
core.on_message(hub, CMD.ROOM, string.pack("<i2i2", 2310, 4100))
local n3, f3 = room_events("humidity")
check(n3 == n2 + 1 and f3 == 2 and room_events("temperature") == nt2 + 1, "온도만 바뀌어도 습도를 같이 보냄")
core.on_message(hub, CMD.ROOM, string.pack("<i2i2", 2400, -32768))     -- 습도가 없는 센서(MPL3115A2)
check(room_events("humidity") == n3 and room_events("temperature") == nt2 + 2, "습도 없는 센서 → 온도만 보냄")
os.time = real_time2

print("[30] 검침일: 전달 검침일 다음 날 ~ 이번 달 검침일이 한 달, 말일(31)이면 1일 ~ 말일")
reset()
local real_date, fake_day = os.date, nil
os.date = function(fmt, t)                       -- 오늘 날짜만 바꾼다(다른 시각 계산은 그대로)
  if fmt == "*t" and t == nil and fake_day then return fake_day end
  return real_date(fmt, t)
end
local function on_day(y, m, d) fake_day = { year = y, month = m, day = d, hour = 12, min = 0, sec = 0 } end
local function gas_at(count) core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4I4", count, count * 10, 0, 0, 0, 10)) end
on_day(2026, 11, 22)
gas_at(5000)                                      -- 가스 기기 생성, 지침 50㎥
local gdev = driver._children["gas"]
gdev.preferences = { gasBillDay = 8 }
gdev:set_field("gas_month_tag", "2026-11", { persist = true })     -- 옛 규칙이 적어 둔 태그(11/8 에 시작한 기간)
gdev:set_field("gas_month_base", 40, { persist = true })
gdev:set_field("gas_tag_v", nil, { persist = true })               -- 새 규칙으로 옮기기 전의 상태
core.info_changed(nil, gdev)
check(lastv("gas", "main", "period").value == 10, "옛 규칙의 태그를 이어받음: 기준 40 그대로 → 이번 달 10㎥(0 으로 돌아가지 않음)")
check(gdev:get_field("gas_month_tag") == "2026-12", "태그는 새 규칙(끝나는 달 = 12월)으로 바뀜")
on_day(2026, 12, 8); gas_at(5300)
check(lastv("gas", "main", "period").value == 13, "검침일 당일(12/8)까지는 같은 달 몫 → 13㎥")
on_day(2026, 12, 9); gas_at(5400)
check(lastv("gas", "main", "period").value == 0, "검침일 다음 날(12/9)부터 새 달 → 0 부터")
check(gdev:get_field("gas_month_tag") == "2027-01", "12/9 ~ 1/8 은 1월 몫")
-- 말일
gdev.preferences = { gasBillDay = 31 }
on_day(2026, 11, 29); gas_at(5500)
check(gdev:get_field("gas_month_tag") == "2026-11", "말일: 11/29 는 11월 몫")
on_day(2026, 11, 30); gas_at(5600)
check(lastv("gas", "main", "period").value == 1, "말일: 11월 30일(그 달 마지막 날)까지 11월 → 1㎥")
on_day(2026, 12, 1); gas_at(5700)
check(lastv("gas", "main", "period").value == 0 and gdev:get_field("gas_month_tag") == "2026-12", "말일: 12/1 부터 12월 → 0 부터")
on_day(2027, 2, 28); gas_at(5800)
check(gdev:get_field("gas_month_tag") == "2027-02", "말일: 2월은 28일이 마지막 날")
on_day(2027, 3, 1); gas_at(5900)
check(gdev:get_field("gas_month_tag") == "2027-03", "말일: 3/1 은 3월 몫")
-- 허브가 알려 준 검침일이 드라이버 설정보다 먼저
gdev.preferences = { gasBillDay = 8 }
on_day(2027, 3, 20)
core.on_message(hub, CMD.TARIFF, string.pack("<I4I4I4I4I4", 993900, 42576000, 22622600, 1250, 31))
check(gdev:get_field("gas_month_tag") == "2027-03", "허브가 검침일 = 말일로 알려 주면 그 값을 씀(3/20 → 3월 몫, 설정 8일이면 4월)")
os.date = real_date

print("[31] 허브의 동작 모드: 모드는 프록시·단독 둘, 지금 동작은 상태 글로")
reset()
local MODE = 0x0b
local function mode_msg(m, fb, since, why, rc)
  return string.char(m, fb, 1) .. string.pack("<I4", since or 0) .. (why and string.char(why, rc or 0) or "")
end
core.on_message(hub, MODE, mode_msg(0, 0))
check(hub._caps.main and hub._caps.main.mode and hub._caps.main["connectbook02461.roomStatus"]
  and hub._caps.main["connectbook02461.boilerStatus"], "모드를 알려 주는 펌웨어: 상태·모드 칸이 생김")
check(hub._caps.chip ~= nil, "허브 온도 구역의 구성도 같이 보냄(지워지지 않게)")
check(lastv("hub", "main", "mode") == "프록시", "프록시 모드로 보임")
check(lastv("hub", "main", "status") == "프록시 모드", "상태 글 = 프록시 모드")
local args = lastv("hub", "main", "supportedArguments")
check(#args == 2 and args[1] == "프록시" and args[2] == "단독", "고를 수 있는 것은 프록시·단독 둘")
check(#lastv("hub", "main", "supportedModes") == 2, "모드 값도 둘뿐")
local function count_mode() local n = 0
  for _, e in ipairs(emitted) do if e.key == "hub" and e.attr == "mode" then n = n + 1 end end
  return n end
local n_mode = count_mode()
core.on_message(hub, MODE, mode_msg(0, 0))
check(count_mode() == n_mode, "같은 값을 다시 받으면 모드 이벤트를 또 내지 않음")
local real_time3 = os.time
local noon = 1795316400                               -- 2026-11-22 12:00 KST
os.time = function(t) if t then return real_time3(t) end return noon + 7 * 3600 end      -- 지금 = 같은 날 19:00
core.on_message(hub, MODE, mode_msg(0, 1, noon + 6 * 3600 + 25 * 60))
check(lastv("hub", "main", "mode") == "프록시", "폴백 중에도 모드는 정해 둔 대로 프록시")
check(lastv("hub", "main", "status") == "임시 단독 모드", "기기 목록 글은 짧게(까닭 없이)")
check(lastv("hub", "main", "roomStatus") == "단독 모드 (18:25부터 린나이 서버 접속 불가)", "상세 글에 언제부터 서버에 닿지 않는지")
core.on_message(hub, MODE, mode_msg(0, 1, noon - 20 * 3600))
check(lastv("hub", "main", "roomStatus") == "단독 모드 (11/21 16:00부터 린나이 서버 접속 불가)", "다른 날에 시작됐으면 날짜도")
core.on_message(hub, MODE, mode_msg(0, 1, 0))
check(lastv("hub", "main", "roomStatus") == "단독 모드 (린나이 서버 접속 불가)", "시각을 모르면 시각 없이")
core.on_message(hub, MODE, string.char(0, 1, 1))
check(lastv("hub", "main", "roomStatus") == "단독 모드 (린나이 서버 접속 불가)", "시각이 없는 짧은 메시지도 받음")
-- 까닭을 구체적으로: 연결 안 됨 / 보안 연결 실패 / 로그인 거절(코드) / 응답 없음 / 서버가 끊음 / 집 Wi-Fi
local when = noon + 6 * 3600 + 25 * 60
local whys = { "집 Wi-Fi 끊김", "린나이 서버 연결 안 됨", "린나이 서버 보안 연결 실패", "린나이 서버 로그인 거절(코드 5)",
  "린나이 서버 로그인 응답 없음", "린나이 서버가 연결을 끊음", "린나이 서버 주소 조회 실패" }
local why_ok = true
for w, text in ipairs(whys) do
  core.on_message(hub, MODE, mode_msg(0, 1, when, w, 5))
  if lastv("hub", "main", "roomStatus") ~= "단독 모드 (18:25부터 " .. text .. ")" then why_ok = false end
  if lastv("hub", "main", "status") ~= "임시 단독 모드" then why_ok = false end
end
check(why_ok, "상세 글에 닿지 않는 까닭을 구체적으로(목록 글은 그대로 짧게)")
core.on_message(hub, MODE, mode_msg(0, 1, when, 0, 0))
check(lastv("hub", "main", "roomStatus") == "단독 모드 (18:25부터 린나이 서버 접속 불가)", "까닭을 모르면 '접속 불가'")
os.time = real_time3
core.on_message(hub, MODE, mode_msg(1, 0))
check(lastv("hub", "main", "mode") == "단독" and lastv("hub", "main", "status") == "단독 모드"
  and lastv("hub", "main", "roomStatus") == "단독 모드", "단독 모드로 보임(목록·상세 같은 글)")
sent_cmds = {}
core.capability_handlers["mode"]["setMode"](nil, hub, { component = "main", args = { mode = "프록시" } })
check(last_cmd_has("hub mode=proxy"), "프록시를 고르면 hub mode=proxy")
core.capability_handlers["mode"]["setMode"](nil, hub, { component = "main", args = { mode = "단독" } })
check(last_cmd_has("hub mode=standalone"), "단독을 고르면 hub mode=standalone")
core.on_message(hub, MODE, string.char(0))
check(lastv("hub", "main", "mode") == "단독", "너무 짧은 메시지는 무시")

print("[32] 가스: 허브가 이번 청구 기간 사용량·청구액을 보내면 그 값을 쓴다(관리 화면과 같은 숫자)")
reset()
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4I4", 1000, 50000, 0, 5000, 0, 10))
core.info_changed(nil, driver._children["gas"])
check(lastv("gas", "main", "period").value == 0, "허브 값이 없으면(옛 펌웨어) 드라이버가 스스로 센다 → 첫 값 0")
core.on_message(hub, CMD.GAS, string.pack("<I4I4I4I4I4I4I4I4", 1001, 50010, 0, 5000, 0, 10, 9140, 11004))
check(lastv("gas", "main", "period").value == 9.14 and lastv("gas", "main", "periodText") == "9.14", "이번달 사용량 = 허브 값 9.14㎥")
check(lastv("gas", "main", "month").value == 11004, "이번달 예상 금액 = 허브 값 11004원")

print("[33] 모델코드(0x0d)로 온수·난방수 온도 표를 고른다(린나이 앱과 같은 표)")
reset()
local function models(list)    -- {{6,"c0ffee",0x23}}
  local b = {} for _, e in ipairs(list) do b[#b + 1] = string.char(e[1]) .. mac3(e[2]) .. string.char(e[3]) end
  msg(CMD.MODELS, table.concat(b))
end
local function hw_set(dev, v)
  sent_cmds = {}
  core.capability_handlers["thermostatWaterHeatingSetpoint"]["setHeatingSetpoint"](nil, driver._children[dev], { component = "hotwater", args = { setpoint = v } })
end
local function heat_set(dev, v)
  sent_cmds = {}
  core.capability_handlers["thermostatHeatingSetpoint"]["setHeatingSetpoint"](nil, driver._children[dev], { component = "heat", args = { setpoint = v } })
end
links({ { 6, "c0ffee" } })
state(6, "c0ffee", F06)
state(6, "c0ffee", F06)                                   -- 첫 상태는 기기를 만들고, 둘째부터 값이 그려진다
local r = lastv("bc0ffee", "hotwater", "heatingSetpointRange")
check(r and r.value.minimum == 35 and r.value.maximum == 60 and r.value.step == 1, "모델코드를 모르면(옛 펌웨어) 실측 표 35~60")
hw_set("bc0ffee", 48)
check(last_cmd_has("f06@c0ffee d1=2F,df=0"), "모를 때 60 에서 48 로 내리면 → 47(실측 표에 48·49 없음, 내리는 쪽으로)")
models({ { 6, "c0ffee", 0x23 } })
r = lastv("bc0ffee", "hotwater", "heatingSetpointRange")
check(r.value.minimum == 35 and r.value.maximum == 60 and r.value.step == 1, "우리 집 보일러 23: 온수 16단 35~60")
r = lastv("bc0ffee", "heat", "heatingSetpointRange")
check(r.value.minimum == 40 and r.value.maximum == 85, "보일러 23: 온돌 22단 40~85(35 없음)")
heat_set("bc0ffee", 36)
check(last_cmd_has("f06@c0ffee d2=28"), "23 에서 온돌 36 → 40(35 는 표에 없음)")
models({ { 6, "c0ffee", 0x03 } })
r = lastv("bc0ffee", "hotwater", "heatingSetpointRange")
check(r.value.minimum == 35 and r.value.maximum == 60 and r.value.step == 0.5, "보일러 03: 온수 22단 — 0.5 단위")
hw_set("bc0ffee", 37.5)
check(last_cmd_has("f06@c0ffee d1=25,df=5"), "37.5 → d1=25(37), df=5")
hw_set("bc0ffee", 43.5)
check(last_cmd_has("f06@c0ffee d1=2B,df=0"), "43.5 는 22단 표에 없음 → 60 에서 내리는 쪽으로 43")
state(6, "c0ffee", F06:sub(1, 10) .. "255" .. F06:sub(14))
check(lastv("bc0ffee", "hotwater", "heatingSetpoint").value == 37.5, "상태의 소수 자리(니블 12) → 37.5")
models({ { 6, "c0ffee", 0x06 } })
r = lastv("bc0ffee", "heat", "heatingSetpointRange")
check(r.value.minimum == 35 and r.value.maximum == 85, "보일러 06: 온돌 23단 35~85")
r = lastv("bc0ffee", "hotwater", "heatingSetpointRange")
check(r.value.minimum == 30 and r.value.maximum == 60 and r.value.step == 0.5, "보일러 06: 온수 27단 30~60, 0.5 단위")
models({ { 6, "c0ffee", 0x07 } })
r = lastv("bc0ffee", "hotwater", "heatingSetpointRange")
check(r.value.minimum == 1 and r.value.maximum == 3, "표에 없는 보일러 모델(07): 앱처럼 온수 3단(1~3)")
hw_set("bc0ffee", 2)
check(last_cmd_has("f06@c0ffee d1=02,df=0"), "3단 모델: 2단 → d1=02")
print("     각방 조절기만 있는 집(가상 보일러): 각방 모델코드")
reset()
links({ { 9, "beef01" } })
state(9, "beef01", F09)
state(9, "beef01", F09)
models({ { 9, "beef01", 0x41 } })
r = lastv("vbeef01", "hotwater", "heatingSetpointRange")
check(r and r.value.minimum == 1 and r.value.maximum == 7, "각방 41: 온수 7단")
models({ { 9, "beef01", 0x22 } })
r = lastv("vbeef01", "hotwater", "heatingSetpointRange")
check(r.value.minimum == 35 and r.value.step == 0.5, "각방 22: 온수 22단(0.5 단위)")
state(9, "beef01", F09:sub(1, 98) .. "255" .. F09:sub(102))
check(lastv("vbeef01", "hotwater", "heatingSetpoint").value == 37.5, "각방 상태의 소수 자리(니블 100) → 37.5")
hw_set("vbeef01", 38.5)
check(last_cmd_has("f09@beef01 d1=26,df=5"), "가상 보일러 38.5 → f09 d1=26,df=5")
local vcaps0 = driver._children["vbeef01"]._caps or {}
check(vcaps0.heat and vcaps0.heat["thermostatHeatingSetpoint"], "보통 각방 모델: 난방수 설정 있음")
models({ { 9, "beef01", 0xff } })
local vcaps1 = driver._children["vbeef01"]._caps or {}
check(vcaps1.heat and not vcaps1.heat["thermostatHeatingSetpoint"], "각방 ff(온수 전용): 난방수 설정을 거둠")
heat_set("vbeef01", 60)
check(#sent_cmds == 0, "ff 에 난방수 명령은 보내지 않음")
msg(CMD.MODELS, string.char(9) .. mac3("beef01"))
check(true, "짧은 메시지는 무시(오류 없음)")

print("[가스 요금] 보일러 카드: 허브의 이번 기간 값(0x0f)")
reset()
links({ { 6, "a1b2c3" }, { 9, "d4e5f6" } })
state(6, "a1b2c3", F06)
state(9, "d4e5f6", F09)
local bcaps = driver._children["ba1b2c3"]._caps or {}
check(not bcaps.gas, "값이 오기 전엔 '가스 요금' 구역 없음")
msg(0x0f, string.char(2) .. string.pack("<I4I4", 37000, 48720))
bcaps = driver._children["ba1b2c3"]._caps or {}
check(bcaps.gas and bcaps.gas["connectbook02461.boilerGasBill"], "0x0f 받음 → '가스 요금' 구역을 내놓음")
check(lastv("ba1b2c3", "gas", "usageText") == "37 ㎥ · 보일러 누계", "보일러 기준: 정수 ㎥ + 기준 글자")
check(lastv("ba1b2c3", "gas", "usage").value == 37, "사용량 숫자(루틴 조건용) 37 ㎥")
check(lastv("ba1b2c3", "gas", "cost").value == 48720, "예상 요금 48720 원")
check(lastv("ba1b2c3", "gas", "costRange").value.maximum == 50000, "막대 끝값 5만(값이 들어가는 둥근 수)")
msg(0x0f, string.char(1) .. string.pack("<I4I4", 123456, 160300))
check(lastv("ba1b2c3", "gas", "usageText") == "123.46 ㎥ · 계량기", "계량기 기준: 소수 둘째 자리")
check(lastv("ba1b2c3", "gas", "costRange").value.maximum == 200000, "16만 원 → 막대 끝값 20만")
msg(0x0f, string.char(1) .. string.pack("<I4I4", 123456, 260300))
check(lastv("ba1b2c3", "gas", "costRange").value.maximum == 500000, "26만 원 → 막대 끝값 50만")
check(not (driver._children["vd4e5f6"] and (driver._children["vd4e5f6"]._caps or {}).gas), "가상 보일러에는 안 붙음")
msg(0x0f, string.char(0))
bcaps = driver._children["ba1b2c3"]._caps or {}
check(not bcaps.gas, "[0] → 구역을 거둠")
msg(0x06, string.pack("<I4", 0x7FFFFFFF & ~(1 << 13)))
msg(0x0f, string.char(2) .. string.pack("<I4I4", 1000, 2000))
bcaps = driver._children["ba1b2c3"]._caps or {}
check(not bcaps.gas, "노출 설정에서 '가스 요금'(비트 13)을 끄면 안 보임")

print(string.format("\n== %d통과 / %d실패 ==", pass, fail))
os.exit(fail == 0 and 0 or 1)

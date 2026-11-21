-- LYNNAI 허브 공통 로직(Zigbee 드라이버와 LAN 드라이버가 같이 쓴다).
--
-- 허브(ESP32/NAS)는 보일러(f06)·각방 조절기(f09)의 클라우드 연결을 중계하면서 상태를 엿보고 아래로 보낸다.
-- 기기는 종류(f06/f09)가 아니라 MAC 끝 3바이트(instance, 6 hex)로 식별한다 → 보일러 N대·써머스탯 N개.
--   허브 → ST  0x01 상태 [종류 6|9][MAC끝3(3B)][EPC70 블롭]
--              0x02 연결 ([종류][MAC끝3]) * n  (목록에 없으면 오프라인)
--              0x03 온습도 int16 x2   0x04 가스 u32 x5   0x05 플러그인 비트(+고장 비트)   0x06 노출 u32
--   ST → 허브  0x00 명령 "f06@<mac> b1=1" / "f09@<mac> a0=7,d4=10" / "refresh" (여러 개는 ";")
--
-- 자식 기기 키: 보일러 = "b"..mac, 존 = "t"..mac.."_"..slot. 센서는 "room"/"gas"(허브 1개당 하나).
-- 짝(어느 f09가 어느 f06에 붙나)은 1단계에선 '보일러가 하나뿐이면 암묵 연동'. 2단계에서 관리 화면 수동 지정.
local capabilities = require "st.capabilities"
local log = require "log"

local CMD_STATE, CMD_LINKS, CMD_ROOM, CMD_GAS, CMD_PLUGINS, CMD_EXPOSE, CMD_PAIRS = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
local CMD_FORGET = 0x08
local CMD_TARIFF = 0x09
local KIND_F06, KIND_F09 = 6, 9

-- 한 f09 안의 방 슬롯(7바이트, 슬롯 s 는 byte[7s]). 상태 0x02=켜짐, 0x40=난방 중. 명령 선택자 a0=슬롯+1.
-- 기기가 리포트하는 슬롯만 보인다(decode_rooms 가 거른다). 이름은 슬롯(a0)대로 방1~방6/거실.
-- 슬롯 간격 7바이트(stat=base, cur=base+3, set=base+4). z1~z3 은 기존 그대로(기기 안 바뀌게).
local ZONES = {
  z1 = { label = "방1", stat = 0,  cur = 3,  set = 4,  a0 = 1, max = 30, n = 1 },
  z2 = { label = "방2", stat = 7,  cur = 10, set = 11, a0 = 2, max = 30, n = 2 },
  z3 = { label = "거실", stat = 42, cur = 45, set = 46, a0 = 7, max = 30, n = 3 },
  z4 = { label = "방3", stat = 14, cur = 17, set = 18, a0 = 3, max = 30, n = 4 },
  z5 = { label = "방4", stat = 21, cur = 24, set = 25, a0 = 4, max = 30, n = 5 },
  z6 = { label = "방5", stat = 28, cur = 31, set = 32, a0 = 5, max = 30, n = 6 },
  z7 = { label = "방6", stat = 35, cur = 38, set = 39, a0 = 6, max = 30, n = 7 },
}
local ZONE_ORDER = { "z1", "z2", "z3", "z4", "z5", "z6", "z7" }

local SWITCH = {
  power = { pos = 0, code = "80" },
  heat = { pos = 2, code = "b1" },
  hotwater = { pos = 3, code = "d7" },
  preheat = { pos = 5, code = "b6" },
}
-- 외출은 보일러(f06 b3) 가 아니라 각방 조절기의 방마다(f09 a0=<방>,b3=1) — f06 b3 는 실측으로 안 먹힘.
-- 보일러의 '외출' = 그 보일러에 붙은 방이 모두 외출(update_outing), 외출 명령은 방마다 보낸다(outing_cmds).
local SWITCH_COMPONENT = { power = "main", hotwater = "hotwater", preheat = "preheat" }
-- 화면에는 '난방' 스위치와 '외출' 스위치로 나온다(둘 중 하나만 켜짐). 붙은 방이 없는 보일러는 외출 스위치를 내놓지 않는다
-- (외출을 걸 방이 없다 — f06 b3 는 안 먹힘).
local HEAT_MODES = { [0] = "온돌", [1] = "실내" }
local ONDOL_TEMPS = { 35, 40, 45 }
for t = 46, 60 do ONDOL_TEMPS[#ONDOL_TEMPS + 1] = t end
for _, t in ipairs({ 65, 70, 75, 80, 85 }) do ONDOL_TEMPS[#ONDOL_TEMPS + 1] = t end
local ROOM_TEMPS = {}
for t = 5, 40 do ROOM_TEMPS[#ROOM_TEMPS + 1] = t end
local HW_TEMPS = {}
for t = 35, 55 do HW_TEMPS[#HW_TEMPS + 1] = t end
HW_TEMPS[#HW_TEMPS + 1] = 60
local RANGE = { ["온돌"] = { 35, 85 }, ["실내"] = { 5, 40 } }

local status_cap = capabilities["connectbook02461.boilerStatus"]
local room_cap = capabilities["connectbook02461.roomStatus"]
-- 보일러의 조작 항목은 표준 capability 를 쓴다(앱이 표준 항목 전용 모양으로 그린다 — 린나이 공식 연동과 같은 구성):
--   난방 방식 = thermostatMode("ondol" 온돌 / "heat" 실내), 수온 설정 = thermostatWaterHeatingSetpoint
local tm_cap = capabilities.thermostatMode
local hw_cap = capabilities.thermostatWaterHeatingSetpoint
local TM_OF = { ["온돌"] = "ondol", ["실내"] = "heat" }
local cost_cap = capabilities["connectbook02461.gasCost"]
local usage_cap = capabilities["connectbook02461.gasUse"]     -- 사용량 카드: 24시간 · 1시간 · 이번달
local flow_cap = capabilities["connectbook02461.gasFlow"]     -- 지금 유량

local driver

-- 허브(부모)별 상태. boilers[mac]=해독값, thermos[mac]={z1=..}, online={["6:mac"]=true}
local hubs = {}
local function hub_state(parent)
  hubs[parent.id] = hubs[parent.id] or { boilers = {}, thermos = {}, online = {} }
  return hubs[parent.id]
end

-- 노출(펌웨어 hub_expose.h). 0x06 = [u32 마스크] + ([종류][MAC끝3][숨김 비트])*n
--   마스크: 종류별 컨트롤(보일러 조절기 1~7, 방 8~10·12 예약) + 허브 칩 온도(11, Zigbee 허브 기기) + 플러그인 기기(29 온습도, 30 가스).
--   숨김 비트(기기별): bit0 = 기기 자신(보일러 조절기), bit1~7 = 온도 조절기의 방 z1~z7(ZONES[id].n)
local ALL_VISIBLE = 0x7FFFFFFF   -- 펌웨어 ALL_BITS 와 같게
local BIT = { power = 1, heat_mode = 2, heat_type = 3, heat_temp = 4, hw = 5, hw_temp = 6, preheat = 7,
  room_on = 8, room_set = 9, room_out = 10, hub_temp = 11, room_resv = 12, room = 29, gas = 30 }
local function parse_expose(s, raw)
  s.vis = string.unpack("<I4", raw)
  s.hidden = {}
  for i = 5, #raw - 4, 5 do
    s.hidden[raw:byte(i) .. ":" .. string.format("%02x%02x%02x", raw:byte(i + 1, i + 3))] = raw:byte(i + 4)
  end
end
local function vis_state(parent)
  local s = hub_state(parent)
  if not s.vis then
    local hex = parent:get_field("expose2")          -- 저장해 둔 마지막 0x06(16진) — 재시작 직후에도 같은 노출
    if type(hex) == "string" and #hex >= 8 then
      parse_expose(s, (hex:gsub("..", function(h) return string.char(tonumber(h, 16)) end)))
    else
      s.vis, s.hidden = ALL_VISIBLE, {}
    end
  end
  return s
end
local function visible(parent, bit) return ((vis_state(parent).vis >> bit) & 1) == 1 end
-- 이 기기(sub=0) / 이 방(sub=ZONES[id].n) 을 보이나
local function shown(parent, kind, mac, sub)
  local h = vis_state(parent).hidden[kind .. ":" .. mac] or 0
  return ((h >> sub) & 1) == 0
end

local function is_child(device) return device.parent_assigned_child_key ~= nil end
local function parent_of(device) return is_child(device) and device:get_parent_device() or device end

-- ------------------------------------------------------------------ 기기 키
local ROOM_KEY, GAS_KEY = "room", "gas"
local function bkey(mac) return "b" .. mac end
local function vbkey(mac) return "v" .. mac end       -- 가상 보일러(단독 써머스탯용)
local function zkey(mac, id) return "t" .. mac .. "_" .. id end
local function parse_key(key)
  local mac = key:match("^b(%x+)$")
  if mac then return "boiler", mac end
  local vmac = key:match("^v(%x+)$")
  if vmac then return "vboiler", vmac end
  local m, id = key:match("^t(%x+)_(z%d)$")
  if m then return "zone", m, id end
  if key == ROOM_KEY or key == GAS_KEY then return key end
end
-- 보일러 명령 대상 토픽 접두어: 실보일러 → f06@, 가상(단독) → 그 써머스탯의 f09@
local function boiler_prefix(kind, mac) return (kind == "vboiler" and "f09@" or "f06@") .. mac end

-- ------------------------------------------------------------------ 해독
local function tohex(s) return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end)) end

local function decode_boiler(blob)
  local raw = tohex(blob)
  if #raw < 32 then return nil end
  local function ch(i) return raw:sub(i + 1, i + 1) end
  local function hx(i, j) return tonumber(raw:sub(i + 1, j), 16) end
  local m = {}
  for k, s in pairs(SWITCH) do m[k] = ch(s.pos) == "1" end
  m.heat_mode = HEAT_MODES[ch(7) == "1" and 1 or 0]
  m.hw_set = hx(10, 12)
  m.ondol_set = hx(13, 15)
  m.room_set = hx(15, 17)
  m.heat_temp = m.heat_mode == "실내" and m.room_set or m.ondol_set
  -- 예약(읽기전용 표시용): 위치 40 reservationStatus, 41 reservationType(0 일반/1 24시간/2 패턴).
  -- 스펙 '위치'=문자열index+1 → ch(39)/ch(40). 명령은 안 보냄(코드 96 미검증).
  if #raw > 40 then
    m.reservation = ch(39) == "1"
    m.reservation_type = tonumber(ch(40)) or 0
  end
  return m
end

local function decode_rooms(blob)
  local out = {}
  for id, z in pairs(ZONES) do
    local st, cur, set = blob:byte(z.stat + 1), blob:byte(z.cur + 1), blob:byte(z.set + 1)
    if st and cur and set and (st ~= 0 or cur ~= 0) then    -- stat·현재온도 다 0 = 그 슬롯에 방 없음(안 리포트)
      -- stat 비트: 0x02 켜짐, 0x40 난방 중, 0x10 외출, 0x08 반복예약. stat 다음 바이트 = 저장된 예약 값(97)
      -- (실기기: 예약 켬 → 0x8b·다음 01 유지, 96=0 → 0x83·다음 01 그대로, 방 끔 → 0x81·다음 00)
      local resv = blob:byte(z.stat + 2) or 0
      out[id] = { on = (st & 0x02) ~= 0, heating = (st & 0x40) ~= 0, outing = (st & 0x10) ~= 0,
                  reserve = (st & 0x08) ~= 0, resv = resv, current = cur, target = set }
    end
  end
  return out
end

-- 단독 써머스탯용 '가상 보일러' 상태: f09 블롭에서 뽑을 수 있는 보일러 레벨 값.
-- 지금은 온수 설정(byte 49, 예전 실측) + 난방 중 여부(존 집계)만. 나머지(전원·모드·난방온도)는
-- 실제 단독 f09 로 자리 확인 필요 → 명령은 write-through, 표시는 best-effort.
local function decode_vboiler(blob, rooms)
  local vb = { heating = false }
  if #blob >= 50 then vb.hw_set = blob:byte(50) end
  for _, z in pairs(rooms or {}) do if z.heating then vb.heating = true end end
  return vb
end

-- 짝 보일러: 관리 화면에서 이 써머스탯에 지정한 보일러(2단계). 없으면 보일러가 하나뿐일 때 암묵 연동(1단계).
-- tmac 생략 시(보일러 자신 등) 암묵만.
local function the_boiler(parent, tmac)
  local s = hub_state(parent)
  local bmac = tmac and s.pairs and s.pairs[tmac]
  if bmac and s.boilers[bmac] ~= nil then return bmac, s.boilers[bmac] end
  if bmac then return nil, nil end        -- 짝을 '단독'으로 지정했거나 짝 보일러가 아직 없음
  local only, cnt = nil, 0
  for mac in pairs(s.boilers) do only = mac; cnt = cnt + 1 end
  if cnt == 1 then return only, s.boilers[only] end
  return nil, nil
end

-- 이 보일러(실 f06 / 가상 = 그 f09)에 붙은 써머스탯 mac 목록
local function thermos_of(parent, kind, mac)
  if kind == "vboiler" then return { mac } end
  local out = {}
  local s = hub_state(parent)
  for tmac in pairs(s.thermos) do                 -- 연결 목록을 받은 뒤엔 지금 연결된 조절기만(끊긴 것의 옛 상태는 빼고)
    if the_boiler(parent, tmac) == mac and (not s.linked or s.linked["9:" .. tmac]) then out[#out + 1] = tmac end
  end
  return out
end

local function outing_of(parent, kind, mac)
  local any, all = false, true
  for _, tmac in ipairs(thermos_of(parent, kind, mac)) do
    for _, z in pairs(hub_state(parent).thermos[tmac] or {}) do
      any = true
      if not z.outing then all = false end
    end
  end
  return any and all
end

local function update_outing(parent)
  local s = hub_state(parent)
  for bmac, m in pairs(s.boilers) do m.outing = outing_of(parent, "boiler", bmac) end
  for tmac, vb in pairs(s.vboilers or {}) do vb.outing = outing_of(parent, "vboiler", tmac) end
end

-- 재시작 직후 판단 막기: 연결 목록을 받았고, 목록의 기기가 모두 한 번씩 상태를 보냈나
-- (그 전엔 자동화·가상 보일러 만들기/지우기를 하지 않는다 — 일부만 보고 '전부 꺼짐'·'단독'으로 오판하지 않게)
-- 단, 목록에는 있는데 REPORT_WAIT_S 가 지나도록 상태를 못 보내는 기기는 더 기다리지 않는다(그 하나가 전체를 막지 않게).
local REPORT_WAIT_S = 120
local function all_reported(parent)
  local s = hub_state(parent)
  if not s.linked then return false end
  local now = os.time()
  for key in pairs(s.linked) do
    if not (s.seen and s.seen[key]) and now - ((s.link_since and s.link_since[key]) or now) < REPORT_WAIT_S then
      return false
    end
  end
  return true
end

-- ------------------------------------------------------------------ 허브로 명령
local send_hook
local SUFFIX = ""
local function send_text(parent, text)
  log.info("→ 허브 명령: " .. text)
  send_hook(parent, text)
end

local function snap(v, allowed, cur)
  if cur and v > cur then
    for _, a in ipairs(allowed) do if a >= v then return a end end
  end
  if cur and v < cur then
    for i = #allowed, 1, -1 do if allowed[i] <= v then return allowed[i] end end
  end
  local best = allowed[1]
  for _, a in ipairs(allowed) do if math.abs(a - v) < math.abs(best - v) then best = a end end
  return best
end

-- ------------------------------------------------------------------ 화면 갱신
local sent = {}
-- 값 → 비교용 문자열(표는 키 순서대로 펼침 — 설정 범위처럼 값이 표인 이벤트도 중복 억제가 되게)
local function ser(v)
  if type(v) ~= "table" then return tostring(v) end
  local kv = {}
  for k, x in pairs(v) do kv[#kv + 1] = { tostring(k), x } end
  table.sort(kv, function(a, b) return a[1] < b[1] end)
  local out = {}
  for _, e in ipairs(kv) do out[#out + 1] = e[1] .. "=" .. ser(e[2]) end
  return "{" .. table.concat(out, ",") .. "}"
end
local function emit(device, comp, ev)
  if not device:supports_capability_by_id(ev.capability.ID, comp) then return end
  local key = table.concat({ device.id, comp, ev.capability.ID, ev.attribute.NAME }, "|")
  local val = ser(ev.value)
  if sent[key] == val then return end
  sent[key] = val
  device:emit_component_event(device.profile.components[comp], ev)
end

local function deg(v) return tonumber(v) and string.format("%d°C", math.floor(tonumber(v) + 0.5)) or "?" end
local function setpoint(v) return capabilities.thermostatHeatingSetpoint.heatingSetpoint({ value = v, unit = "C" }) end
local function sp_range(r)
  return capabilities.thermostatHeatingSetpoint.heatingSetpointRange({ value = { minimum = r[1], maximum = r[2], step = 1 }, unit = "C" })
end

local RES_LABEL = { [0] = "예약", [1] = "예약(24시간)", [2] = "예약(패턴)" }
local function main_text(m)
  local function t(v) return (tonumber(v) or 0) > 0 and (" " .. deg(v)) or "" end
  local function res(s) return m.reservation and (s .. " · " .. (RES_LABEL[m.reservation_type] or "예약")) or s end
  if not m.power then return res("꺼짐") end
  local heat
  if m.outing then heat = "외출"
  elseif m.heat then heat = m.heat_mode .. t(m.heat_temp)
  else heat = "꺼짐" end
  if m.hotwater then return res(heat .. " · 온수" .. t(m.hw_set)) end
  if heat == "꺼짐" then return res("대기") end
  return res(heat)
end

-- 예약(각방, f09): 96 = 예약 켜기/끄기, 97 = 예약 시간(1~24시간, 16진 2자리 — 12시간 = 0c). 0시간 예약은 없다
-- (97=00 을 보내면 예약이 풀리고 바로 난방한다 — 2026-11-22 린나이 앱 명령 캡처).
-- 예약을 걸면 기기가 그 방을 켜짐으로 바꾼다(켜는 명령을 따로 보내지 않는다 — 앱도 96·97 만 보낸다).
-- 예약 시간 슬라이더(1~24시간). 앱은 우리가 만든 슬라이더를 한 줄짜리(숫자 옆 슬라이더)로 그린다 — 표준
-- thermostatHeatingSetpoint 와 똑같은 구조로 만들어도 두 줄짜리(−/+ 버튼)가 되지 않았다(2026-11-22 시험).
local rh_cap = capabilities["connectbook02461.reserveHours"]
local function resv_hours(n)                                   -- 기억해 둔 값 → 1~24(없거나 0 이면 1)
  n = math.floor((tonumber(n) or 0) + 0.5)
  return n < 1 and 1 or n > 24 and 24 or n
end

local function zone_text(m, z)
  local what
  if z.outing then what = "외출"                                    -- 존별 외출(f09)
  elseif z.reserve then what = "예약 " .. resv_hours(z.resv) .. "시간"   -- 존별 예약(f09 96, 시간 97)
  elseif (m and (not m.power or not m.heat)) or not z.on then what = "꺼짐"
  else what = "난방 " .. deg(z.target) end
  return deg(z.current) .. " · " .. what
end

local last_text = {}
local function emit_text(device, text, is_zone)
  if last_text[device.id] == text then return end
  last_text[device.id] = text
  local ev = is_zone and room_cap.roomStatus(text, { state_change = true }) or status_cap.status(text, { state_change = true })
  device:emit_component_event(device.profile.components.main, ev)
end

-- 오프라인은 3초 뒤에(글자가 먼저 보이게). 그 사이 돌아오면(mark_online) 예약을 무른다.
local pending_off, off_seq = {}, 0
local function mark_online(device)
  pending_off[device.id] = nil
  device:online()
end
local function go_offline(device, is_zone, no_text)
  if not no_text then emit_text(device, "연결 끊김", is_zone) end
  off_seq = off_seq + 1
  local my = off_seq
  pending_off[device.id] = my
  driver:call_with_delay(3, function()
    if pending_off[device.id] == my then
      pending_off[device.id] = nil
      device:offline()
    end
  end)
end

-- 자동화(상태 기반, 양방향): 보일러에 붙은 방들의 on/off 집계가 바뀌는 '순간'에 보일러를 맞춘다.
--   · 일부 켜짐 → 전부 꺼짐  : 보일러 난방 끔 (보일러는 이 방향을 스스로 안 해줌)
--   · 전부 꺼짐 → 일부 켜짐  : 보일러 깨움(전원·외출해제·난방)
-- 상태로 들어오므로 ST 앱뿐 아니라 Rinnai 리모컨·스케줄로 방이 바뀌어도 동작한다. 전환 순간에만 1회라
-- 재시작(첫 관찰 씨딩)·사용자가 수동으로 보일러를 켜거나 끈 경우엔 되돌리지 않는다(안 싸움).
-- (ST 앱에서 방 켤 때 즉시 깨우는 건 on_switch 에 그대로 두고, 여기선 보조/비-ST 경로를 커버.)
local prev_all_off = {}        -- bmac -> 직전 '전부 꺼짐' 여부
local prev_set = {}            -- bmac -> 직전에 집계한 조절기 목록(바뀌면 기준을 다시 잡는다)
local function auto_boiler(parent, bmac)
  local s = hub_state(parent)
  local m = s.boilers[bmac]
  if not m or not all_reported(parent) then return end                  -- 재시작 직후 일부 상태로는 판단 안 함
  local has_thermo, all_off, set = false, true, {}
  for tmac, zones in pairs(s.thermos) do
    -- 지금 연결된 조절기만 센다(끊긴 조절기의 옛 '켜짐'으로 보일러를 깨우지 않게)
    if the_boiler(parent, tmac) == bmac and (not s.linked or s.linked["9:" .. tmac]) then
      if next(zones) == nil then return end                              -- 방 정보 없음 = 모름(꺼짐으로 치지 않음)
      for _, id in ipairs(ZONE_ORDER) do                                 -- 있던 방이 방금 안 보임(1~2번) = 모름: 판단 보류
        local a = s.absent and s.absent[zkey(tmac, id)]
        if a and a < 3 and s.had and s.had[zkey(tmac, id)] then return end
      end
      has_thermo = true
      set[#set + 1] = tmac
      for _, z in pairs(zones) do if z.on then all_off = false end end   -- z.on = 방 자신의 on/off
    end
  end
  if not has_thermo then prev_all_off[bmac] = nil; prev_set[bmac] = nil; return end   -- 붙은 방 없음 → 건드리지 않음
  table.sort(set)
  local sig = table.concat(set, ",")
  local was = prev_all_off[bmac]
  if prev_set[bmac] ~= sig then was = nil end                            -- 집계 대상이 바뀜(조절기가 붙거나 끊김) → 다시 씨딩
  prev_set[bmac] = sig
  prev_all_off[bmac] = all_off
  if was == nil then return end                                          -- 첫 관찰은 씨딩만(재시작 안전)
  if all_off and not was then                                            -- 전부 꺼짐
    if m.power and m.heat and not m.outing then
      log.info("모든 방 꺼짐 → 보일러 " .. bmac .. " 난방 끔")
      send_text(parent, "f06@" .. bmac .. " b1=0")
    end
  elseif not all_off and was then                                        -- 방 켜짐
    local list = {}
    if not m.power then list[#list + 1] = "f06@" .. bmac .. " 80=1" end
    if not m.heat then list[#list + 1] = "f06@" .. bmac .. " b1=1" end
    if #list > 0 then
      log.info("방 켜짐 → 보일러 " .. bmac .. " 깨움")
      send_text(parent, table.concat(list, ";"))
    end
  end
end

local apply_caps                       -- 전방 선언(아래 '보이는 컨트롤' 절에서 정의)

local function refresh_boiler(parent, mac)
  local child = parent:get_child_by_parent_assigned_key(bkey(mac))
  if not child then return end
  local s = hub_state(parent)
  if not s.online["6:" .. mac] then return go_offline(child) end
  local m = s.boilers[mac]
  if not m then return end
  mark_online(child)
  apply_caps(parent, child)            -- 붙은 방이 생기거나 없어지면 외출 스위치를 내놓거나 거둔다(바뀔 때만 실제로 적용)
  for key, comp in pairs(SWITCH_COMPONENT) do
    emit(child, comp, m[key] and capabilities.switch.switch.on() or capabilities.switch.switch.off())
  end
  -- 난방·외출은 스위치 둘로 보인다. 외출 중이면 난방 스위치는 꺼짐으로(보일러의 난방 비트는 그대로라 외출을 풀면 돌아온다)
  local on, off = capabilities.switch.switch.on, capabilities.switch.switch.off
  emit(child, "heat", (m.heat and not m.outing) and on() or off())
  emit(child, "outing", m.outing and on() or off())
  emit(child, "heat", tm_cap.supportedThermostatModes({ "ondol", "heat" }, { visibility = { displayed = false } }))
  emit(child, "heat", tm_cap.thermostatMode(TM_OF[m.heat_mode] or "ondol"))
  emit(child, "heat", sp_range(RANGE[m.heat_mode]))
  if m.heat_temp then emit(child, "heat", setpoint(m.heat_temp)) end
  emit(child, "hotwater", hw_cap.heatingSetpointRange({ value = { minimum = 35, maximum = 60, step = 1 }, unit = "C" }))
  if m.hw_set then emit(child, "hotwater", hw_cap.heatingSetpoint({ value = m.hw_set, unit = "C" })) end
  emit_text(child, main_text(m))
end

local function refresh_zone(parent, mac, id)
  local child = parent:get_child_by_parent_assigned_key(zkey(mac, id))
  if not child then return end
  local s = hub_state(parent)
  if not s.online["9:" .. mac] then return go_offline(child, true) end
  local z = s.thermos[mac] and s.thermos[mac][id]
  if not z then return end
  mark_online(child)
  local _, m = the_boiler(parent, mac)   -- 짝 보일러(지정 or 암묵)
  local heating = z.on and (not m or (m.power and m.heat))
  emit(child, "main", heating and capabilities.switch.switch.on() or capabilities.switch.switch.off())
  emit(child, "main", sp_range({ 10, ZONES[id].max }))
  emit(child, "main", setpoint(z.target))
  emit(child, "main", capabilities.temperatureMeasurement.temperature({ value = z.current, unit = "C" }))
  emit(child, "outing", z.outing and capabilities.switch.switch.on() or capabilities.switch.switch.off())  -- 존별 외출(f09 stat bit 0x10)
  emit(child, "reserve", z.reserve and capabilities.switch.switch.on() or capabilities.switch.switch.off())  -- 존별 예약(stat 0x08)
  -- 예약 시간: 예약 중이면 기기 값(다음 바이트)을 기억해 두고, 꺼져 있으면 기억한 값(없으면 1시간)을 보여 준다
  if z.reserve and (z.resv or 0) >= 1 then child:set_field("resv_iv", z.resv, { persist = true }) end
  emit(child, "reserve", rh_cap.hours({ value = resv_hours(child:get_field("resv_iv")), unit = "시간" }))
  emit_text(child, zone_text(m, z), true)
end

-- 단독(붙은 보일러도 짝도 없음) 써머스탯엔 가상 보일러를 둔다. 짝이 생기면 가상 보일러를 지운다.
local create_vboiler, refresh_vboiler, delete_child
local function is_standalone(parent, mac) return (the_boiler(parent, mac)) == nil end

local function refresh_all(parent)
  local s = hub_state(parent)
  for mac in pairs(s.boilers) do refresh_boiler(parent, mac) end
  for mac in pairs(s.thermos) do
    for _, id in ipairs(ZONE_ORDER) do refresh_zone(parent, mac, id) end
    local vchild = parent:get_child_by_parent_assigned_key(vbkey(mac))
    if not all_reported(parent) then
      refresh_vboiler(parent, mac)  -- 다 모이기 전엔 만들거나 지우지 않고 있는 것만 갱신
    elseif is_standalone(parent, mac) then
      create_vboiler(parent, mac)
      refresh_vboiler(parent, mac)
    elseif vchild then
      delete_child(vchild)        -- 실보일러에 연결됨 → 가상 보일러 불필요
    end
  end
end

-- ------------------------------------------------------------------ 자식 기기 만들기
local creating = {}             -- [parent.id|key] = 만들기 요청한 시각(만들어지는 동안 같은 요청을 또 보내지 않게)
local function create_child(parent, key, label, profile, model)
  if parent:get_child_by_parent_assigned_key(key) then creating[parent.id .. "|" .. key] = nil; return end
  local ck = parent.id .. "|" .. key
  if creating[ck] and os.time() - creating[ck] < 60 then return end
  creating[ck] = os.time()
  log.info("자식 기기 만들기: " .. key .. " " .. label)
  label = label .. SUFFIX
  driver:try_create_device({
    type = "EDGE_CHILD", label = label, profile = profile, parent_device_id = parent.id,
    parent_assigned_child_key = key, vendor_provided_label = label, manufacturer = "Lynnai", model = model,
  })
end

local function create_boiler(parent, mac) create_child(parent, bkey(mac), "보일러", "lynnai-boiler", "boiler") end
local function create_zone(parent, mac, id) create_child(parent, zkey(mac, id), ZONES[id].label, "lynnai-zone", "zone") end
function create_vboiler(parent, mac) create_child(parent, vbkey(mac), "보일러", "lynnai-boiler", "vboiler") end

function delete_child(child)
  log.info("기기 빼기: " .. tostring(child.parent_assigned_child_key))
  for ck in pairs(creating) do                 -- 지운 뒤 다시 보이면 곧바로 다시 만들 수 있게
    if ck:sub(-(#child.parent_assigned_child_key + 1)) == "|" .. child.parent_assigned_child_key then creating[ck] = nil end
  end
  if driver.try_delete_device then driver:try_delete_device(child.id) end
end

function refresh_vboiler(parent, mac)
  local child = parent:get_child_by_parent_assigned_key(vbkey(mac))
  if not child then return end
  local s = hub_state(parent)
  if not s.online["9:" .. mac] then return go_offline(child) end
  local vb = s.vboilers and s.vboilers[mac]
  mark_online(child)
  if vb and vb.hw_set and vb.hw_set > 0 then
    emit(child, "hotwater", hw_cap.heatingSetpointRange({ value = { minimum = 35, maximum = 60, step = 1 }, unit = "C" }))
    emit(child, "hotwater", hw_cap.heatingSetpoint({ value = vb.hw_set, unit = "C" }))
  end
  emit_text(child, vb and vb.heating and "난방 중" or "대기")
end

-- 켤 optional 능력(나머지는 숨김). key 로 보일러/존 판별.
local function wanted_caps(parent, key)
  local function pick(list)
    local out = {}
    for _, e in ipairs(list) do if visible(parent, e[1]) then out[#out + 1] = e[2] end end
    return out
  end
  local kind, mac, id = parse_key(key)
  if kind == "boiler" or kind == "vboiler" then
    local comps = {
      { "main", { { BIT.power, "switch" } } },
      { "heat", { { BIT.heat_mode, "switch" }, { BIT.heat_type, "thermostatMode" },
        { BIT.heat_temp, "thermostatHeatingSetpoint" } } },
      -- 외출 스위치는 외출을 걸 방이 붙어 있을 때만
      { "outing", #thermos_of(parent, kind, mac) > 0 and { { BIT.heat_mode, "switch" } } or {} },
      { "hotwater", { { BIT.hw, "switch" }, { BIT.hw_temp, "thermostatWaterHeatingSetpoint" } } },
      { "preheat", { { BIT.preheat, "switch" } } },
    }
    local out = {}
    for _, c in ipairs(comps) do
      local caps = pick(c[2])
      if #caps > 0 then out[#out + 1] = { c[1], caps } end
    end
    return "lynnai-boiler", out
  elseif kind == "zone" then
    local out = { { "main", pick({ { BIT.room_on, "switch" }, { BIT.room_set, "thermostatHeatingSetpoint" } }) } }
    local outing = pick({ { BIT.room_out, "switch" } })
    if #outing > 0 then out[#out + 1] = { "outing", outing } end
    local reserve = pick({ { BIT.room_resv, "switch" }, { BIT.room_resv, "connectbook02461.reserveHours" } })
    if #reserve > 0 then out[#out + 1] = { "reserve", reserve } end
    return "lynnai-zone", out
  end
end

local applied = {}
function apply_caps(parent, child)
  local profile, list = wanted_caps(parent, child.parent_assigned_child_key)
  if not profile then return end
  local parts = {}
  for _, c in ipairs(list) do parts[#parts + 1] = c[1] .. ":" .. table.concat(c[2], ",") end
  local sig = profile .. "|" .. table.concat(parts, ";")
  if applied[child.id] == sig then return end
  applied[child.id] = sig
  log.info("보이는 컨트롤: " .. child.parent_assigned_child_key .. " → " .. sig)
  child:try_update_metadata({ profile = profile, optional_component_capabilities = list })
  for k in pairs(sent) do
    if k:sub(1, #child.id) == child.id then sent[k] = nil end
  end
end

-- ------------------------------------------------------------------ 허브에서 온 것
local function mac_of(b) return string.format("%02x%02x%02x", b:byte(2), b:byte(3), b:byte(4)) end

local function on_state(parent, b)
  local kind = b:byte(1)
  local mac = mac_of(b)
  local blob = b:sub(5)
  local s = hub_state(parent)
  -- 연결 목록을 받은 뒤엔 목록에 있는 기기만 온라인으로(허브가 끊긴 기기의 옛 상태를 보내도 온라인으로 되돌리지 않게)
  if not s.linked or s.linked[kind .. ":" .. mac] then s.online[kind .. ":" .. mac] = true end
  s.seen = s.seen or {}
  s.seen[kind .. ":" .. mac] = true
  if kind == KIND_F06 then
    s.boilers[mac] = decode_boiler(blob)
    update_outing(parent)                                             -- 새로 만든 상태에 외출 집계를 먼저(화면 갱신 전에)
    if shown(parent, KIND_F06, mac, 0) then create_boiler(parent, mac) end
    refresh_boiler(parent, mac)
  elseif kind == KIND_F09 then
    s.thermos[mac] = decode_rooms(blob)
    s.vboilers = s.vboilers or {}
    s.vboilers[mac] = decode_vboiler(blob, s.thermos[mac])
    update_outing(parent)                                             -- 방 상태가 바뀌었으니 외출 집계부터
    s.absent = s.absent or {}
    for _, id in ipairs(ZONE_ORDER) do
      local zk = zkey(mac, id)
      local c = parent:get_child_by_parent_assigned_key(zk)
      if not shown(parent, KIND_F09, mac, ZONES[id].n) then           -- 숨긴 방: 바로 정리
        s.absent[zk] = nil
        if c then delete_child(c) end
      elseif s.thermos[mac][id] then                                  -- 기기가 리포트한 방만 생성
        s.absent[zk] = nil
        s.had = s.had or {}
        s.had[zk] = true
        create_zone(parent, mac, id)
      else                                                            -- 안 보이는 방: 3번 연속일 때만 정리(기기 ID 유지)
        s.absent[zk] = (s.absent[zk] or 0) + 1
        if s.absent[zk] >= 3 and s.had then s.had[zk] = nil end
        if c and s.absent[zk] >= 3 then delete_child(c) end
      end
      refresh_zone(parent, mac, id)
    end
    if is_standalone(parent, mac) and all_reported(parent) then
      create_vboiler(parent, mac)
      refresh_vboiler(parent, mac)
    end
  end
  update_outing(parent)
  for bmac in pairs(s.boilers) do auto_boiler(parent, bmac) end   -- 방 집계 바뀌면 보일러 맞춤(끔/깸)
  if kind == KIND_F09 then for bmac in pairs(s.boilers) do refresh_boiler(parent, bmac) end end   -- 외출 표시
end

-- 보일러↔써머스탯 짝(관리 화면 수동): ([f09 mac3][f06 mac3])*n
local function on_pairs(parent, b)
  local s = hub_state(parent)
  s.pairs = {}
  for i = 1, #b, 6 do
    if b:byte(i + 5) then
      local f09 = string.format("%02x%02x%02x", b:byte(i), b:byte(i + 1), b:byte(i + 2))
      local f06 = string.format("%02x%02x%02x", b:byte(i + 3), b:byte(i + 4), b:byte(i + 5))
      s.pairs[f09] = f06
    end
  end
  update_outing(parent)
  refresh_all(parent)
end

local function on_links(parent, b)
  local s = hub_state(parent)
  s.online = {}
  s.linked = {}
  local since = {}                                   -- 목록에 들어온 시각(all_reported 의 기한)
  for i = 1, #b - 3, 4 do
    local kind = b:byte(i)
    if kind == KIND_F06 or kind == KIND_F09 then
      local key = kind .. ":" .. string.format("%02x%02x%02x", b:byte(i + 1), b:byte(i + 2), b:byte(i + 3))
      s.online[key] = true
      s.linked[key] = true
      since[key] = (s.link_since and s.link_since[key]) or os.time()
    end
  end
  s.link_since = since
  update_outing(parent)
  refresh_all(parent)
end

-- 허브에 직접 꽂은 센서(허브 1개당 하나) — 식별 MAC 없음, 키 고정
-- 허브가 고장으로 본 센서(플러그인 알림의 둘째 바이트: 1 온습도계 응답 없음, 2 난방 중인데 가스 펄스 없음)는 오프라인.
local FAULT_ROOM, FAULT_GAS = 1, 2
local function faulty(parent, bit) return ((hub_state(parent).plug_fault or 0) & bit) ~= 0 end

local function show_room(parent)
  local child = parent:get_child_by_parent_assigned_key(ROOM_KEY)
  if child and faulty(parent, FAULT_ROOM) then return go_offline(child, false, true) end   -- 글자 칸이 없는 기기
  local v = hub_state(parent).room
  if not child or not v then return end
  mark_online(child)
  emit(child, "main", capabilities.temperatureMeasurement.temperature({ value = math.floor(v[1] / 10 + 0.5) / 10, unit = "C" }))
  emit(child, "main", capabilities.relativeHumidityMeasurement.humidity({ value = math.floor(v[2] / 100 + 0.5) }))
end

local function on_room(parent, b)
  hub_state(parent).room = { string.unpack("<i2i2", b) }
  if not visible(parent, BIT.room) then return end
  create_child(parent, ROOM_KEY, "다용도실 온습도", "lynnai-room", "sht31")
  show_room(parent)
end

-- 도시가스 요금(기본값: 서울 코원, 한국도시가스협회 ’26.11 주택난방 · 각 시·도 보정계수 고시)
-- 수동 기본값(자동 조회 꺼짐·실패 시 폴백). SD07 경기 중부1 코원 기준.
local GAS_PREF = {
  vcf     = { key = "gasVcf",     def = 0.9939,  api = "api_vcf" },   -- 온압보정계수
  heat    = { key = "gasHeat",    def = 42.576,  api = "api_heat" },  -- 월평균 열량 MJ/N㎥
  rate    = { key = "gasRate",    def = 22.6226, api = "api_rate" },  -- 주택난방 단가 원/MJ
  base    = { key = "gasBaseFee", def = 1250,    api = "api_base" },  -- 기본료 원/월
  billday = { key = "gasBillDay", def = 8 },         -- 검침일
  vat     = { key = "gasVat",     def = true },      -- 부가세 10% 포함
}
-- 자동 조회 주소(LAN). 비었으면 nil → 수동값만.
local function api_base(dev)
  local b = dev and dev.preferences and dev.preferences.gasApiBase
  if b and b ~= "" then return (b:gsub("/+$", "")) end
end
-- 요금 요소: 받아온 캐시(펌웨어 중계 CMD 0x09 또는 LAN 직접 조회)가 있으면 그 값, 아니면 수동 설정값.
local function pref(dev, p)
  if p.api then
    local a = dev and dev:get_field(p.api)
    if a ~= nil then return a end
  end
  local v = dev and dev.preferences and dev.preferences[p.key]
  if v == nil then return p.def end
  return v
end

-- 이번 청구 기간(월)의 시작 태그 "YYYY-MM". 오늘이 청구일 이전이면 지난달이 기준.
local function period_tag(billday)
  local t = os.date("*t")
  local y, m = t.year, t.month
  if t.day < billday then
    m = m - 1
    if m < 1 then m, y = 12, y - 1 end
  end
  return string.format("%04d-%02d", y, m)
end

-- 사용량 m³ → 요금(원). with_base 면 기본료 포함(이번 달), 아니면 사용요금만(지난 하루).
local function gas_won(dev, m3, with_base)
  local won = m3 * pref(dev, GAS_PREF.vcf) * pref(dev, GAS_PREF.heat) * pref(dev, GAS_PREF.rate)
  if with_base then won = won + pref(dev, GAS_PREF.base) end
  if pref(dev, GAS_PREF.vat) ~= false then won = won * 1.1 end
  return math.floor(won + 0.5)
end

-- gas-heat-logger(/api/bill)에서 요금 요소(보정계수·열량·단가·기본료)를 받아 기기에 캐시한다.
-- Edge 샌드박스는 공개 인터넷을 막으므로 gasApiBase 는 LAN 주소여야 한다(예 NAS http://192.168.0.10:8496).
local show_gas                          -- 전방 선언(fetch_tariff 가 먼저 쓴다)
local GAS_API_TTL = 6 * 3600           -- 성공·실패와 무관하게 조회 간격(폭주 방지)
local gas_fetching = {}                 -- child.id -> true (동시 조회 방지)
local function num_after(body, key)     -- 평탄한 JSON 에서 "<key>": <숫자> 뽑기
  local s = body:match('"' .. key .. '"%s*:%s*(%-?%d[%d%.]*)')
  return s and tonumber(s)
end
local function fetch_tariff(child)
  local base = api_base(child)
  if not base or gas_fetching[child.id] then return end
  local region = child.preferences and child.preferences.gasRegion
  if not region or region == "" then region = "SD07" end
  local ok_c, cosock = pcall(require, "cosock")   -- 시뮬 등 네트워크 없는 환경에선 건너뜀
  if not ok_c then return end
  gas_fetching[child.id] = true
  local url = base .. "/api/bill?m3=0&region=" .. region
  cosock.spawn(function()
    local ok, body = pcall(function()
      local req = url:match("^https") and cosock.asyncify "ssl.https" or cosock.asyncify "socket.http"
      req.TIMEOUT = 10
      local b, code = req.request(url)
      if code ~= 200 or not b then error("http " .. tostring(code)) end
      return b
    end)
    gas_fetching[child.id] = nil
    if not ok then require("log").warn("가스 요금 조회 실패: " .. tostring(body)); return end
    local vcf, heat = num_after(body, "보정계수"), num_after(body, "열량")
    local rate, base2 = num_after(body, "단가"), num_after(body, "기본요금")
    if not (vcf and heat and rate) then require("log").warn("가스 요금 응답 파싱 실패"); return end
    child:set_field("api_vcf", vcf, { persist = true })
    child:set_field("api_heat", heat, { persist = true })
    child:set_field("api_rate", rate, { persist = true })
    if base2 then child:set_field("api_base", base2, { persist = true }) end
    require("log").info(string.format("가스 요금 자동: 보정 %.4f 열량 %.3f 단가 %.4f", vcf, heat, rate))
    local p = child:get_parent_device()
    if p then show_gas(p) end                   -- 받은 값으로 다시 그림
  end, "gas-tariff")
end

function show_gas(parent)
  local child = parent:get_child_by_parent_assigned_key(GAS_KEY)
  if child and faulty(parent, FAULT_GAS) then
    emit(child, "main", status_cap.status("펄스 없음 — 센서 확인"))
    return go_offline(child, false, true)
  end
  local v = hub_state(parent).gas
  if not child or not v then return end
  local count, reading, hour, day, flow = table.unpack(v)
  mark_online(child)
  local m3 = (reading ~= 0xFFFFFFFF) and reading / 1000 or count * 0.01
  local function f(l) return string.format("%.2f㎥", l / 1000) end
  local function r2(l) return math.floor(l / 10 + 0.5) / 100 end     -- 리터 → ㎥, 0.01 자리로
  -- 가스 기기는 구역(component)이 main 하나다 — 구역이 여럿이면 앱이 구역마다 제목 줄(main·유량·요금)을 붙인다.
  -- 글자 상태(1D · 1H)는 팝업·기기 카드용 요약이고, 상세 화면에는 아래 숫자 카드들이 나온다(화면 구성은 프로필의 vid).
  emit(child, "main", capabilities.gasMeter.gasMeterVolume({ value = m3, unit = "m^3" }))
  emit(child, "main", status_cap.status("1D " .. f(day) .. " · 1H " .. f(hour)))
  emit(child, "main", usage_cap.day({ value = r2(day), unit = "m^3" }))
  emit(child, "main", usage_cap.hour({ value = r2(hour), unit = "m^3" }))
  emit(child, "main", flow_cap.flow({ value = r2(flow), unit = "m³/h" }))

  -- 요금: 이번 달(청구일 기준 리셋) + 지난 하루. 누적 계량값은 펌웨어 NVS 에 남아 재부팅해도 이어진다.
  local tag = period_tag(pref(child, GAS_PREF.billday))
  local base_m3 = child:get_field("gas_month_base")
  if child:get_field("gas_month_tag") ~= tag or base_m3 == nil or m3 < base_m3 then
    base_m3 = m3                                   -- 새 청구 기간·재설치·계량기 교체 → 이번 달 기준 재설정
    child:set_field("gas_month_tag", tag, { persist = true })
    child:set_field("gas_month_base", base_m3, { persist = true })
  end
  local month_m3 = m3 - base_m3
  if month_m3 < 0 then month_m3 = 0 end
  month_m3 = math.floor(month_m3 * 100 + 0.5) / 100     -- 0.01㎥ 로 반올림(부동소수점 꼬리 제거)
  -- 금액은 숫자만 보낸다(서버가 capability 의 기본 단위 "원"을 붙인다). 단위를 같이 보내면 허브가 거부한다 — 허브가
  -- 들고 있는 capability 정의가 옛것(KRW 만)이라서다. 2026-11-22 에 두 번 확인: 정의를 고친 지 2시간이 지나고 드라이버를
  -- 다시 설치해도 "Given 원 for unit. Incorrect string given" 으로 그 뒤의 요금 갱신이 전부 멈췄다.
  emit(child, "main", cost_cap.month(gas_won(child, month_m3, true)))
  emit(child, "main", cost_cap.today(gas_won(child, day / 1000, false)))
  emit(child, "main", usage_cap.period({ value = month_m3, unit = "m^3" }))     -- 이번달 사용량은 사용량 카드에

  -- 요금 요소 자동 갱신(6시간마다, 비동기). 받으면 새 값으로 다시 그린다.
  -- 시도 시각으로 게이트 → 주소가 안 닿아도 매 업링크마다 재시도하지 않는다.
  if api_base(child) then
    local last = child:get_field("api_try")
    if not last or os.time() - last > GAS_API_TTL then
      child:set_field("api_try", os.time(), { persist = true })
      fetch_tariff(child)
    end
  end
end

local function on_gas(parent, b)
  hub_state(parent).gas = { string.unpack("<I4I4I4I4I4", b) }
  if not visible(parent, BIT.gas) then return end
  create_child(parent, GAS_KEY, "가스 계량기", "lynnai-gas", "gasmeter")
  show_gas(parent)
end

local PLUGIN_CHILD = { [1] = { key = ROOM_KEY, field = "room" }, [2] = { key = GAS_KEY, field = "gas" } }
local function on_plugins(parent, b)
  local m = b:byte(1) or 0
  local s = hub_state(parent)
  local was = s.plug_fault or 0
  s.plug_fault = b:byte(2) or 0                      -- 옛 펌웨어는 첫 바이트만 보낸다 → 고장 없음
  for bit, p in pairs(PLUGIN_CHILD) do
    if (m & bit) == 0 then
      s[p.field] = nil
      local child = parent:get_child_by_parent_assigned_key(p.key)
      if child then delete_child(child) end
    end
  end
  if s.plug_fault ~= was then                        -- 바뀐 순간에만(5분마다 오는 알림으로 값을 다시 내보내지 않게)
    show_room(parent)
    show_gas(parent)
  end
end

-- Zigbee 허브 기기 자체의 온도(ESP32 칩 온도 — 방 온도와 다름)를 보일지. LAN 허브 기기엔 온도가 없다.
local function apply_hub_caps(parent)
  if parent.network_type ~= "DEVICE_ZIGBEE" then return end
  local want = visible(parent, BIT.hub_temp) and { "temperatureMeasurement" } or {}
  local sig = table.concat(want, ",")
  if applied[parent.id] == sig then return end
  applied[parent.id] = sig
  log.info("허브 온도 " .. (#want > 0 and "보임" or "숨김"))
  parent:try_update_metadata({ profile = "lynnai-hub", optional_component_capabilities = { { "chip", want } } })
end

-- 이 통로에 무엇을 보일지(관리 화면 '노출'). 종류별 컨트롤 마스크 + 기기·방별 숨김.
local function on_expose(parent, b)
  if #b < 4 then return end
  local s = hub_state(parent)
  local hex = (b:gsub(".", function(c) return string.format("%02x", c:byte()) end))
  local changed = s.expose_hex ~= hex
  s.expose_hex = hex
  parse_expose(s, b)
  if changed then
    parent:set_field("expose2", hex, { persist = true })
    log.info("노출 바뀜: " .. hex)
  end
  -- show = 보여야 하나(true/false)
  local function sync(key, show, has_data, create)
    local child = parent:get_child_by_parent_assigned_key(key)
    if not show then
      if child then delete_child(child) end
    elseif not child then
      if has_data then create() end
    else
      apply_caps(parent, child)
    end
  end
  for mac in pairs(s.boilers) do
    sync(bkey(mac), shown(parent, KIND_F06, mac, 0), true, function() create_boiler(parent, mac) end)
  end
  for mac in pairs(s.thermos) do
    for _, id in ipairs(ZONE_ORDER) do
      sync(zkey(mac, id), shown(parent, KIND_F09, mac, ZONES[id].n), s.thermos[mac][id] ~= nil,
        function() create_zone(parent, mac, id) end)
    end
    local vc = parent:get_child_by_parent_assigned_key(vbkey(mac))   -- 가상 보일러도 보일러 컨트롤 노출을 따름
    if vc then apply_caps(parent, vc) end
  end
  apply_hub_caps(parent)
  sync(ROOM_KEY, visible(parent, BIT.room), s.room ~= nil, function() create_child(parent, ROOM_KEY, "다용도실 온습도", "lynnai-room", "sht31") end)
  sync(GAS_KEY, visible(parent, BIT.gas), s.gas ~= nil, function() create_child(parent, GAS_KEY, "가스 계량기", "lynnai-gas", "gasmeter") end)
  if changed then
    driver:call_with_delay(3, function()
      refresh_all(parent)
      show_room(parent)
      show_gas(parent)
    end)
  end
end

-- ------------------------------------------------------------------ 명령
local function boiler_cmds(parent, list)
  send_text(parent, table.concat(list, ";"))
end

-- 보일러 레벨 상태(실보일러 or 가상보일러). 가상은 아는 필드만(온수 설정 등).
local function bstate(parent, kind, mac)
  local s = hub_state(parent)
  return kind == "vboiler" and (s.vboilers or {})[mac] or s.boilers[mac]
end

-- 이 보일러에 붙은 방들의 외출을 on 으로(이미 그 상태인 방은 빼고). 방마다 한 줄.
local function outing_cmds(parent, kind, mac, on, list)
  for _, tmac in ipairs(thermos_of(parent, kind, mac)) do
    for _, id in ipairs(ZONE_ORDER) do
      local z = (hub_state(parent).thermos[tmac] or {})[id]
      if z and z.outing ~= on then
        list[#list + 1] = string.format("f09@%s a0=%d,b3=%s", tmac, ZONES[id].a0, on and "1" or "0")
      end
    end
  end
  return list
end

local function on_switch(on)
  return function(_, device, cmd)
    local parent = parent_of(device)
    local kind, mac, id = parse_key(device.parent_assigned_child_key)
    if kind == "boiler" or kind == "vboiler" then
      if cmd.component == "heat" or cmd.component == "outing" then
        local pre, list = boiler_prefix(kind, mac), {}
        if cmd.component == "heat" then
          -- 난방 켜기 = 방들의 외출을 풀고 난방 켬(외출 스위치는 저절로 꺼짐으로 보인다). 끄기 = 난방만 끔
          if on then outing_cmds(parent, kind, mac, false, list) end
          list[#list + 1] = pre .. " b1=" .. (on and "1" or "0")
        else
          -- 외출 = 붙은 방마다 f09 a0,b3(방마다 따로 — 한 메시지가 길어지지 않게). 켜면 난방 스위치는 꺼짐으로 보인다
          outing_cmds(parent, kind, mac, on, list)
          if on and #list == 0 then log.warn("외출을 걸 방이 없음(붙은 조절기 없음 또는 이미 모두 외출) — 보낸 명령 없음") end
        end
        for _, c in ipairs(list) do send_text(parent, c) end
        return
      end
      for key, c in pairs(SWITCH_COMPONENT) do
        if c == cmd.component then
          return boiler_cmds(parent, { string.format("%s %s=%s", boiler_prefix(kind, mac), SWITCH[key].code, on and "1" or "0") })
        end
      end
      return
    end
    if kind ~= "zone" then return end
    if cmd.component == "reserve" then
      -- 예약은 f09 에 a0(존)+96(켜기/끄기). 켤 때는 앱처럼 97(예약 시간)도 같이 — 기억해 둔 시간, 없으면 1시간.
      -- 꺼진 방이어도 켜는 명령은 따로 보내지 않는다(예약을 걸면 기기가 켜짐으로 바꾼다).
      local z = ZONES[id]
      local zs = (hub_state(parent).thermos[mac] or {})[id] or {}
      local v = resv_hours(device:get_field("resv_iv") or (zs.reserve and zs.resv))
      local list = {}
      if on then
        list[#list + 1] = string.format("f09@%s a0=%d,96=1,97=%02x", mac, z.a0, v)
      else
        list[#list + 1] = string.format("f09@%s a0=%d,96=0", mac, z.a0)
      end
      return boiler_cmds(parent, list)
    end
    if cmd.component == "outing" then
      -- 외출은 f09(각방 조절기)에 a0(존 선택)+b3 로 보낸다(실제 앱 캡처: f09 a0=7,b3=1).
      -- f06 b3 는 이 보일러에서 안 먹힌다(실측). a0 는 이 존의 선택자.
      local z = ZONES[id]
      return boiler_cmds(parent, { string.format("f09@%s a0=%d,b3=%s", mac, z.a0, on and "1" or "0") })
    end
    local z = ZONES[id]
    local list = {}
    if on then
      -- 방을 켜면 '이 방 난방' 뜻: 짝 보일러(지정 or 하나뿐)가 꺼져 있으면 같이 켠다
      local bmac, m = the_boiler(parent, mac)
      if bmac then
        m = m or {}
        if m.power == false then list[#list + 1] = "f06@" .. bmac .. " 80=1" end
        if m.heat == false then list[#list + 1] = "f06@" .. bmac .. " b1=1" end
      end
      local zs = (hub_state(parent).thermos[mac] or {})[id]
      if zs and zs.outing then list[#list + 1] = string.format("f09@%s a0=%d,b3=0", mac, z.a0) end   -- 이 방 외출 풀기
    end
    list[#list + 1] = string.format("f09@%s a0=%d,b1=%s", mac, z.a0, on and "1" or "0")
    boiler_cmds(parent, list)
    driver:call_with_delay(8, function()
      sent[table.concat({ device.id, "main", "switch", "switch" }, "|")] = nil
      refresh_zone(parent, mac, id)
    end)
  end
end

local function set_heating_setpoint(_, device, cmd)
  local parent = parent_of(device)
  local v = math.floor(cmd.args.setpoint + 0.5)
  local kind, mac, id = parse_key(device.parent_assigned_child_key)
  if kind == "zone" then
    v = math.max(10, math.min(ZONES[id].max, v))
    return boiler_cmds(parent, { string.format("f09@%s a0=%d,d4=%02X", mac, ZONES[id].a0, v) })
  end
  if (kind ~= "boiler" and kind ~= "vboiler") or cmd.component ~= "heat" then return end
  local m = bstate(parent, kind, mac) or {}
  if m.heat_mode == "실내" then
    boiler_cmds(parent, { string.format("%s d4=%02X", boiler_prefix(kind, mac), snap(v, ROOM_TEMPS, m.room_set)) })
  else
    boiler_cmds(parent, { string.format("%s d2=%02X", boiler_prefix(kind, mac), snap(v, ONDOL_TEMPS, m.ondol_set)) })
  end
end

-- 예약 시간 슬라이더: 고른 시간으로 예약을 건다(앱과 같다 — 시간만 바꾸고 예약은 안 거는 명령은 없다)
local function set_reserve_hours(_, device, cmd)
  local parent = parent_of(device)
  local kind, mac, id = parse_key(device.parent_assigned_child_key)
  if kind ~= "zone" then return end
  local v = resv_hours(cmd.args.hours)
  device:set_field("resv_iv", v, { persist = true })
  return boiler_cmds(parent, { string.format("f09@%s a0=%d,96=1,97=%02x", mac, ZONES[id].a0, v) })
end

local function set_heat_type(_, device, cmd)
  local kind, mac = parse_key(device.parent_assigned_child_key)
  if kind ~= "boiler" and kind ~= "vboiler" then return end
  local want = cmd.args.mode
  if want ~= "ondol" and want ~= "heat" then                    -- 표준 목록의 다른 값은 무시(난방 방식을 바꾸지 않게)
    log.warn("모르는 난방 방식 무시: " .. tostring(want))
    return
  end
  boiler_cmds(parent_of(device), { string.format("%s b4=%s", boiler_prefix(kind, mac), want == "heat" and "1" or "0") })
end

local function set_hw_setpoint(_, device, cmd)
  local parent = parent_of(device)
  local kind, mac = parse_key(device.parent_assigned_child_key)
  if kind ~= "boiler" and kind ~= "vboiler" then return end
  local m = bstate(parent, kind, mac) or {}
  local v = snap(math.floor(cmd.args.setpoint + 0.5), HW_TEMPS, m.hw_set)
  boiler_cmds(parent, { string.format("%s d1=%02X,df=0", boiler_prefix(kind, mac), v) })
end

local function do_refresh(_, device)
  local parent = parent_of(device)
  send_text(parent, "refresh")
  refresh_all(parent)
end

-- ------------------------------------------------------------------ 수명
local on_hub_init

local function device_init(_, device)
  if is_child(device) then
    local parent = device:get_parent_device()
    if parent then
      apply_caps(parent, device)
      refresh_all(parent)
      show_room(parent)
      show_gas(parent)
    end
    return
  end
  on_hub_init(device)
end

-- 허브에서 기기 삭제 알림: [종류][mac3] → 그 mac 의 자식 기기(보일러·존·가상)를 모두 지운다.
local function on_forget(parent, b)
  local kind = b:byte(1)
  local mac = string.format("%02x%02x%02x", b:byte(2) or 0, b:byte(3) or 0, b:byte(4) or 0)
  local s = hub_state(parent)
  if kind == KIND_F06 then
    s.boilers[mac] = nil
  elseif kind == KIND_F09 then
    s.thermos[mac] = nil
    if s.vboilers then s.vboilers[mac] = nil end
  end
  s.online[kind .. ":" .. mac] = nil
  for _, d in ipairs(driver:get_devices()) do
    local key = d.parent_assigned_child_key
    if key then
      local _, m2 = parse_key(key)
      if m2 == mac then delete_child(d) end
    end
  end
  refresh_all(parent)
end

-- ------------------------------------------------------------------ 밖으로
local M = {}
M.is_child = is_child

function M.setup(drv, opts)
  driver = drv
  send_hook = opts.send
  on_hub_init = opts.on_hub_init
  SUFFIX = opts.suffix or ""
end

-- 허브가 중계한 가스 요금 요소(CMD 0x09): u32 x4 (보정계수·열량·단가 ×1e6, 기본료 원). 가스 기기에 캐시.
local function on_tariff(parent, b)
  if #b < 16 then return end
  local vcf_e6, heat_e6, rate_e6, base = string.unpack("<I4I4I4I4", b)
  local child = parent:get_child_by_parent_assigned_key(GAS_KEY)
  if not child then return end          -- 가스 기기 아직 없으면 버림(다음 주기에 다시 온다)
  child:set_field("api_vcf", vcf_e6 / 1e6, { persist = true })
  child:set_field("api_heat", heat_e6 / 1e6, { persist = true })
  child:set_field("api_rate", rate_e6 / 1e6, { persist = true })
  child:set_field("api_base", base, { persist = true })
  log.info(string.format("가스 요금 중계 수신: 보정 %.4f 열량 %.3f 단가 %.4f 기본 %d",
    vcf_e6 / 1e6, heat_e6 / 1e6, rate_e6 / 1e6, base))
  show_gas(parent)
end

local HANDLERS = { [CMD_STATE] = on_state, [CMD_LINKS] = on_links, [CMD_ROOM] = on_room, [CMD_GAS] = on_gas,
  [CMD_PLUGINS] = on_plugins, [CMD_EXPOSE] = on_expose, [CMD_PAIRS] = on_pairs, [CMD_FORGET] = on_forget,
  [CMD_TARIFF] = on_tariff }

function M.info_changed(_, device)
  if not is_child(device) then return end
  for k in pairs(sent) do
    if k:sub(1, #device.id) == device.id then sent[k] = nil end
  end
  last_text[device.id] = nil
  local parent = device:get_parent_device()
  if parent then
    refresh_all(parent)
    show_room(parent)
    show_gas(parent)
  end
end

function M.on_message(parent, cmd, bytes)
  local h = HANDLERS[cmd]
  if h then h(parent, bytes) end
end

function M.hub_lost(parent)
  hub_state(parent).online = {}
  refresh_all(parent)                    -- 상태를 아는 보일러·방: "연결 끊김" 표시
  -- 그 밖의 자식도 모두 오프라인으로: 센서(온습도·가스), 그리고 드라이버가 재시작해 상태를 모르는 보일러·방
  for _, d in ipairs(driver:get_devices()) do
    if is_child(d) then
      local p = d:get_parent_device()
      if p and p.id == parent.id then
        pending_off[d.id] = nil
        d:offline()
      end
    end
  end
end

function M.request_refresh(parent) send_text(parent, "refresh") end

M.device_init = device_init

M.capability_handlers = {
  [capabilities.switch.ID] = {
    [capabilities.switch.commands.on.NAME] = on_switch(true),
    [capabilities.switch.commands.off.NAME] = on_switch(false),
  },
  [capabilities.thermostatHeatingSetpoint.ID] = {
    [capabilities.thermostatHeatingSetpoint.commands.setHeatingSetpoint.NAME] = set_heating_setpoint,
  },
  [rh_cap.ID] = {
    [rh_cap.commands.setHours.NAME] = set_reserve_hours,
  },
  [tm_cap.ID] = {
    [tm_cap.commands.setThermostatMode.NAME] = set_heat_type,
  },
  [hw_cap.ID] = {
    [hw_cap.commands.setHeatingSetpoint.NAME] = set_hw_setpoint,
  },
  [capabilities.refresh.ID] = {
    [capabilities.refresh.commands.refresh.NAME] = do_refresh,
  },
}

return M

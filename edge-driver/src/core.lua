-- LYNNAI 허브 공통 로직(Zigbee 드라이버와 LAN 드라이버가 같이 쓴다).
--
-- 허브(ESP32/NAS)는 보일러(f06)·각방 조절기(f09)의 클라우드 연결을 중계하면서 상태를 엿보고 아래로 보낸다.
-- 기기는 종류(f06/f09)가 아니라 MAC 끝 3바이트(instance, 6 hex)로 식별한다 → 보일러 N대·써머스탯 N개.
--   허브 → ST  0x01 상태 [종류 6|9][MAC끝3(3B)][EPC70 블롭]
--              0x02 연결 ([종류][MAC끝3]) * n  (목록에 없으면 오프라인)
--              0x03 온습도 int16 x2   0x04 가스 u32 x5   0x05 플러그인 비트   0x06 노출 u32
--   ST → 허브  0x00 명령 "f06@<mac> b1=1" / "f09@<mac> a0=7,d4=10" / "refresh" (여러 개는 ";")
--
-- 자식 기기 키: 보일러 = "b"..mac, 존 = "t"..mac.."_"..slot. 센서는 "room"/"gas"(허브 1개당 하나).
-- 짝(어느 f09가 어느 f06에 붙나)은 1단계에선 '보일러가 하나뿐이면 암묵 연동'. 2단계에서 관리 화면 수동 지정.
local capabilities = require "st.capabilities"
local log = require "log"

local CMD_STATE, CMD_LINKS, CMD_ROOM, CMD_GAS, CMD_PLUGINS, CMD_EXPOSE, CMD_PAIRS = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
local CMD_FORGET = 0x08
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
  outing = { pos = 19, code = "b3" },
}
local SWITCH_COMPONENT = { power = "main", hotwater = "hotwater", preheat = "preheat" }
local HEAT_STATES = { "난방", "외출", "끄기" }
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
local hw_cap = capabilities["connectbook02461.hotWaterSetpoint"]
local ht_cap = capabilities["connectbook02461.heatType"]

local driver

-- 허브(부모)별 상태. boilers[mac]=해독값, thermos[mac]={z1=..}, online={["6:mac"]=true}
local hubs = {}
local function hub_state(parent)
  hubs[parent.id] = hubs[parent.id] or { boilers = {}, thermos = {}, online = {} }
  return hubs[parent.id]
end

-- 노출(펌웨어 hub_expose.h). 0x06 = [u32 마스크] + ([종류][MAC끝3][숨김 비트])*n
--   마스크: 종류별 컨트롤(보일러 조절기 1~7, 방 8~10) + 플러그인 기기(29 온습도, 30 가스). 그 종류 모든 기기에 적용.
--   숨김 비트(기기별): bit0 = 기기 자신(보일러 조절기), bit1~7 = 온도 조절기의 방 z1~z7(ZONES[id].n)
local ALL_VISIBLE = 0x7FFFFFFF   -- 펌웨어 ALL_BITS 와 같게
local BIT = { power = 1, heat_mode = 2, heat_type = 3, heat_temp = 4, hw = 5, hw_temp = 6, preheat = 7,
  room_on = 8, room_set = 9, room_out = 10, room = 29, gas = 30 }
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
      out[id] = { on = (st & 0x02) ~= 0, heating = (st & 0x40) ~= 0, outing = (st & 0x10) ~= 0, current = cur, target = set }
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
local function emit(device, comp, ev)
  if not device:supports_capability_by_id(ev.capability.ID, comp) then return end
  local key = table.concat({ device.id, comp, ev.capability.ID, ev.attribute.NAME }, "|")
  local v = ev.value
  local val = type(v) == "table" and table.concat({ tostring(v.value), tostring(v.minimum), tostring(v.maximum),
    tostring(v[1]), tostring(#v) }, ",") or tostring(v)
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

local function zone_text(m, z)
  local what
  if z.outing then what = "외출"                                    -- 존별 외출(f09)
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

local function go_offline(device, is_zone)
  emit_text(device, "연결 끊김", is_zone)
  driver:call_with_delay(3, function() device:offline() end)
end

-- 자동화(상태 기반, 양방향): 보일러에 붙은 방들의 on/off 집계가 바뀌는 '순간'에 보일러를 맞춘다.
--   · 일부 켜짐 → 전부 꺼짐  : 보일러 난방 끔 (보일러는 이 방향을 스스로 안 해줌)
--   · 전부 꺼짐 → 일부 켜짐  : 보일러 깨움(전원·외출해제·난방)
-- 상태로 들어오므로 ST 앱뿐 아니라 Rinnai 리모컨·스케줄로 방이 바뀌어도 동작한다. 전환 순간에만 1회라
-- 재시작(첫 관찰 씨딩)·사용자가 수동으로 보일러를 켜거나 끈 경우엔 되돌리지 않는다(안 싸움).
-- (ST 앱에서 방 켤 때 즉시 깨우는 건 on_switch 에 그대로 두고, 여기선 보조/비-ST 경로를 커버.)
local prev_all_off = {}        -- bmac -> 직전 '전부 꺼짐' 여부
local function auto_boiler(parent, bmac)
  local s = hub_state(parent)
  local m = s.boilers[bmac]
  if not m then return end
  local has_thermo, all_off = false, true
  for tmac, zones in pairs(s.thermos) do
    if the_boiler(parent, tmac) == bmac then
      has_thermo = true
      for _, z in pairs(zones) do if z.on then all_off = false end end   -- z.on = 방 자신의 on/off
    end
  end
  if not has_thermo then prev_all_off[bmac] = nil; return end            -- 붙은 방 없음 → 건드리지 않음
  local was = prev_all_off[bmac]
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
    if m.outing then list[#list + 1] = "f06@" .. bmac .. " b3=0" end
    if not m.heat then list[#list + 1] = "f06@" .. bmac .. " b1=1" end
    if #list > 0 then
      log.info("방 켜짐 → 보일러 " .. bmac .. " 깨움")
      send_text(parent, table.concat(list, ";"))
    end
  end
end

local function refresh_boiler(parent, mac)
  local child = parent:get_child_by_parent_assigned_key(bkey(mac))
  if not child then return end
  local s = hub_state(parent)
  if not s.online["6:" .. mac] then return go_offline(child) end
  local m = s.boilers[mac]
  if not m then return end
  child:online()
  for key, comp in pairs(SWITCH_COMPONENT) do
    emit(child, comp, m[key] and capabilities.switch.switch.on() or capabilities.switch.switch.off())
  end
  emit(child, "heat", capabilities.mode.supportedModes(HEAT_STATES, { visibility = { displayed = false } }))
  emit(child, "heat", capabilities.mode.supportedArguments(HEAT_STATES, { visibility = { displayed = false } }))
  emit(child, "heat", capabilities.mode.mode(m.outing and "외출" or m.heat and "난방" or "끄기"))
  emit(child, "heat", ht_cap.heatType(m.heat_mode))
  emit(child, "heat", sp_range(RANGE[m.heat_mode]))
  if m.heat_temp then emit(child, "heat", setpoint(m.heat_temp)) end
  if m.hw_set then emit(child, "hotwater", hw_cap.setpoint({ value = m.hw_set, unit = "C" })) end
  emit_text(child, main_text(m))
end

local function refresh_zone(parent, mac, id)
  local child = parent:get_child_by_parent_assigned_key(zkey(mac, id))
  if not child then return end
  local s = hub_state(parent)
  if not s.online["9:" .. mac] then return go_offline(child, true) end
  local z = s.thermos[mac] and s.thermos[mac][id]
  if not z then return end
  child:online()
  local _, m = the_boiler(parent, mac)   -- 짝 보일러(지정 or 암묵)
  local heating = z.on and (not m or (m.power and m.heat))
  emit(child, "main", heating and capabilities.switch.switch.on() or capabilities.switch.switch.off())
  emit(child, "main", sp_range({ 10, ZONES[id].max }))
  emit(child, "main", setpoint(z.target))
  emit(child, "main", capabilities.temperatureMeasurement.temperature({ value = z.current, unit = "C" }))
  emit(child, "outing", z.outing and capabilities.switch.switch.on() or capabilities.switch.switch.off())  -- 존별 외출(f09 stat bit 0x10)
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
    if is_standalone(parent, mac) then
      create_vboiler(parent, mac)
      refresh_vboiler(parent, mac)
    elseif vchild then
      delete_child(vchild)        -- 실보일러에 연결됨 → 가상 보일러 불필요
    end
  end
end

-- ------------------------------------------------------------------ 자식 기기 만들기
local function create_child(parent, key, label, profile, model)
  if parent:get_child_by_parent_assigned_key(key) then return end
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
  if driver.try_delete_device then driver:try_delete_device(child.id) end
end

function refresh_vboiler(parent, mac)
  local child = parent:get_child_by_parent_assigned_key(vbkey(mac))
  if not child then return end
  local s = hub_state(parent)
  if not s.online["9:" .. mac] then return go_offline(child) end
  local vb = s.vboilers and s.vboilers[mac]
  child:online()
  if vb and vb.hw_set and vb.hw_set > 0 then
    emit(child, "hotwater", hw_cap.setpoint({ value = vb.hw_set, unit = "C" }))
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
  local kind, _, id = parse_key(key)
  if kind == "boiler" or kind == "vboiler" then
    local comps = {
      { "main", { { BIT.power, "switch" } } },
      { "heat", { { BIT.heat_mode, "mode" }, { BIT.heat_type, "connectbook02461.heatType" },
        { BIT.heat_temp, "thermostatHeatingSetpoint" } } },
      { "hotwater", { { BIT.hw, "switch" }, { BIT.hw_temp, "connectbook02461.hotWaterSetpoint" } } },
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
    return "lynnai-zone", out
  end
end

local applied = {}
local function apply_caps(parent, child)
  local profile, list = wanted_caps(parent, child.parent_assigned_child_key)
  if not profile then return end
  local parts = {}
  for _, c in ipairs(list) do parts[#parts + 1] = c[1] .. ":" .. table.concat(c[2], ",") end
  local sig = table.concat(parts, ";")
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
  s.online[kind .. ":" .. mac] = true
  if kind == KIND_F06 then
    s.boilers[mac] = decode_boiler(blob)
    if shown(parent, KIND_F06, mac, 0) then create_boiler(parent, mac) end
    refresh_boiler(parent, mac)
  elseif kind == KIND_F09 then
    s.thermos[mac] = decode_rooms(blob)
    s.vboilers = s.vboilers or {}
    s.vboilers[mac] = decode_vboiler(blob, s.thermos[mac])
    for _, id in ipairs(ZONE_ORDER) do
      if s.thermos[mac][id] and shown(parent, KIND_F09, mac, ZONES[id].n) then   -- 기기가 리포트한 방만 생성
        create_zone(parent, mac, id)
      else                                                            -- 없는 방/숨긴 방의 옛 기기는 정리
        local c = parent:get_child_by_parent_assigned_key(zkey(mac, id))
        if c then delete_child(c) end
      end
      refresh_zone(parent, mac, id)
    end
    if is_standalone(parent, mac) then
      create_vboiler(parent, mac)
      refresh_vboiler(parent, mac)
    end
  end
  for bmac in pairs(s.boilers) do auto_boiler(parent, bmac) end   -- 방 집계 바뀌면 보일러 맞춤(끔/깸)
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
  refresh_all(parent)
end

local function on_links(parent, b)
  local s = hub_state(parent)
  s.online = {}
  for i = 1, #b, 4 do
    local kind = b:byte(i)
    if kind then
      s.online[kind .. ":" .. string.format("%02x%02x%02x", b:byte(i + 1), b:byte(i + 2), b:byte(i + 3))] = true
    end
  end
  refresh_all(parent)
end

-- 허브에 직접 꽂은 센서(허브 1개당 하나) — 식별 MAC 없음, 키 고정
local function show_room(parent)
  local child = parent:get_child_by_parent_assigned_key(ROOM_KEY)
  local v = hub_state(parent).room
  if not child or not v then return end
  child:online()
  emit(child, "main", capabilities.temperatureMeasurement.temperature({ value = math.floor(v[1] / 10 + 0.5) / 10, unit = "C" }))
  emit(child, "main", capabilities.relativeHumidityMeasurement.humidity({ value = math.floor(v[2] / 100 + 0.5) }))
end

local function on_room(parent, b)
  hub_state(parent).room = { string.unpack("<i2i2", b) }
  if not visible(parent, BIT.room) then return end
  create_child(parent, ROOM_KEY, "다용도실 온습도", "lynnai-room", "sht31")
  show_room(parent)
end

local function show_gas(parent)
  local child = parent:get_child_by_parent_assigned_key(GAS_KEY)
  local v = hub_state(parent).gas
  if not child or not v then return end
  local count, reading, hour, day, flow = table.unpack(v)
  child:online()
  local m3 = (reading ~= 0xFFFFFFFF) and reading / 1000 or count * 0.01
  local function f(l) return string.format("%.2f㎥", l / 1000) end
  emit(child, "main", capabilities.gasMeter.gasMeterVolume({ value = m3, unit = "m^3" }))
  emit(child, "main", status_cap.status("1D " .. f(day) .. " · 1H " .. f(hour)))
  emit(child, "flow", status_cap.status(flow > 0 and string.format("%.2f㎥/h", flow / 1000) or "사용 안 함"))
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
  for bit, p in pairs(PLUGIN_CHILD) do
    if (m & bit) == 0 then
      hub_state(parent)[p.field] = nil
      local child = parent:get_child_by_parent_assigned_key(p.key)
      if child then delete_child(child) end
    end
  end
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

local function on_switch(on)
  return function(_, device, cmd)
    local parent = parent_of(device)
    local kind, mac, id = parse_key(device.parent_assigned_child_key)
    if kind == "boiler" or kind == "vboiler" then
      for key, c in pairs(SWITCH_COMPONENT) do
        if c == cmd.component then
          return boiler_cmds(parent, { string.format("%s %s=%s", boiler_prefix(kind, mac), SWITCH[key].code, on and "1" or "0") })
        end
      end
      return
    end
    if kind ~= "zone" then return end
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
        if m.outing then list[#list + 1] = "f06@" .. bmac .. " b3=0" end
        if m.heat == false then list[#list + 1] = "f06@" .. bmac .. " b1=1" end
      end
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

local function set_mode(_, device, cmd)
  if cmd.component ~= "heat" then return end
  local parent = parent_of(device)
  local kind, mac = parse_key(device.parent_assigned_child_key)
  if kind ~= "boiler" and kind ~= "vboiler" then return end
  local pre = boiler_prefix(kind, mac)
  local m = bstate(parent, kind, mac) or {}
  local want = cmd.args.mode
  if want == "외출" then return boiler_cmds(parent, { pre .. " b3=1" }) end
  local list = {}
  if m.outing then list[#list + 1] = pre .. " b3=0" end
  list[#list + 1] = pre .. " b1=" .. (want == "난방" and "1" or "0")
  boiler_cmds(parent, list)
end

local function set_heat_type(_, device, cmd)
  local kind, mac = parse_key(device.parent_assigned_child_key)
  if kind ~= "boiler" and kind ~= "vboiler" then return end
  boiler_cmds(parent_of(device), { string.format("%s b4=%s", boiler_prefix(kind, mac), cmd.args.heatType == "실내" and "1" or "0") })
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

local HANDLERS = { [CMD_STATE] = on_state, [CMD_LINKS] = on_links, [CMD_ROOM] = on_room, [CMD_GAS] = on_gas,
  [CMD_PLUGINS] = on_plugins, [CMD_EXPOSE] = on_expose, [CMD_PAIRS] = on_pairs, [CMD_FORGET] = on_forget }

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
  refresh_all(parent)
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
  [capabilities.mode.ID] = {
    [capabilities.mode.commands.setMode.NAME] = set_mode,
  },
  [ht_cap.ID] = {
    [ht_cap.commands.setHeatType.NAME] = set_heat_type,
  },
  [hw_cap.ID] = {
    [hw_cap.commands.setSetpoint.NAME] = set_hw_setpoint,
  },
  [capabilities.refresh.ID] = {
    [capabilities.refresh.commands.refresh.NAME] = do_refresh,
  },
}

return M

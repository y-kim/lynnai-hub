-- LYNNAI 허브 Zigbee Edge 드라이버.
--
-- 허브(ESP32-C6)는 보일러(f06)·각방 조절기(f09)의 클라우드 연결을 중계하면서 상태를 엿보고,
-- 전용 클러스터 0xFC31 로 SmartThings 와 주고받는다(펌웨어 hub_zb_rinnai.c 참고).
--   허브 → ST  0x01 상태 [종류 6|9][EPC 70 블롭]   0x02 연결 [bit0 보일러, bit1 각방]
--   ST → 허브  0x00 명령 "f06 b1=1" / "f09 a0=7,b1=1,d4=10" / "refresh" (여러 개는 ";")
--
-- 허브 아래에 자식 기기를 만든다: 보일러(전원·난방·온수·예열) + 각방 3개.
-- 화면 구성·문구는 예전 LAN 드라이버(rinnai-client/stdriver)와 같다.
local capabilities = require "st.capabilities"
local ZigbeeDriver = require "st.zigbee"
local defaults = require "st.zigbee.defaults"
local messages = require "st.zigbee.messages"
local zcl_messages = require "st.zigbee.zcl"
local data_types = require "st.zigbee.data_types"
local generic_body = require "st.zigbee.generic_body"
local zb_const = require "st.zigbee.constants"
local log = require "log"

local CLUSTER = 0xFC31
local CMD_REQUEST, CMD_STATE, CMD_LINKS = 0x00, 0x01, 0x02
local HUB_EP = 10

-- 자식 기기: 키 → 이름·프로필. 구역은 f09 상태 블롭의 슬롯(7바이트, 슬롯 s 는 byte[7s]).
--   상태 바이트 0x02 = 켜짐, 0x40 = 난방 중(밸브 열림). 명령 선택자 a0 = 슬롯+1.
local MAIN_KEY = "main"
local ZONES = {
  z1 = { label = "안방 보일러", stat = 0, cur = 3, set = 4, a0 = 1, max = 30 },
  z2 = { label = "보일러 방2", stat = 7, cur = 10, set = 11, a0 = 2, max = 30 },
  z3 = { label = "보일러 거실", stat = 42, cur = 45, set = 46, a0 = 7, max = 30 },
}
local ZONE_ORDER = { "z1", "z2", "z3" }

-- 보일러 명령 코드(앱 RinnaiUtil / habridge BOILER_SWITCHES): 상태 문자열 위치(0부터) + 명령
local SWITCH = {
  power = { pos = 0, code = "80" },
  heat = { pos = 2, code = "b1" },
  hotwater = { pos = 3, code = "d7" },
  preheat = { pos = 5, code = "b6" },
  outing = { pos = 19, code = "b3" },
}
local SWITCH_COMPONENT = { power = "main", hotwater = "hotwater", preheat = "preheat" }
local HEAT_STATES = { "난방", "외출", "끄기" }
local HEAT_MODES = { [0] = "온돌", [1] = "실내" }       -- 상태 위치 7 / 명령 b4
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

-- 허브(부모)별 마지막 상태
local hubs = {}       -- [parent.id] = { links = n, main = {...}, rooms = {...} }
local function hub_state(parent)
  hubs[parent.id] = hubs[parent.id] or { rooms = {} }
  return hubs[parent.id]
end

local function is_child(device) return device.parent_assigned_child_key ~= nil end
local function parent_of(device) return is_child(device) and device:get_parent_device() or device end

-- ------------------------------------------------------------------ 해독

local function tohex(s) return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end)) end

-- f06 블롭 → habridge decode_boiler 와 같은 값 (상태 문자열 위치 = 16진 문자 위치)
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
  return m
end

local function decode_rooms(blob)
  local out = {}
  for id, z in pairs(ZONES) do
    local st, cur, set = blob:byte(z.stat + 1), blob:byte(z.cur + 1), blob:byte(z.set + 1)
    if st and cur and set then
      out[id] = { on = (st & 0x02) ~= 0, heating = (st & 0x40) ~= 0, current = cur, target = set }
    end
  end
  return out
end

-- ------------------------------------------------------------------ 허브로 명령

local function send_text(parent, text)
  log.info("→ 허브 명령: " .. text)
  local header = zcl_messages.ZclHeader({ cmd = data_types.ZCLCommandId(CMD_REQUEST) })
  header.frame_ctrl:set_cluster_specific()
  header.frame_ctrl:set_disable_default_response()
  local addr = messages.AddressHeader(zb_const.HUB.ADDR, zb_const.HUB.ENDPOINT, parent:get_short_address(),
    HUB_EP, zb_const.HA_PROFILE_ID, CLUSTER)
  local body = zcl_messages.ZclMessageBody({ zcl_header = header, zcl_body = generic_body.GenericBody(text) })
  parent:send(messages.ZigbeeMessageTx({ address_header = addr, body = body }))
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

-- 바뀐 것만 보낸다(허브가 이벤트를 너무 많이 보내는 기기를 늦춘다)
local sent = {}
local function emit(device, comp, ev)
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

local function main_text(m)
  local function t(v) return (tonumber(v) or 0) > 0 and (" " .. deg(v)) or "" end
  if not m.power then return "꺼짐" end
  local heat
  if m.outing then heat = "외출"
  elseif m.heat then heat = m.heat_mode .. t(m.heat_temp)
  else heat = "꺼짐" end
  if m.hotwater then return heat .. " · 온수" .. t(m.hw_set) end
  if heat == "꺼짐" then return "대기" end
  return heat
end

local function zone_text(m, z)
  local what
  if (m and (not m.power or not m.heat)) or not z.on then what = "꺼짐"
  elseif m and m.outing then what = "외출"
  else what = "난방 " .. deg(z.target) end
  return deg(z.current) .. " · " .. what
end

-- 상태 문구는 같은 값이면 허브가 버리므로 바뀔 때만 강제로 보낸다
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

local function refresh_main(parent)
  local child = parent:get_child_by_parent_assigned_key(MAIN_KEY)
  local s = hub_state(parent)
  if not child then return end
  if not s.links or (s.links & 1) == 0 then return go_offline(child) end
  local m = s.main
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

local function refresh_zone(parent, id)
  local child = parent:get_child_by_parent_assigned_key(id)
  local s = hub_state(parent)
  if not child then return end
  if not s.links or (s.links & 2) == 0 then return go_offline(child, true) end
  local z = s.rooms[id]
  if not z then return end
  child:online()
  local m = s.main
  local heating = z.on and (not m or (m.power and m.heat))
  emit(child, "main", heating and capabilities.switch.switch.on() or capabilities.switch.switch.off())
  emit(child, "main", sp_range({ 10, ZONES[id].max }))
  emit(child, "main", setpoint(z.target))
  emit(child, "main", capabilities.temperatureMeasurement.temperature({ value = z.current, unit = "C" }))
  emit_text(child, zone_text(m, z), true)
end

local function refresh_all(parent)
  refresh_main(parent)
  for _, id in ipairs(ZONE_ORDER) do refresh_zone(parent, id) end
end

-- ------------------------------------------------------------------ 자식 기기 만들기

local function create_child(parent, key, label, profile, model)
  if parent:get_child_by_parent_assigned_key(key) then return end
  log.info("자식 기기 만들기: " .. key .. " " .. label)
  driver:try_create_device({
    type = "EDGE_CHILD", label = label, profile = profile, parent_device_id = parent.id,
    parent_assigned_child_key = key, vendor_provided_label = label, manufacturer = "Lynnai", model = model,
  })
end

local function ensure_children(parent, kind)
  if kind == 6 then create_child(parent, MAIN_KEY, "보일러", "lynnai-boiler", "boiler") end
  if kind == 9 then
    for _, id in ipairs(ZONE_ORDER) do create_child(parent, id, ZONES[id].label, "lynnai-zone", "zone") end
  end
end

-- ------------------------------------------------------------------ 허브에서 온 것

local function on_state(_, parent, zb_rx)
  local b = zb_rx.body.zcl_body.body_bytes
  local kind, blob = b:byte(1), b:sub(2)
  local s = hub_state(parent)
  s.links = (s.links or 0) | (kind == 6 and 1 or kind == 9 and 2 or 0)
  if kind == 6 then
    s.main = decode_boiler(blob)
  elseif kind == 9 then
    s.rooms = decode_rooms(blob)
  else
    return
  end
  log.info(string.format("f0%d 상태 %d바이트 %s", kind, #blob, tohex(blob)))
  ensure_children(parent, kind)
  refresh_all(parent)
end

local function on_links(_, parent, zb_rx)
  local s = hub_state(parent)
  s.links = zb_rx.body.zcl_body.body_bytes:byte(1) or 0
  log.info(string.format("연결 상태 보일러=%d 각방=%d", s.links & 1, (s.links >> 1) & 1))
  refresh_all(parent)
end

-- ------------------------------------------------------------------ 명령

local function boiler_cmds(parent, list)
  send_text(parent, table.concat(list, ";"))
end

local function main_switch(parent, comp, on)
  for key, c in pairs(SWITCH_COMPONENT) do
    if c == comp then return boiler_cmds(parent, { "f06 " .. SWITCH[key].code .. "=" .. (on and "1" or "0") }) end
  end
end

local function on_switch(on)
  return function(_, device, cmd)
    local parent = parent_of(device)
    local key = device.parent_assigned_child_key
    local z = ZONES[key]
    if not z then return main_switch(parent, cmd.component, on) end
    local list = {}
    if on then
      -- 방을 켜면 '이 방 난방' 뜻: 보일러가 꺼져 있으면 같이 켠다
      local m = hub_state(parent).main or {}
      if m.power == false then list[#list + 1] = "f06 80=1" end
      if m.outing then list[#list + 1] = "f06 b3=0" end
      if m.heat == false then list[#list + 1] = "f06 b1=1" end
    end
    list[#list + 1] = string.format("f09 a0=%d,b1=%s", z.a0, on and "1" or "0")
    boiler_cmds(parent, list)
    -- 아무것도 안 바뀌어도(이미 켜짐) 앱이 멈추지 않게 지금 값을 다시 보낸다
    driver:call_with_delay(8, function()
      sent[table.concat({ device.id, "main", "switch", "switch" }, "|")] = nil
      refresh_zone(parent, key)
    end)
  end
end

local function set_heating_setpoint(_, device, cmd)
  local parent = parent_of(device)
  local v = math.floor(cmd.args.setpoint + 0.5)
  local z = ZONES[device.parent_assigned_child_key]
  if z then
    v = math.max(10, math.min(z.max, v))
    return boiler_cmds(parent, { string.format("f09 a0=%d,d4=%02X", z.a0, v) })
  end
  local m = hub_state(parent).main
  if cmd.component ~= "heat" or not m then return end
  if m.heat_mode == "실내" then
    boiler_cmds(parent, { string.format("f06 d4=%02X", snap(v, ROOM_TEMPS, m.room_set)) })
  else
    boiler_cmds(parent, { string.format("f06 d2=%02X", snap(v, ONDOL_TEMPS, m.ondol_set)) })
  end
end

local function set_mode(_, device, cmd)
  if cmd.component ~= "heat" then return end
  local parent = parent_of(device)
  local m = hub_state(parent).main or {}
  local want = cmd.args.mode
  -- 앱처럼: 외출 해제는 따로 보내고, 그다음 난방 켜기/끄기
  if want == "외출" then return boiler_cmds(parent, { "f06 b3=1" }) end
  local list = {}
  if m.outing then list[#list + 1] = "f06 b3=0" end
  list[#list + 1] = "f06 b1=" .. (want == "난방" and "1" or "0")
  boiler_cmds(parent, list)
end

local function set_heat_type(_, device, cmd)
  boiler_cmds(parent_of(device), { "f06 b4=" .. (cmd.args.heatType == "실내" and "1" or "0") })
end

local function set_hw_setpoint(_, device, cmd)
  local parent = parent_of(device)
  local m = hub_state(parent).main or {}
  local v = snap(math.floor(cmd.args.setpoint + 0.5), HW_TEMPS, m.hw_set)
  boiler_cmds(parent, { string.format("f06 d1=%02X,df=0", v) })
end

local function do_refresh(_, device)
  local parent = parent_of(device)
  send_text(parent, "refresh")
  refresh_all(parent)
end

-- ------------------------------------------------------------------ 수명

local function device_init(_, device)
  if is_child(device) then
    local parent = device:get_parent_device()
    if parent then refresh_all(parent) end
    return
  end
  -- 허브: 지금 상태를 다시 보내 달라고 한다(드라이버 재시작 후 자식 기기 갱신)
  driver:call_with_delay(5, function() send_text(device, "refresh") end)
end

local driver_template = {
  supported_capabilities = {
    capabilities.temperatureMeasurement,
    capabilities.refresh,
  },
  zigbee_handlers = {
    cluster = {
      [CLUSTER] = {
        [CMD_STATE] = on_state,
        [CMD_LINKS] = on_links,
      },
    },
  },
  capability_handlers = {
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
  },
  lifecycle_handlers = {
    init = device_init,
    added = device_init,
  },
}

defaults.register_for_default_handlers(driver_template, driver_template.supported_capabilities)

driver = ZigbeeDriver("lynnai-hub", driver_template)
driver:run()

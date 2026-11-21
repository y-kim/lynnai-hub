-- LYNNAI 허브 Zigbee Edge 드라이버: Zigbee 그릇만 여기, 보일러·각방·센서 로직은 core.lua.
--
-- 허브와는 전용 클러스터 0xFC31 로 주고받는다(펌웨어 hub_zb_rinnai.c).
-- 명령 id = 메시지 번호, 페이로드 = 내용 (번호·내용은 core.lua 맨 위).
local capabilities = require "st.capabilities"
local ZigbeeDriver = require "st.zigbee"
local defaults = require "st.zigbee.defaults"
local messages = require "st.zigbee.messages"
local zcl_messages = require "st.zigbee.zcl"
local data_types = require "st.zigbee.data_types"
local generic_body = require "st.zigbee.generic_body"
local zb_const = require "st.zigbee.constants"
local core = require "core"

local CLUSTER = 0xFC31
local CMD_REQUEST = 0x00
local HUB_EP = 10
local TEMP_CLUSTER = 0x0402      -- 표준 Zigbee 온도 클러스터(허브 칩 온도)

-- 허브 칩 온도는 main 이 아니라 'chip' 하위 컴포넌트로 보낸다(본체 타일을 온도계로 만들지 않게).
local function on_chip_temp(_, device, value)
  if value and value.value then
    device:emit_component_event(device.profile.components.chip,
      require("st.capabilities").temperatureMeasurement.temperature({ value = value.value / 100.0, unit = "C" }))
  end
end

local driver

local function send(parent, text)
  local header = zcl_messages.ZclHeader({ cmd = data_types.ZCLCommandId(CMD_REQUEST) })
  header.frame_ctrl:set_cluster_specific()
  header.frame_ctrl:set_disable_default_response()
  local addr = messages.AddressHeader(zb_const.HUB.ADDR, zb_const.HUB.ENDPOINT, parent:get_short_address(),
    HUB_EP, zb_const.HA_PROFILE_ID, CLUSTER)
  local body = zcl_messages.ZclMessageBody({ zcl_header = header, zcl_body = generic_body.GenericBody(text) })
  parent:send(messages.ZigbeeMessageTx({ address_header = addr, body = body }))
end

-- 허브 끊김 감시. 허브는 20~30초마다 생존 신호(0x0A)를 보낸다 — 90초 동안 아무것도 안 오면 사라진 것으로 본다.
-- 생존 신호가 없는 옛 펌웨어는 5분마다 연결 목록만 다시 보내므로 11분을 기다린다(한 번이라도 0x0A 를 받으면 빠른 쪽).
-- 끊기면 보일러·방에 "연결 끊김" 글자를 쓴다. 온라인/오프라인 배지는 SmartThings 가 Zigbee 무선 단에서 직접
-- 정하고(약 30분) 드라이버의 offline() 요청은 무시되므로(2026-11-21 실측), 배지는 그 판단을 기다린다.
local LOST_S, LOST_FAST_S = 660, 90
local last_rx, lost, has_alive = {}, {}, {}

local function from_hub(cmd)
  return function(_, parent, zb_rx)
    last_rx[parent.id] = os.time()
    if cmd == 0x0A then has_alive[parent.id] = true end
    lost[parent.id] = nil
    local ok, err = pcall(core.on_message, parent, cmd, zb_rx.body.zcl_body.body_bytes)
    if not ok then require("log").error("허브 메시지 처리 실패: " .. tostring(err)) end
  end
end

local function watch_hubs()
  for _, d in ipairs(driver:get_devices()) do
    if not core.is_child(d) then
      local t = last_rx[d.id]
      if not t then
        last_rx[d.id] = os.time()                  -- 드라이버 시작 시점부터 잰다
      elseif not lost[d.id] and os.time() - t > (has_alive[d.id] and LOST_FAST_S or LOST_S) then
        lost[d.id] = true
        require("log").warn("허브에서 " .. (os.time() - t) .. "초 동안 소식 없음 — 기기들에 연결 끊김 표시")
        core.hub_lost(d)
      end
    end
  end
end

local driver_template = {
  supported_capabilities = {
    capabilities.refresh,
  },
  zigbee_handlers = {
    attr = {
      [TEMP_CLUSTER] = { [0x0000] = on_chip_temp },   -- 칩 온도 → chip 컴포넌트
    },
    cluster = {
      [CLUSTER] = {
        [0x01] = from_hub(0x01),
        [0x02] = from_hub(0x02),
        [0x03] = from_hub(0x03),
        [0x04] = from_hub(0x04),
        [0x05] = from_hub(0x05),
        [0x06] = from_hub(0x06),
        [0x07] = from_hub(0x07),
        [0x08] = from_hub(0x08),
        [0x09] = from_hub(0x09),
        [0x0A] = from_hub(0x0A),   -- 생존 신호: 받은 시각만 갱신(처리할 내용 없음)
      },
    },
  },
  capability_handlers = core.capability_handlers,
  lifecycle_handlers = {
    init = core.device_init,
    added = core.device_init,
    infoChanged = core.info_changed,
  },
}

defaults.register_for_default_handlers(driver_template, driver_template.supported_capabilities)

driver = ZigbeeDriver("lynnai-hub", driver_template)
core.setup(driver, {
  send = send,
  -- 허브: 지금 상태를 다시 보내 달라고 한다(드라이버 재시작 후 자식 기기 갱신)
  on_hub_init = function(device) driver:call_with_delay(5, function() core.request_refresh(device) end) end,
})
driver:call_on_schedule(15, watch_hubs, "lynnai-hub-watch")
driver:run()

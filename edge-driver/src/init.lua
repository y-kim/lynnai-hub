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

local function from_hub(cmd)
  return function(_, parent, zb_rx) core.on_message(parent, cmd, zb_rx.body.zcl_body.body_bytes) end
end

local driver_template = {
  supported_capabilities = {
    capabilities.temperatureMeasurement,
    capabilities.refresh,
  },
  zigbee_handlers = {
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
driver:run()

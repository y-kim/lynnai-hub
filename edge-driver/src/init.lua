-- LYNNAI 허브 Zigbee Edge 드라이버.
-- 1단계: 허브(ESP32-C6)가 보내는 온도(ZCL Temperature Measurement)만 표시해 드라이버 경로를 검증한다.
-- 다음 단계에서 보일러·각방(Thermostat 클러스터 등)으로 확장.
local capabilities = require "st.capabilities"
local ZigbeeDriver = require "st.zigbee"
local defaults = require "st.zigbee.defaults"

local driver_template = {
  supported_capabilities = {
    capabilities.temperatureMeasurement,
    capabilities.refresh,
  },
}

defaults.register_for_default_handlers(driver_template, driver_template.supported_capabilities)

local driver = ZigbeeDriver("lynnai-hub", driver_template)
driver:run()

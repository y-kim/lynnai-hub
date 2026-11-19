-- LYNNAI 허브 LAN Edge 드라이버: LAN 그릇만 여기, 보일러·각방·센서 로직은 core.lua(Zigbee 드라이버와 공용).
--
-- 찾기: SSDP M-SEARCH (ST: urn:lynnai-com:device:hub:1) → 허브가 LYNNAI-ID / LYNNAI-PORT 로 답한다.
-- 연결: TCP 줄 단위(펌웨어 hub_lan.c)
--   → "hello <토큰>"   ← "ok <id>" | "denied"(허브 버튼 3초 누른 뒤 3분 안에 다시)
--   ← "<명령 16진 2자리> <내용 16진>"   → "00 <명령 텍스트 16진>"   ← "ping"(30초마다)
-- 토큰은 기기마다 한 번 만들어 저장한다. 허브는 처음 붙는 드라이버를 믿고 기억한다.
local Driver = require "st.driver"
local capabilities = require "st.capabilities"
local cosock = require "cosock"
local socket = cosock.socket
local log = require "log"
local core = require "core"

local SSDP_ST = "urn:lynnai-com:device:hub:1"
local DNI_PREFIX = "lynnai-hub-"
local status_cap = capabilities["connectbook02461.boilerStatus"]

local driver
local conns = {}          -- [device.id] = { alive = bool, sock = tcp|nil }
local seen = {}           -- 찾기에서 본 허브: [id] = { ip, port }

local function hex(s) return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end)) end
local function unhex(h) return (h:gsub("%x%x", function(x) return string.char(tonumber(x, 16)) end)) end

-- ------------------------------------------------------------------ 찾기

local function ssdp_search(wait)
  local found = {}
  local u = socket.udp()
  if not u then return found end
  u:setsockname("0.0.0.0", 0)
  local msg = table.concat({
    "M-SEARCH * HTTP/1.1", "HOST: 239.255.255.250:1900", 'MAN: "ssdp:discover"', "MX: 2", "ST: " .. SSDP_ST, "", "",
  }, "\r\n")
  u:sendto(msg, "239.255.255.250", 1900)
  local deadline = socket.gettime() + wait
  while true do
    local left = deadline - socket.gettime()
    if left <= 0 then break end
    u:settimeout(left)
    local data, ip = u:receivefrom()
    if not data then break end
    local id = data:match("\n[Ll][Yy][Nn][Nn][Aa][Ii]%-[Ii][Dd]:%s*(%w+)")
    local port = tonumber(data:match("\n[Ll][Yy][Nn][Nn][Aa][Ii]%-[Pp][Oo][Rr][Tt]:%s*(%d+)") or "")
    if id and port then
      found[id] = { ip = ip, port = port }
      seen[id] = found[id]
    end
  end
  u:close()
  return found
end

local function find_device(dni)
  for _, d in ipairs(driver:get_devices()) do
    if d.device_network_id == dni then return d end
  end
end

local function discovery(_, _, should_continue)
  for _ = 1, 3 do
    for id, a in pairs(ssdp_search(3)) do
      if not find_device(DNI_PREFIX .. id) then
        log.info(string.format("허브 찾음 %s (%s:%d)", id, a.ip, a.port))
        driver:try_create_device({
          type = "LAN", device_network_id = DNI_PREFIX .. id, label = "LYNNAI Hub (LAN)",
          profile = "lynnai-hub-lan", manufacturer = "Lynnai", model = "LYNNAI-HUB", vendor_provided_label = "LYNNAI Hub",
        })
      end
    end
    if not should_continue() then return end
  end
end

-- ------------------------------------------------------------------ 연결

local function token_of(device)
  local t = device:get_field("token")
  if not t then
    local r = {}
    for i = 1, 16 do r[i] = string.format("%02x", math.random(0, 255)) end
    t = table.concat(r)
    device:set_field("token", t, { persist = true })
  end
  return t
end

local function show(device, text)
  device:emit_event(status_cap.status(text, { state_change = true }))
end

local function run(device)
  local id = device.device_network_id:sub(#DNI_PREFIX + 1)
  local backoff = 5
  while conns[device.id] and conns[device.id].alive do
    local ip, port = device:get_field("ip"), device:get_field("port")
    if not ip then
      local a = seen[id] or ssdp_search(3)[id]
      if a then
        ip, port = a.ip, a.port
        device:set_field("ip", ip, { persist = true })
        device:set_field("port", port, { persist = true })
      end
    end
    local c = ip and socket.tcp()
    local connected = false
    if c then
      c:settimeout(5)
      connected = c:connect(ip, port)
    end
    if connected then
      c:settimeout(10)
      c:send("hello " .. token_of(device) .. "\n")
      local first = c:receive("*l")
      if first and first:match("^ok") then
        log.info(string.format("허브 연결됨 %s:%d", ip, port))
        conns[device.id].sock = c
        backoff = 5
        device:online()
        show(device, "연결됨")
        c:settimeout(90)                          -- 허브가 30초마다 ping
        while conns[device.id] and conns[device.id].alive do
          local l = c:receive("*l")
          if not l then break end
          local cmd, body = l:match("^(%x%x) (%x*)$")
          if cmd then core.on_message(device, tonumber(cmd, 16), unhex(body)) end
        end
        conns[device.id].sock = nil
        log.warn("허브 연결 끊김")
        core.hub_lost(device)
        show(device, "연결 끊김")
      elseif first == "denied" then
        log.warn("허브가 거절함: 허브 버튼을 3초 눌러 페어링 창을 연 뒤 새로고침")
        show(device, "페어링 필요: 허브 버튼 3초")
        backoff = 30
      end
      c:close()
    else
      if c then c:close() end
      -- IP 가 바뀌었을 수 있다: 다음엔 다시 찾는다
      device:set_field("ip", nil, { persist = true })
      seen[id] = nil
      show(device, "허브 못 찾음")
    end
    socket.sleep(backoff)
    backoff = math.min(backoff * 2, 60)
  end
end

local function send(parent, text)
  local c = conns[parent.id] and conns[parent.id].sock
  if not c then
    log.warn("허브와 연결 안 됨 — 명령 버림: " .. text)
    return
  end
  c:send("00 " .. hex(text) .. "\n")
end

local function start(device)
  if conns[device.id] then return end
  conns[device.id] = { alive = true }
  cosock.spawn(function() run(device) end, "lynnai-lan-" .. device.id)
end

local function removed(_, device)
  if core.is_child(device) then return end
  local c = conns[device.id]
  conns[device.id] = nil
  if c and c.sock then c.sock:close() end
end

math.randomseed(os.time())

driver = Driver("lynnai-hub-lan", {
  discovery = discovery,
  capability_handlers = core.capability_handlers,
  lifecycle_handlers = {
    init = core.device_init,
    added = core.device_init,
    infoChanged = core.info_changed,
    removed = removed,
  },
})
core.setup(driver, { send = send, on_hub_init = start, suffix = " LAN" })
driver:run()

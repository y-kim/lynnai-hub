-- LYNNAI 허브 LAN Edge 드라이버: LAN 그릇만 여기, 보일러·각방·센서 로직은 core.lua(Zigbee 드라이버와 공용).
--
-- 찾기: SSDP M-SEARCH (ST: urn:lynnai-com:device:hub:1) → 허브가 LYNNAI-ID / LYNNAI-PORT 로 답한다.
-- 연결: TCP 줄 단위(펌웨어 hub_lan.c)
--   → "hello <토큰>"   ← "ok <id>" | "denied"(허브 버튼 짧게 누른 뒤 3분 안에 다시)
--   ← "<명령 16진 2자리> <내용 16진>"   → "00 <명령 텍스트 16진>"   ← "ping"(30초마다)
-- 토큰은 기기마다 한 번 만들어 저장한다. 허브는 버튼을 짧게 누른 뒤 3분(페어링 창) 안에 온 새 토큰만 기억한다.
-- 허브가 "ok <id>" 로 답한 id 가 찾은 허브의 id 와 같아야 붙는다(다른 기기가 허브인 척 답해도 명령을 보내지 않게).
local Driver = require "st.driver"
local capabilities = require "st.capabilities"
local cosock = require "cosock"
local socket = cosock.socket
local log = require "log"
local core = require "core"

local SSDP_ST = "urn:lynnai-com:device:hub:1"
local DNI_PREFIX = "lynnai-hub-"
local status_cap = capabilities["connectbook02461.boilerStatus"]
local room_cap = capabilities["connectbook02461.roomStatus"]

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

-- 토큰 만들기: Edge 에는 암호 난수가 없어, 밖에서 알 수 없는 값(기기 UUID — SmartThings 가 무작위로 준다)과
-- 시각·주소·메모리 상태를 섞는다. math.random(os.time 시드)만 쓰면 시각을 짐작해 토큰을 맞힐 수 있다.
local function mix32(h, str)
  for i = 1, #str do
    h = ((h ~ str:byte(i)) * 16777619) & 0xFFFFFFFF          -- FNV-1a
    h = ((h << 13) | (h >> 19)) & 0xFFFFFFFF
  end
  return h
end
local function new_token(device)
  local pool = table.concat({ tostring(device.id), tostring(device.device_network_id), tostring(socket.gettime()),
    tostring(os.clock()), tostring({}), tostring(collectgarbage("count")), tostring(math.random()) }, "|")
  local out = {}
  local h = 2166136261
  for i = 1, 4 do
    h = mix32(h, pool .. "#" .. i .. "#" .. tostring(socket.gettime()))
    out[i] = string.format("%08x", h)
  end
  return table.concat(out)
end

local function token_of(device)
  local t = device:get_field("token")
  if not t then
    t = new_token(device)
    device:set_field("token", t, { persist = true })
  end
  return t
end

-- 기기 목록(boilerStatus)과 상세 화면(roomStatus) 두 곳에 같은 글
local function show(device, text)
  device:emit_event(status_cap.status(text, { state_change = true }))
  device:emit_event(room_cap.roomStatus(text, { state_change = true }))
end

local function run(device)
  local id = device.device_network_id:sub(#DNI_PREFIX + 1)
  local backoff = 5
  -- 허브와 끊겼으면(처음부터 못 붙은 경우 포함) 허브 기기와 자식 기기를 한 번 오프라인으로. 다시 붙으면 online()
  local is_lost = false
  local function mark_lost()
    if is_lost then return end
    is_lost = true
    core.hub_lost(device)
    device:offline()
  end
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
      local got = first and first:match("^ok (%w+)$")
      if first and first:match("^ok") and got ~= id then
        -- 다른 허브(또는 허브인 척하는 기기)가 답함: 주소를 잊고 다시 찾는다
        log.warn(string.format("허브 id 다름(%s ≠ %s) — 끊고 다시 찾음", tostring(got), id))
        device:set_field("ip", nil, { persist = true })
        seen[id] = nil
        show(device, "허브 못 찾음")
        mark_lost()
      elseif got and not conns[device.id] then
        c:close()                                 -- 인사하는 사이 허브 기기가 지워짐
        return
      elseif got then
        log.info(string.format("허브 연결됨 %s:%d", ip, port))
        conns[device.id].sock = c
        backoff = 5
        is_lost = false
        device:online()
        show(device, "연결됨")
        c:settimeout(90)                          -- 허브가 30초마다 ping
        while conns[device.id] and conns[device.id].alive do
          local l = c:receive("*l")
          if not l then break end
          local cmd, body = l:match("^(%x%x) (%x*)$")
          if cmd and #body % 2 == 0 then
            -- 메시지 하나가 깨져도(잘림 등) 연결 코루틴이 죽지 않게
            local ok, err = pcall(core.on_message, device, tonumber(cmd, 16), unhex(body))
            if not ok then log.error("허브 메시지 처리 실패: " .. tostring(err)) end
          end
        end
        if not conns[device.id] then               -- 그 사이 허브 기기가 지워짐: 지워진 기기는 더 건드리지 않는다
          c:close()
          return
        end
        conns[device.id].sock = nil
        log.warn("허브 연결 끊김")
        show(device, "연결 끊김")
        mark_lost()
      elseif first == "busy" then
        log.warn("허브에 이미 드라이버 2개가 붙어 있음")
        show(device, "허브 연결 가득")
        mark_lost()
        backoff = 30
      elseif first == "denied" then
        log.warn("허브가 거절함: 허브 버튼을 짧게 눌러 페어링 창을 연 뒤 새로고침")
        show(device, "페어링 필요: 허브 버튼 짧게")
        mark_lost()
        backoff = 30
      end
      c:close()
    else
      if c then c:close() end
      -- IP 가 바뀌었을 수 있다: 다음엔 다시 찾는다
      device:set_field("ip", nil, { persist = true })
      seen[id] = nil
      show(device, "허브 못 찾음")
      mark_lost()
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

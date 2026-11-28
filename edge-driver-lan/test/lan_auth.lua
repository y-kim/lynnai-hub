-- LAN 드라이버(../src/init.lua)의 연결 루프를 가짜 소켓·SSDP 위에서 돌린다. 시나리오(SCEN)는 run_lan_auth.py 가 정한다.
-- 가짜 상대: legacy = hello2 를 모르는 옛 펌웨어(닫음), denied = 무엇이든 거절, v2 = 토큰을 알고 증명함.
-- 붙은 뒤엔 receive 가 nil 이라 바로 끊겨 루프가 돈다. sleep 이 dead_after 번 불리면 기기를 지워 루프를 끝낸다.
-- 돌려주는 값: 보낸 줄들("주소 줄"), 끝난 뒤 home·ip 필드.
local SRC = ...
package.path = SRC .. "/?.lua;" .. package.path
local sent = {}            -- { addr, line }
local opts_cap, on_init
local dead_after = 14
local sleeps = 0
local function cap() return setmetatable({ ID = "x" }, { __index = function() return function() return {} end end }) end
package.preload["st.driver"] = function() return function(_, opts) opts_cap = opts; return { get_devices = function() return {} end, run = function() end } end end
package.preload["st.capabilities"] = function() return setmetatable({}, { __index = function() return cap() end }) end
package.preload["log"] = function() return { info = function() end, warn = function(m) LOGS[#LOGS+1] = m end, error = function(m) LOGS[#LOGS+1] = "E " .. m end } end
package.preload["core"] = function() return {
  setup = function(_, o) on_init = o.on_hub_init end, hub_lost = function() end, on_message = function() end,
  is_child = function() return false end, capability_handlers = {}, device_init = function() end, info_changed = function() end } end
LOGS = {}
local hmac_mod = dofile(SRC .. "/hmac.lua")
package.loaded["hmac"] = hmac_mod
local function tcp()
  local s = { addr = nil, q = {}, peer = nil }
  function s:settimeout() end
  function s:connect(ip, port)
    self.addr = ip .. ":" .. port
    self.peer = SCEN.peers[self.addr]
    return self.peer ~= nil
  end
  function s:send(l)
    sent[#sent+1] = { self.addr, l }
    local p = self.peer
    local line = l:gsub("\n$", "")
    if line:match("^hello2 ") then
      if p.kind == "legacy" then self.closed = true
      else self.cn = line:match("^hello2 (%x+)"); self.sn = string.rep("ab", 16); self.q[#self.q+1] = "chal " .. self.sn end
    elseif line:match("^auth ") then
      if p.kind == "denied" then self.q[#self.q+1] = "denied"
      else
        local cp = line:match("^auth (%x+)")
        if p.token and hmac_mod.same(cp, hmac_mod.proof(p.token, "c", self.cn, self.sn)) then
          self.q[#self.q+1] = "ok " .. p.id .. " " .. hmac_mod.proof(p.token, "s", self.sn, self.cn, p.id)
        else self.q[#self.q+1] = "denied" end
      end
    elseif line:match("^hello ") then
      local tok = line:match("^hello (%x+)")
      if p.kind ~= "denied" and p.token == tok then self.q[#self.q+1] = "ok " .. p.id else self.q[#self.q+1] = "denied" end
    end
    return #l
  end
  function s:receive()
    if self.closed then return nil end
    return table.remove(self.q, 1)
  end
  function s:close() self.closed = true end
  return s
end
local function udp()
  local u, done = {}, false
  function u:setsockname() end
  function u:sendto() end
  function u:settimeout() end
  function u:receivefrom()
    if done or not SCEN.ssdp then return nil end
    done = true
    local ip, port = SCEN.ssdp:match("^(.+):(%d+)$")
    return "HTTP/1.1 200 OK\r\nLYNNAI-ID: " .. SCEN.id .. "\r\nLYNNAI-PORT: " .. port .. "\r\n", ip
  end
  function u:close() end
  return u
end
local t = 0
package.preload["cosock"] = function() return {
  socket = { tcp = tcp, udp = udp, gettime = function() t = t + 0.001; return t end,
    sleep = function() sleeps = sleeps + 1; if sleeps >= dead_after then opts_cap.lifecycle_handlers.removed(nil, DEV) end end },
  spawn = function(f) f() end } end
local fields = {}
for k, v in pairs(SCEN.fields) do fields[k] = v end
DEV = { id = "dev1", device_network_id = "lynnai-hub-" .. SCEN.id,
  get_field = function(_, k) return fields[k] end, set_field = function(_, k, v) fields[k] = v end,
  online = function() end, offline = function() end, emit_event = function() end,
  supports_capability_by_id = function() return false end }
dofile(SRC .. "/init.lua")
on_init(DEV)
local out = {}
for _, e in ipairs(sent) do out[#out+1] = e[1] .. " " .. e[2]:gsub("\n", "") end
return table.concat(out, "\n"), tostring(fields.home), tostring(fields.ip)

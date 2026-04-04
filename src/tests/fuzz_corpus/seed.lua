local socket = require("socket")
local json = require("cjson")

local Config = {}
Config.__index = Config

function Config.new(host, port)
    local self = setmetatable({}, Config)
    self.host = host or "localhost"
    self.port = port or 8080
    return self
end

local function start_server(config)
    local server = socket.tcp()
    server:bind(config.host, config.port)
    server:listen(32)
    print("Listening on " .. config.host .. ":" .. config.port)
    return server
end

local DEFAULT_TIMEOUT = 30

return {
    Config = Config,
    start_server = start_server,
    DEFAULT_TIMEOUT = DEFAULT_TIMEOUT,
}

-- SPDX-License-Identifier: BSD-2-Clause
-- Logging, files, the state file and calls to the other services

local ffi = require "ffi"

ffi.cdef [[
void openlog(const char *ident, int option, int facility);
void syslog(int priority, const char *format, ...);
int isatty(int fd);
]]

local M = {}

local LOG_PID, LOG_DAEMON = 0x01, 3 * 8
local LOG_ERR, LOG_INFO = 3, 6
local IDENT = "osirisd"		-- openlog keeps the pointer

-- what the MCU reported, for the next start: the model (the host name at
-- boot, /lib/preinit/30_hostname) and the listening mode, which the MCU
-- keeps across restarts but does not resend. One key=value per line.
local STATE = "/data/osiris/state"

local conn

-- to the terminal when run from one, to syslog when run as a daemon
local TERMINAL = ffi.C.isatty(2) == 1

local function say(prio, fmt, ...)
	local s = string.format(fmt, ...)
	if TERMINAL then
		io.stderr:write(s, "\n")
	else
		ffi.C.syslog(prio, "%s", s)
	end
end

function M.open_log()
	ffi.C.openlog(IDENT, LOG_PID, LOG_DAEMON)
end

function M.log(fmt, ...)
	say(LOG_INFO, fmt, ...)
end

function M.err(fmt, ...)
	say(LOG_ERR, fmt, ...)
end

-- s as one shell word
function M.quote(s)
	return "'" .. s:gsub("'", "'\\''") .. "'"
end

-- "v1.2.3" and "v1.2.3~g<hash>" -> 1, 2, 3
local function numbers(v)
	local a, b, c = (v or ""):match("^v(%d+)%.(%d+)%.(%d+)")
	if a then
		return { tonumber(a), tonumber(b), tonumber(c) }
	end
end

-- version v is newer than w; a build after a release (v1.2.3~g...) is not
-- newer than it
function M.newer(v, w)
	local a, b = numbers(v), numbers(w)
	if not a then
		return false
	end
	if not b then
		return true
	end
	for i = 1, 3 do
		if a[i] ~= b[i] then
			return a[i] > b[i]
		end
	end
	return false
end

-- the build: VERSION in /etc/os-release
function M.os_version()
	return (M.read_file("/etc/os-release") or ""):match('\nVERSION="?([^"\n]*)') or ""
end

function M.read_file(path)
	local f = io.open(path, "r")
	if not f then
		return nil
	end
	local s = f:read("*a")
	f:close()
	return s
end

function M.write_file(path, text)
	local tmp = path .. ".tmp"
	local f = io.open(tmp, "w")
	if not f then
		return false
	end
	f:write(text)
	f:close()
	return os.rename(tmp, path)
end

function M.load_state()
	local t = {}

	for k, v in (M.read_file(STATE) or ""):gmatch("([%w_]+)=([^\n]*)") do
		t[k] = v
	end

	return t
end

-- merge into the state file, rewritten (atomically) only when it changes
function M.save_state(changes)
	local t = M.load_state()
	local keys, lines = {}, {}

	for k, v in pairs(changes) do
		t[k] = tostring(v)
	end

	for k in pairs(t) do
		keys[#keys + 1] = k
	end

	table.sort(keys)
	for _, k in ipairs(keys) do
		lines[#lines + 1] = k .. "=" .. t[k] .. "\n"
	end

	local text = table.concat(lines)
	if text ~= M.read_file(STATE) then
		M.write_file(STATE, text)
	end
end

function M.set_conn(c)
	conn = c
end

function M.call(object, method, args)
	local res = conn:call(object, method, args or {})
	if not res and method ~= "set" and method ~= "identity" and method ~= "network" and
	   method ~= "power" and method ~= "update" and method ~= "usb" and
	   method ~= "display" and method ~= "meter" and method ~= "input" and method ~= "volume" and method ~= "stream" then
		M.err("%s %s: no answer", object, method)
	end
	return res
end

return M

-- SPDX-License-Identifier: BSD-2-Clause
-- AirPlay: shairport-sync's metadata pipe gives the track and the play
-- state; they go to the front panel, and a pause or skip makes pcmd drop
-- what it has queued. The binding has no fd watch, so a timer polls it.

local bit = require "bit"
local ffi = require "ffi"
local uloop = require "uloop"
local source = require "source"
local events = require "events"
local util = require "util"

ffi.cdef [[
int open(const char *path, int flags, ...);
long read(int fd, void *buf, unsigned long n);
int mkfifo(const char *path, unsigned int mode);
int kill(int pid, int sig);
]]

local call = util.call

local M = {}

local META_PIPE = "/var/run/airplay-meta"
local META_MS = 100
local O_RDWR, O_NONBLOCK, SIGUSR1 = 2, 0x800, 10
local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local meta_fd, meta_buf, meta_timer = -1, ""
local track = { state = "stop" }
local meta_chunk = ffi.new("char[4096]")

local function base64(s)
	local out, bits, n = {}, 0, 0
	for c in s:gmatch("[%w+/]") do
		bits = bit.bor(bit.lshift(bits, 6), B64:find(c, 1, true) - 1)
		n = n + 6
		if n >= 8 then
			n = n - 8
			out[#out + 1] = string.char(bit.band(bit.rshift(bits, n), 0xff))
		end
	end
	return table.concat(out)
end

function M.set_display(mode)
	if mode then
		call("mcu", "display", { mode = mode })
	end
end

function M.show_track()
	call("mcu", "display", { title = track.title or "", artist = track.artist or "",
				 album = track.album or "", state = track.state })
end

local function pcmd_flush()
	local pid = tonumber(util.read_file("/var/run/pcmd.pid") or "")
	if pid then
		ffi.C.kill(pid, SIGUSR1)
	end
end

local META_STATES = { pbeg = "play", prsm = "play", pfls = "pause", pend = "stop" }

-- The AirPlay slider sets the master volume: from the lowest step at the
-- bottom to airplay_max_db (state file, -40 dB unless set) at the top, so
-- a slider pushed up never blasts. At most one change every 100 ms.
local AIRPLAY_MAX_DB, AIRPLAY_RANGE_DB, VOLUME_MIN_DB, VOLUME_MS = -40, 30, -82, 100
local volume_want, volume_sent, volume_timer, volume_armed

local function volume_tick()
	volume_armed = false
	if volume_want and volume_want ~= volume_sent then
		volume_sent = volume_want
		call("mcu", "volume", { db = volume_want })
	end
end

local function airplay_volume(data)
	-- "airplay,volume,lowest,highest": the slider -30..0 dB, -144 muted
	local slider = tonumber(data:match("^([^,]+)"))
	if not slider then
		return
	end

	local top = tonumber(util.load_state().airplay_max_db or "") or AIRPLAY_MAX_DB
	top = top ~= top and AIRPLAY_MAX_DB or top > 0 and 0 or top < VOLUME_MIN_DB and VOLUME_MIN_DB or top
	slider = slider < -AIRPLAY_RANGE_DB and -AIRPLAY_RANGE_DB or slider > 0 and 0 or slider
	volume_want = math.floor(VOLUME_MIN_DB + (top - VOLUME_MIN_DB) *
				 (slider + AIRPLAY_RANGE_DB) / AIRPLAY_RANGE_DB + 0.5)
	volume_timer = volume_timer or uloop.timer(volume_tick)
	if not volume_armed then
		volume_armed = true
		volume_timer:set(VOLUME_MS)
	end
end

-- the knob moved it (mcu.volume, 0.01 dB): the next slider position counts again
local function volume_moved(volume)
	if volume_sent and volume ~= volume_sent * 100 then
		volume_sent = nil
	end
end

local function meta_item(kind, code, data)
	if kind == "core" and code == "minm" then
		track.title = data
	elseif kind == "core" and code == "asar" then
		track.artist = data
	elseif kind == "core" and code == "asal" then
		track.album = data
	elseif kind == "ssnc" and code == "pvol" then
		airplay_volume(data)
	elseif kind == "ssnc" and code == "mden" then
		M.show_track()
	elseif kind == "ssnc" and META_STATES[code] then
		-- a pause, skip or stop: what is queued is not wanted any more
		if code == "pfls" or code == "pend" then
			pcmd_flush()
		end
		-- a phone starts or resumes playing: the receiver goes to NET
		if META_STATES[code] == "play" and source.input_now ~= "network" then
			call("mcu", "input", { input = "net" })
		end
		if code == "pend" then
			track.title, track.artist, track.album = nil, nil, nil
		end
		track.state = META_STATES[code]
		M.show_track()
	end
end

local function unhex(h)
	return (h:gsub("%x%x", function(x) return string.char(tonumber(x, 16)) end))
end

local function meta_tick()
	if meta_fd < 0 then
		ffi.C.mkfifo(META_PIPE, tonumber("666", 8))
		-- read-write: no end of file while shairport-sync is not there
		meta_fd = ffi.C.open(META_PIPE, O_RDWR + O_NONBLOCK)
	end

	while meta_fd >= 0 do
		local n = tonumber(ffi.C.read(meta_fd, meta_chunk, 4096))
		if n <= 0 then
			break
		end
		meta_buf = meta_buf .. ffi.string(meta_chunk, n)
	end

	-- <item><type>hex</type><code>hex</code><length>n</length>[<data ...>b64</data>]</item>
	while true do
		local s, e = meta_buf:find("</item>", 1, true)
		if not s then
			break
		end
		local item = meta_buf:sub(1, e)
		meta_buf = meta_buf:sub(e + 1)
		local kind, code = item:match("<type>(%x+)</type><code>(%x+)</code>")
		if kind then
			meta_item(unhex(kind), unhex(code), base64(item:match("<data[^>]*>(.-)</data>") or ""))
		end
	end

	-- a stray partial item never grows without bound
	if #meta_buf > 65536 then
		meta_buf = ""
	end
	meta_timer:set(META_MS)
end

events.on("mcu.ready", function()
	M.set_display(util.load_state().display)
	M.show_track()
end)
-- DISPLAY on NET picked another mode: keep it
events.on("mcu.display", function(msg) util.save_state({ display = msg.mode }) end)
events.on("mcu.volume", function(msg)
	if msg.volume then
		volume_moved(msg.volume)
	end
end)

-- after uloop.init()
function M.start()
	meta_timer = uloop.timer(meta_tick)
	meta_timer:set(META_MS)
end

return M

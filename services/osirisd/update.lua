-- SPDX-License-Identifier: BSD-2-Clause
-- Updates: the USB sticks there are, the bundle on them or the release
-- netupdate found, and the steps of the receiver's update dialogue

local uloop = require "uloop"
local events = require "events"
local util = require "util"

local call, log, err = util.call, util.log, util.err
local read_file, quote = util.read_file, util.quote

local M = {}

local UPGRADE_LOG = "/tmp/osiris-upgrade.log"
local PROGRESS_MS = 4000	-- the MCU gives up after ~20 s without progress
-- osiris-upgrade's output -> percent
local INSTALL_STEPS = {
	{ "running ", 15 }, { " to kernel_", 20 }, { " to rootfs_", 30 },
	{ " to uboot_", 90 }, { "will be tried", 100 },
}

local media = {}		-- mounted USB storage, by directory
local bundle			-- { path, version } of an update on it, or the release
local net			-- netupdate's release, offered again when a stick's goes:
				-- also { size, fetch(done) }, fetched only once the user asks
local chosen			-- the bundle downloaded: the one installed, whatever comes since
local job			-- the running update step: its steps, percent, process
local progress_timer		-- made once: cancelling one frees its callback twice

function M.running_version()
	return util.os_version()
end

function M.set_usb()
	call("mcu", "usb", { present = next(media) ~= nil })
end

function M.set_media(dir, present)
	media[dir] = present or nil
	if not present and bundle and bundle.path:sub(1, #dir + 1) == dir .. "/" then
		log("update %s gone", bundle.path)
		bundle = net
	end

	M.set_usb()
end

-- answered at once if the MCU is waiting
local function offer(b)
	log("update %s (%s)", b.path, b.version)
	bundle = b
	-- the MCU waits ~2.5 min for an answer before it asks again
	local st = call("mcu", "status")

	if st and st.update_check then
		call("mcu", "update", { available = true })
		return true
	end
end

function M.set_bundle(path, version)
	if not path or not version or version == M.running_version() then
		return
	end
	-- of several on a stick, the newest
	if bundle and bundle ~= net and not util.newer(version, bundle.version) then
		return
	end

	offer({ path = path, version = version })
end

-- a stick's bundle comes first, unless the release is newer
function M.set_net_bundle(b)
	local stick = bundle ~= net and bundle
	net = b
	if stick and not util.newer(b.version, stick.version) then
		return
	end

	-- the MCU does not ask again by itself once the release is found
	if not offer(b) then
		call("mcu", "update", { ask = true })
	end
end

-- a bundle being read or installed
function M.busy()
	return job ~= nil
end

-- sticks mounted before we listened
function M.scan_media()
	for line in (read_file("/proc/mounts") or ""):gmatch("[^\n]+") do
		local dir = line:match("^%S+ (/media/%S+) ")
		if dir then
			media[dir] = true
		end
	end

	for dir in pairs(media) do
		local p = io.popen("ls " .. quote(dir) .. "/osiris-*.tar 2> /dev/null")
		for f in p:lines() do
			local v = io.popen("tar -xOf " .. quote(f) .. " VERSION 2> /dev/null")
			M.set_bundle(f, (v:read("*a") or ""):gsub("%s+$", ""))
			v:close()
		end
		p:close()
	end
end

-- the MCU asked before we listened (mcu status update_check)
function M.answer_check(st)
	if st and st.update_check and bundle then
		call("mcu", "update", { available = true })
	end
end

-- the percent the steps reach in UPGRADE_LOG, or a function gives, repeated
-- so that the MCU does not give up
local function progress_tick()
	if not job then
		return
	end

	if type(job.steps) == "function" then
		job.percent = math.max(job.percent, job.steps())
	else
		local out = read_file(UPGRADE_LOG) or ""

		for _, s in ipairs(job.steps) do
			if s[2] > job.percent and out:find(s[1], 1, true) then
				job.percent = s[2]
			end
		end
	end

	call("mcu", "update", { progress = job.percent })
	progress_timer:set(PROGRESS_MS)
end

local function start_job(steps)
	call("mcu", "update", { progress = 0 })
	job = { steps = steps, percent = 0 }
	progress_timer = progress_timer or uloop.timer(progress_tick)
	progress_timer:set(PROGRESS_MS)
end

-- run an update step, its output to UPGRADE_LOG
local function run_step(cmd, steps, done)
	os.remove(UPGRADE_LOG)
	start_job(steps)

	-- kept in job: uloop lists the process, and the binding does not keep it
	job.process = uloop.process("/bin/sh", { "-c", cmd .. " > " .. UPGRADE_LOG .. " 2>&1" },
				    { "PATH=/usr/sbin:/usr/bin:/sbin:/bin" }, function(ret)
		job = nil
		if ret ~= 0 then
			err("update failed: %s", (read_file(UPGRADE_LOG) or ""):gsub("%s+$", ""))
		end

		done(ret == 0)
	end)
end

local function file_size(path)
	local f = io.open(path, "r")
	local n = f and f:seek("end") or 0
	if f then
		f:close()
	end
	return n
end

local function downloaded(ok, b)
	chosen = ok and b or nil
	if not ok then
		call("mcu", "update", { result = "error" })
		return
	end

	call("mcu", "update", { progress = 100 })
	call("mcu", "update", { downloaded = file_size(b.path) })
end

-- a release is fetched, and its signature checked, only now; the stick is
-- read once in full before the MCU is told to go on
local function download()
	local b = bundle

	if not b or job then
		call("mcu", "update", { result = "error" })
		return
	end

	chosen = nil
	if b.fetch and not b.fetched then
		start_job(function()
			return b.size and b.size > 0 and math.min(99, math.floor(file_size(b.path) * 100 / b.size)) or 0
		end)
		b.fetch(function(ok)
			job = nil
			b.fetched = ok
			downloaded(ok, b)
		end)
	elseif b.fetch then
		downloaded(true, b)
	else
		log("reading %s", b.path)
		run_step("tar -tf " .. quote(b.path), {}, function(ok) downloaded(ok, b) end)
	end
end

-- the new slot boots when the user switches the receiver off and on again,
-- as the MCU asks after "Completed!"
local function install()
	local b = chosen

	-- a stick pulled out since
	if not b or job or file_size(b.path) == 0 then
		call("mcu", "update", { result = "error" })
		return
	end

	-- AirPlay and pcmd would take CPU time from it; they come back with
	-- the new slot or, if it fails, at once
	log("installing %s", b.version)
	os.execute("/etc/init.d/airplay stop; /etc/init.d/osiris stop pcmd")
	run_step("osiris-upgrade -n " .. quote(b.path), INSTALL_STEPS, function(ok)
		if not ok then
			os.execute("/etc/init.d/osiris start; /etc/init.d/airplay start")
		end
		-- the MCU takes the result, an error too, only after 100 %
		call("mcu", "update", { progress = 100 })
		call("mcu", "update", { result = ok and "ok" or "error" })
		if ok then
			bundle, net, chosen = nil, nil, nil
		end
	end)
end

function M.update(stage)
	if stage == "check" then
		-- unanswered, the MCU asks again later: the notice can still come
		if bundle then
			call("mcu", "update", { available = true })
		end
	elseif stage == "download" then
		download()
	elseif stage == "install" then
		install()
	end
end

events.on("mcu.ready", function() M.set_usb() end)
events.on("mcu.update", function(msg) M.update(msg.stage) end)
events.on("osiris.media", function(msg)
	if msg.dir then
		M.set_media(msg.dir, msg.present)
	end
end)
events.on("osiris.update", function(msg) M.set_bundle(msg.path, msg.version) end)

return M

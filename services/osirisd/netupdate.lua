-- SPDX-License-Identifier: BSD-2-Clause
-- Updates from the network: the newest OSIRIS release on GitHub, checked a
-- while after the clock is set and about once a day after that. A newer one
-- is offered like a bundle on a USB stick; only when the user downloads it
-- from the receiver's update dialogue is it fetched into /tmp and its
-- signature checked.

local uloop = require "uloop"
local events = require "events"
local update = require "update"
local util = require "util"

local log, err, read_file, quote = util.log, util.err, util.read_file, util.quote

local M = {}

-- the list, not releases/latest: the repository has dspboot releases too
local URL = "https://api.github.com/repos/ubis/osiris/releases?per_page=20"
local URL_FILE = "/data/osiris/update-url"	-- another release list, for tests
local KEYS = "/usr/share/osiris/keys"		-- usign keys, named by fingerprint
local SYNCED = "/run/ntp-synced"		-- the ntpd hook's mark
local DIR = "/tmp/osiris-net"			-- the release list, the fetched bundle
local RELEASES = DIR .. "/releases.json"
local MINUTE = 60 * 1000

local timer
local proc			-- the running fetch or check; the binding does not keep it
local found			-- the newest version offered, not offered again
local checked			-- a check was started, the first one is not needed

local function sh(cmd, done)
	proc = uloop.process("/bin/sh", { "-c", cmd .. " > /dev/null 2>&1" },
			     { "PATH=/usr/sbin:/usr/bin:/sbin:/bin" }, function(ret)
		proc = nil
		done(ret == 0)
	end)
end

local function fetch(url, file, done)
	sh("uclient-fetch -q -T 30 -O " .. quote(file) .. " " .. quote(url), done)
end

local function json(file, expr)
	local p = io.popen("jsonfilter -i " .. quote(file) .. " -e " .. quote(expr) .. " 2> /dev/null")
	local out = p:read("*a") or ""
	p:close()
	return (out:gsub("%s+$", ""))
end

local function lines(s)
	local t = {}
	for l in s:gmatch("[^\n]+") do
		t[#t + 1] = l
	end
	return t
end

-- the release's bundle and signature, the signature checked; update's
-- download step runs it once the user asks
local function get(version, tar_url, sig_url, done)
	local name = "osiris-" .. version .. ".tar"
	local tar, sig = DIR .. "/" .. name, DIR .. "/" .. name .. ".sig"

	local function fail(fmt, ...)
		err("update: " .. fmt, ...)
		os.remove(tar)
		os.remove(sig)
		done(false)
	end

	-- the check's own fetch shares proc
	if proc then
		return fail("an update check is running")
	end
	log("fetching %s", version)
	-- an older release fetched before
	os.execute("rm -f " .. DIR .. "/osiris-*")
	fetch(sig_url, sig, function(ok)
		if not ok then
			return fail("cannot fetch %s", sig_url)
		end
		fetch(tar_url, tar, function(ok)
			if not ok then
				return fail("cannot fetch %s", tar_url)
			end
			sh("usign -V -q -P " .. KEYS .. " -m " .. quote(tar) .. " -x " .. quote(sig), function(ok)
				if not ok then
					return fail("%s is not signed with the release key", name)
				end
				local p = io.popen("tar -xOf " .. quote(tar) .. " VERSION 2> /dev/null")
				local v = (p:read("*a") or ""):gsub("%s+$", "")
				p:close()
				if v ~= version then
					return fail("%s says it is %s", name, v)
				end
				done(true)
			end)
		end)
	end)
end

function M.check()
	checked = true
	timer:set((24 * 60 + math.random(0, 60)) * MINUTE)
	-- a fetch running, or a bundle being read or installed
	if proc or update.busy() then
		return
	end

	local url = (read_file(URL_FILE) or URL):gsub("%s+$", "")
	os.execute("mkdir -p " .. DIR)
	fetch(url, RELEASES, function(ok)
		if not ok then
			os.remove(RELEASES)
			return err("update check: cannot fetch %s", url)
		end
		local tags = lines(json(RELEASES, "@[*].tag_name"))
		local pre = lines(json(RELEASES, "@[*].prerelease"))
		local i, version
		for n, tag in ipairs(tags) do
			local v = tag:match("^osiris%-(v.*)")
			if v and pre[n] ~= "true" and (not version or util.newer(v, version)) then
				i, version = n - 1, v
			end
		end
		local name = "osiris-" .. (version or "") .. ".tar"
		local asset = "@[" .. (i or 0) .. "].assets[@.name=\"%s\"].%s"
		local tar_url = json(RELEASES, asset:format(name, "browser_download_url"))
		local sig_url = json(RELEASES, asset:format(name .. ".sig", "browser_download_url"))
		local size = tonumber(json(RELEASES, asset:format(name, "size")))
		os.remove(RELEASES)

		if not util.newer(version, update.running_version()) or version == found then
			return
		end
		if tar_url == "" or sig_url == "" then
			return err("update check: %s has no signed bundle", version)
		end
		found = version
		update.set_net_bundle({
			path = DIR .. "/" .. name, version = version, size = size,
			fetch = function(done) get(version, tar_url, sig_url, done) end,
		})
	end)
end

-- the first check, a few minutes to an hour after the clock is set, so that
-- receivers powered on together do not ask GitHub at once
local function synced()
	if not checked then
		timer:set(math.random(5, 60) * MINUTE)
	end
end

-- not the clock: before ntpd sets it, it is the same on every receiver
local function seed()
	local f = io.open("/dev/urandom", "rb")
	local s = f and f:read(4) or ""
	local n = 0
	if f then
		f:close()
	end
	for i = 1, #s do
		n = n * 256 + s:byte(i)
	end
	math.randomseed(n)
end

function M.start()
	seed()
	timer = uloop.timer(M.check)
	local f = io.open(SYNCED)
	if f then
		f:close()
		synced()
	end
end

events.on("osiris.time", function() synced() end)
events.on("osiris.check", function() M.check() end)

return M

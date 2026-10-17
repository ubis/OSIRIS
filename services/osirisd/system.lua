-- SPDX-License-Identifier: BSD-2-Clause
-- The board's side: the version the receiver shows, the host name, the
-- network menu, power off and factory reset

local bit = require "bit"
local events = require "events"
local util = require "util"

local call, log, read_file = util.call, util.log, util.read_file

local M = {}

local OWN_HOSTNAME = "/data/etc/hostname"
local IFACE = "eth0"

-- the version the receiver shows for the DSP: "OSIRIS <build>"
-- the receiver shows 14 characters: "OSIRIS v0.1.0" for a release, the
-- commit for a build in between ("v0.1.0~g1de0787" -> "OSIRIS 1de0787")
function M.set_identity()
	local build = util.os_version()
	local shown = "OSIRIS " .. build

	if #shown > 14 then
		shown = "OSIRIS " .. (build:match("~g(%x+)") or build)
	end

	call("mcu", "identity", { dsp_version = shown:sub(1, 14) })
end

-- name the system after the receiver model the MCU reports
function M.set_model(m)
	if not m or not m.model or m.model == "" then
		return
	end

	util.save_state({ make = m.make, model = m.model, code = m.code })

	-- U-Boot boots the kernel FIT's configuration named after the model
	-- if it has one; written once, the environment is in the flash. Only
	-- when booted from a slot: started from RAM, the flash may not be ours.
	local board = m.model:lower():gsub("[^a-z0-9]", "-")
	if (read_file("/proc/cmdline") or ""):find("osiris%.slot=") then
		local p = io.popen("fw_printenv -n board 2> /dev/null")
		local cur = (p:read("*a") or ""):gsub("%s+$", "")
		p:close()
		if cur ~= board then
			os.execute("fw_setenv board " .. board)
		end
	end

	if read_file(OWN_HOSTNAME) then
		log("%s %s, keeping the hostname from %s", m.make or "", m.model, OWN_HOSTNAME)
		return
	end

	local host = m.model:upper():gsub("[^A-Z0-9]", "-")
	os.execute("hostname " .. host)
	log("%s %s (%s), hostname %s", m.make or "", m.model, m.code or "", host)
end

-- what the network menu shows; mcud answers the MCU with it
function M.set_network()
	local p = io.popen("ifconfig " .. IFACE .. " 2> /dev/null")
	local ifc = p:read("*a")
	local n = { dhcp = true, dns = {} }

	p:close()
	n.ip = ifc:match("inet addr:([%d%.]+)")
	n.netmask = ifc:match("Mask:([%d%.]+)")
	n.link = (read_file("/sys/class/net/" .. IFACE .. "/carrier") or ""):match("^1") ~= nil
	n.mac = (read_file("/sys/class/net/" .. IFACE .. "/address") or ""):gsub("%s", "")
	n.name = (read_file("/proc/sys/kernel/hostname") or ""):gsub("%s", "")

	-- default route, the gateway in host (little endian) order
	local gw = (read_file("/proc/net/route") or ""):match(IFACE .. "%s+00000000%s+(%x+)")
	if gw then
		local v = tonumber(gw, 16)
		n.gateway = string.format("%d.%d.%d.%d", bit.band(v, 0xff),
					  bit.band(bit.rshift(v, 8), 0xff),
					  bit.band(bit.rshift(v, 16), 0xff),
					  bit.rshift(v, 24))
	end

	for ns in (read_file("/etc/resolv.conf") or ""):gmatch("nameserver%s+([%d%.]+)") do
		n.dns[#n.dns + 1] = ns
	end

	call("mcu", "network", n)
end

-- the MCU cuts the power once we answer; preinit sets up a wiped /data again
function M.power(state)
	if state ~= "off" and state ~= "reset" then
		return
	end

	if state == "reset" then
		log("factory reset: wiping /data")
		os.execute("rm -rf /data/* /data/.[!.]*")
	end

	os.execute("sync")
	call("mcu", "power", { state = state })
	if state == "reset" then
		os.execute("(sleep 2; reboot) &")
	end
end

events.on("mcu.ready", function(msg) M.set_model(msg) end)
events.on("mcu.network", function() M.set_network() end)
events.on("mcu.power", function(msg) M.power(msg.state) end)

return M

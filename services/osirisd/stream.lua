-- SPDX-License-Identifier: BSD-2-Clause
-- The source: the input and rate the MCU reports, and the stream status the
-- MCU shows for it (PCM, multichannel PCM, Dolby Digital the DSP decodes)

local bit = require "bit"
local audio = require "audio"
local source = require "source"
local events = require "events"
local util = require "util"

local call = util.call

local M = {}

-- C1 sample rate codes
local FS_HZ = { [8] = 44100, [9] = 48000, [12] = 96000 }

-- multichannel PCM from HDMI as AudioApp tells the MCU (type 0E, format 70):
-- from the CEA-861 allocation below 0x14 (bit 0 LFE, 1 centre, 2-3 the rear:
-- one, a pair or both; 0x10 and up the surround and back pairs) the source's
-- channel mask (FL FR 0005, C 0002, SL SR 0088, SB 0020, SBL SBR 0050) and
-- " 5.1"
local STREAM_PCM = { type = 2, input_mask = 0x0005, lfe = false, channels = " 2.0", format = 0x20 }
local REAR_MASK = { [0] = 0, 0x0020, 0x0088, 0x00A8 }
local REAR_COUNT = { [0] = 0, 1, 2, 3 }
local stream_now		-- the stream status last sent for PCM
local pcm_streams = {}		-- one per allocation, so equal ones compare equal

local function pcm_stream(ca)
	if not ca or ca >= 0x14 then
		return STREAM_PCM
	end
	if pcm_streams[ca] then
		return pcm_streams[ca]
	end

	local lfe, fc = bit.band(ca, 1) ~= 0, bit.band(ca, 2) ~= 0
	local back = bit.band(ca, 0x10) ~= 0
	local rear = back and 2 or bit.band(bit.rshift(ca, 2), 3)
	local mask = 0x0005 + (fc and 0x0002 or 0) + REAR_MASK[rear] + (back and 0x0050 or 0)
	local n = 2 + (fc and 1 or 0) + REAR_COUNT[rear] + (back and 2 or 0)

	pcm_streams[ca] = { type = 0x0E, input_mask = mask, lfe = lfe,
			    channels = string.format(" %d.%d", n, lfe and 1 or 0), format = 0x70 }
	return pcm_streams[ca]
end

-- AC-3 the DSP decodes (dsp.bitstream): with a centre or surrounds it plays
-- as multichannel from a digital input; ac3_ca is its layout as a CEA-861
-- allocation, nil otherwise. ac3_now: the 0x42 the MCU got for it. c1_*:
-- the MCU's last input report, to go back to.
local ac3_ca, ac3_now, c1_interface, c1_channels

-- the source by the MCU's C1 interface: 1 analog, 2 multichannel PCM from
-- HDMI, 5 the NET input (audio from Linux), else digital
function M.set_input(interface, fs, channels)
	c1_interface, c1_channels = interface, channels or c1_channels
	local decoded = ac3_ca and interface ~= 1 and interface ~= 5
	local input = decoded and "multichannel" or interface == 1 and "analog" or
		      interface == 2 and "multichannel" or interface == 5 and "network" or "digital"
	local was = source.input_now

	-- the DSP works out its filters and delays for the rate the MCU reports
	-- a code not known yet above 48 kHz: at least process every other frame
	local hz = FS_HZ[fs] or fs and fs > 9 and 96000

	if hz and hz ~= source.fs_now then
		source.fs_now = hz
		call("dsp", "set", { fs = hz })
	end

	source.input_now = input
	source.channels_now = decoded and ac3_ca or c1_channels

	-- what the MCU shows for PCM: stereo, or multichannel from HDMI
	if not ac3_now then
		local st = interface == 2 and pcm_stream(source.channels_now) or STREAM_PCM
		if st ~= stream_now then
			stream_now = st
			call("mcu", "stream", st)
		end
	end
	audio.set_back_fill()
	if input == was then
		return
	end

	call("dsp", "set", { input = input, meter = input == "network" })
	if (input == "multichannel") ~= (was == "multichannel") then
		audio.set_mode(source.mode_now, source.mode_name)
	end
end

-- AC-3 layouts (acmod) as AudioApp tells the MCU: the source's channel
-- mask, and the count of front and rear channels for " 5.1"
local AC3_MASK = { [0] = 0x2000, 0x0002, 0x0005, 0x0007, 0x0025, 0x0027, 0x008D, 0x008F }
local AC3_FRONT = { [0] = 2, 1, 2, 3, 2, 3, 2, 3 }
local AC3_REAR = { [0] = 0, 0, 0, 0, 1, 1, 2, 2 }

-- liba52's layout names (dsp status "decoded") back to acmod
local AC3_NAMES = { ["1+1"] = 0, ["1/0"] = 1, ["2/0"] = 2, ["3/0"] = 3, ["2/1"] = 4,
		    ["3/1"] = 5, ["2/2"] = 6, ["3/2"] = 7 }

local function ac3_stream(acmod, lfe)
	local text = acmod == 0 and " 1+1" or
		     string.format(" %d.%d", AC3_FRONT[acmod] + AC3_REAR[acmod], lfe and 1 or 0)

	return { type = 3, input_mask = AC3_MASK[acmod], lfe = lfe, channels = text, format = 0x20 }
end

-- Dolby Digital decoded on the DSP: the MCU shows it, and with a centre or
-- surrounds it plays as multichannel (2/0 stays stereo for the upmix)
local function bitstream(msg)
	local was = ac3_ca
	local acmod = tonumber(msg.acmod)

	if msg.decoded and acmod and AC3_MASK[acmod] then
		ac3_ca = acmod >= 3 and (AC3_REAR[acmod] > 0 and 0x0B or 0x00) or nil
		ac3_now = ac3_stream(acmod, msg.lfe == true or msg.lfe == 1)
		call("mcu", "stream", ac3_now)
	else
		ac3_ca = nil
		if ac3_now then
			-- back to what the last input report says
			ac3_now, stream_now = nil, nil
			if not c1_interface then
				stream_now = STREAM_PCM
				call("mcu", "stream", STREAM_PCM)
			end
		end
	end

	if c1_interface and (not stream_now or (ac3_ca ~= nil) ~= (was ~= nil)) then
		M.set_input(c1_interface, nil, c1_channels)
	end
end

-- a restarted dspd decodes nothing yet: no Dolby left over
function M.dsp_lost()
	if ac3_now then
		ac3_ca, ac3_now, stream_now = nil, nil, nil
		if c1_interface then
			M.set_input(c1_interface, nil, c1_channels)
		else
			stream_now = STREAM_PCM
			call("mcu", "stream", STREAM_PCM)
		end
	end
end

-- a restarted mcud starts from PCM
events.on("mcu.ready", function()
	if ac3_now or stream_now then
		call("mcu", "stream", ac3_now or stream_now)
	end
end)

events.on("dsp.bitstream", bitstream)
events.on("mcu.input", function(msg)
	if msg.interface then
		M.set_input(msg.interface, msg.fs, msg.channels)
	end
end)

-- AC-3 decoded since before osirisd started: dspd only tells changes
function M.start_decoded(ds)
	if ds and ds.decoded then
		local name, lfe = ds.decoded:match("^([^.]+)%.(%d)$")
		if name and AC3_NAMES[name] then
			bitstream({ decoded = ds.decoded, acmod = AC3_NAMES[name], lfe = lfe == "1" })
		end
	end
end

return M

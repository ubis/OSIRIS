-- SPDX-License-Identifier: BSD-2-Clause
-- What the DSP plays and how: the listening mode, the speakers, distances,
-- tone, equalizer, Dynamic EQ and Volume, LFE, A/V sync, Late Night and
-- the test noise of level calibration

local bit = require "bit"
local source = require "source"
local events = require "events"
local util = require "util"

local call, log, save_state = util.call, util.log, util.save_state

local M = {}

-- listening modes (D2), with the ISCP name the MCU reports alongside
local LM_STEREO = 0x04				-- LMD00
local LM_ALL_CH_STEREO = 0x0A			-- LMD0C
local LM_FULL_MONO = 0x0B			-- LMD13
local LM_TESTTONE = 0xF1			-- level calibration (C_LM_TESTTONE)

-- calibration channel -> DSP output; front high and wide share the
-- surround back pair
local TESTTONE_OUT = {
	fl = "fl", fr = "fr", c = "c", sw = "sw", sl = "sl", sr = "sr",
	sbl = "sbl", sbr = "sbr", fhl = "sbl", fhr = "sbr", fwl = "sbl", fwr = "sbr",
}
local TESTTONE_LEVEL = -70		-- dBFS RMS, at the MCU's reference volume

-- speakers per listening mode, by the MCU's byte until it names the mode;
-- anything else (stereo, direct, the modes the DSP cannot do yet) plays on
-- the front pair
local MODE_OUTPUTS = {
	[LM_ALL_CH_STEREO] = "all",
	[LM_FULL_MONO] = "mono",
	all_ch_stereo = "all",
	full_mono = "mono",
}

-- the surround decoder's preset for the modes that make surround from
-- stereo, the sound field modes too
local MODE_SURROUND = {
	surround = "movie", film = "movie", thx = "movie", thx_cinema = "movie",
	tv_logic = "movie", neural_surround = "movie",
	plii_movie = "movie", plii_thx_cinema = "movie", plii_movie_dsx = "movie",
	neo6_cinema = "movie", neo6_thx_cinema = "movie", neo6_cinema_dsx = "movie",
	pliiz_height = "movie", pliiz_height_thx_cinema = "movie",
	musical = "music", orchestra = "music", unplugged = "music",
	studio_mix = "music", thx_music = "music",
	plii_music = "music", plii_thx_music = "music", plii_music_dsx = "music",
	neo6_music = "music", neo6_thx_music = "music", neo6_music_dsx = "music",
	pliiz_height_thx_music = "music",
	action = "game", thx_games = "game",
	plii_game = "game", plii_thx_games = "game", plii_game_dsx = "game",
	neo6_thx_games = "game", pliiz_height_thx_games = "game",
}

-- the room the sound field modes add on top of it
local MODE_REVERB = {
	orchestra = "hall", unplugged = "club", studio_mix = "studio",
	film = "theater", musical = "theater", action = "theater", tv_logic = "tv",
}

-- the DSP's outputs; the surround back amplifiers drive surround back,
-- front high or front wide speakers, as the receiver's layout says
local OUTPUTS = { "fl", "fr", "c", "sw", "sl", "sr", "sbl", "sbr" }
local BACK = {
	sb = { speaker = "surround_back", l = "sbl", r = "sbr" },
	fh = { speaker = "front_high", l = "fhl", r = "fhr" },
	fw = { speaker = "front_wide", l = "fwl", r = "fwr" },
}
local EQ_SPEAKERS = {
	"front", "center", "surround", "surround_back", "front_high", "front_wide", "subwoofer",
}
local OUTPUT_SPEAKER = {
	fl = "front", fr = "front", c = "center", sl = "surround", sr = "surround",
}

local back = BACK.sb		-- what the surround back outputs play
local distances_now		-- the MCU's distances by speaker
local testtone_ch = "fl"
local eq_now = {}		-- the receiver's equalizer: eq_on and bands by speaker
local back_fill_now		-- the surround back outputs repeat the surrounds
local lfe_pcm = 0		-- dB, the LFE level for multichannel PCM; -100 off

-- A/V sync in ms: the MCU sends the menu's value less the 22 ms the stock
-- DSP delayed anyway, and 0 for anything up to 20 ms
local AV_STOCK_MS = 22
local LOUDNESS_DB_PER = 4	-- dB below reference per dB of bass boost
local LOUDNESS_MAX = 10
local av_now = 0
local volume_now, adsy_now, loudness_now = 0, {}, nil

-- Late Night, the MCU's setting for Dolby Digital
local LATE_NIGHT = { off = 0, low = 1, high = 2 }
local late_now = 0

function M.set_late_night(name)
	late_now = LATE_NIGHT[name] or 0
	call("dsp", "set", { late_night = late_now })
end

function M.set_av_sync(ms)
	av_now = ms > 0 and ms + AV_STOCK_MS or 0
	call("dsp", "set", { av_sync = av_now })
end

-- Dynamic EQ: more bass the further the volume (0 dB is the reference) is
-- below the reference, less the higher the reference level offset
local function set_loudness()
	local db = -1

	if adsy_now.dynamic_eq then
		local below = -volume_now / 100 - (adsy_now.dynamic_eq_offset or 0)

		db = math.floor(math.max(0, math.min(LOUDNESS_MAX, below / LOUDNESS_DB_PER)) + 0.5)
	end

	if db ~= loudness_now then
		loudness_now = db
		call("dsp", "set", { loudness = db })
	end
end

-- Dynamic Volume, 0 off to 3 heavy, as the MCU sets it
local function set_dynamic_volume()
	call("dsp", "set", { dynamic_volume = adsy_now.dynamic_volume or 0 })
end

-- the master volume the MCU reports, 0.01 dB
function M.set_volume(volume)
	volume_now = volume
	set_loudness()
end

-- Dynamic EQ, its reference offset and Dynamic Volume, as mcu.audyssey
function M.set_audyssey(a)
	adsy_now = a
	set_loudness()
	set_dynamic_volume()
end

-- both from mcu status at start
function M.start_levels(volume, a)
	volume_now = volume or volume_now
	M.set_audyssey(a)
end

-- how much of the LFE channel multichannel PCM plays; the receiver has one
-- setting per format, the DSP plays PCM only
function M.set_lfe(format, db)
	if format ~= "multichannel_pcm" or not db then
		return
	end

	lfe_pcm = db
	save_state({ lfe_multichannel_pcm = db })
	call("dsp", "set", { lfe_level = db })
end

-- noise only during level calibration, on the channel the MCU picked
local function set_testtone()
	local out = source.mode_now == LM_TESTTONE and TESTTONE_OUT[testtone_ch] or "off"

	call("dsp", "set", { testtone = out, testtone_level = TESTTONE_LEVEL })
end

function M.set_testtone_channel(ch)
	testtone_ch = ch or testtone_ch
	set_testtone()
end

-- multichannel without surround back or rear centre (CEA-861 allocation
-- below 0x10 without bit 2) on surround back speakers: they repeat the
-- surrounds, except in the modes that play the source as it is
local UNPROCESSED = { direct = true, pure_audio = true }

function M.set_back_fill()
	local ca = source.channels_now or 0
	local fill = source.input_now == "multichannel" and ca < 0x10 and bit.band(ca, 0x04) == 0 and
		     back == BACK.sb and not UNPROCESSED[source.mode_name] and true or false

	if fill ~= back_fill_now then
		back_fill_now = fill
		call("dsp", "set", { back_fill = fill })
	end
end

-- by the name of the mode's ISCP code, the MCU's byte only where there is
-- no name yet; either can be missing
function M.set_mode(mode, name)
	if not mode and not name then
		return
	end

	-- not the test tone: a restart must not come back into calibration
	if mode ~= LM_TESTTONE then
		save_state({ listening_mode = mode or "", listening_mode_name = name or "" })
	end

	source.mode_now, source.mode_name = mode, name
	local surround = mode ~= LM_TESTTONE and MODE_SURROUND[name] or "off"
	local outputs = MODE_OUTPUTS[name] or MODE_OUTPUTS[mode] or "front"

	-- multichannel already has its channels: Stereo folds them down
	if source.input_now == "multichannel" then
		surround = "off"
		if outputs == "front" and name ~= "stereo" and (name or mode ~= LM_STEREO) then
			outputs = "multichannel"
		end
	end

	call("dsp", "set", {
		outputs = outputs,
		surround = surround,
		reverb = surround ~= "off" and MODE_REVERB[name] or "off",
	})
	set_testtone()
	M.set_back_fill()
end

-- front bass and treble in dB, from the receiver's tone controls
function M.set_tone(bass, treble)
	if not bass or not treble then
		return
	end

	save_state({ bass = bass, treble = treble })
	call("dsp", "set", { bass = bass, treble = treble })
end

-- speaker distances in cm; the DSP holds the nearer speakers back
function M.set_distances(d)
	if not d then
		return
	end

	local out, save = {}, {}

	distances_now = d
	for _, o in ipairs(OUTPUTS) do
		local cm

		if o == "sbl" or o == "sbr" then
			cm = d[o == "sbl" and back.l or back.r] or d[o]
		else
			cm = d[o]
		end

		out[o] = tonumber(cm) or 0
		save["distance_" .. o] = out[o]
	end

	save_state(save)
	call("dsp", "set", { distances = out })
end

-- the receiver's equalizer on the outputs, flat while it is off; the
-- surround back outputs take the bands of what the layout has them drive.
-- mcud reports only what the MCU has sent since it started: the rest stays.
function M.set_eq(e)
	local bands, out = {}, {}

	for k, v in pairs(e or {}) do
		eq_now[k] = v
	end

	e = eq_now
	local save = { eq_on = e.eq_on and 1 or 0 }
	for _, sp in ipairs(EQ_SPEAKERS) do
		local b = type(e[sp]) == "table" and e[sp] or {}

		bands[sp] = e.eq_on and b or {}
		save["eq_" .. sp] = table.concat(b, ",")
	end

	for _, o in ipairs(OUTPUTS) do
		local sp = o == "sw" and "subwoofer" or (o == "sbl" or o == "sbr") and back.speaker or
			   OUTPUT_SPEAKER[o]

		out[o] = bands[sp] or {}
	end

	save_state(save)
	call("dsp", "set", { eq = out })
end

-- the equalizer as saved, for a start before the MCU reports it
function M.saved_eq(saved)
	local e = { eq_on = saved.eq_on == "1" }

	for _, sp in ipairs(EQ_SPEAKERS) do
		local b = {}

		for v in (saved["eq_" .. sp] or ""):gmatch("-?%d+") do
			b[#b + 1] = tonumber(v)
		end

		e[sp] = b
	end

	return e
end

-- the speakers there are, small ones' bass to the subwoofer
function M.set_speakers(m)
	if not m or not m.speakers then
		return
	end

	local sp, crossover, absent = m.speakers, {}, {}
	local kind = (m.layout or "sb"):match("^(%a%a)")

	back = BACK[kind] or BACK.sb
	for _, o in ipairs(OUTPUTS) do
		local name = o == "sbl" and back.speaker or o == "sbr" and back.speaker or
			     OUTPUT_SPEAKER[o]

		if o == "sw" then
			if (m.subwoofers or 0) == 0 then
				absent[#absent + 1] = o
			end
		elseif kind == "no" and (o == "sbl" or o == "sbr") or sp[name] == nil then
			absent[#absent + 1] = o
			crossover[o] = 0
		else
			crossover[o] = sp[name]
		end
	end

	call("dsp", "set", {
		crossover = crossover,
		subwoofer = (m.subwoofers or 0) > 0,
		sub_lowpass = m.lfe_lowpass or 0,
		double_bass = m.double_bass and true or false,
		absent = absent,
	})
	M.set_distances(distances_now)
	M.set_back_fill()
	-- not before the saved equalizer is loaded, it would be saved flat
	if next(eq_now) then
		M.set_eq()
	end
end

function M.saved_distances(saved)
	local d

	for _, o in ipairs(OUTPUTS) do
		local cm = tonumber(saved["distance_" .. o] or "")

		if cm then
			d = d or {}
			d[o] = cm
		end
	end

	return d
end

-- a restarted dspd has none of it: forget what it was sent
function M.dsp_lost()
	back_fill_now = nil
end

-- and send it all again
function M.dsp_setup()
	local saved = util.load_state()

	log("DSP started again, setting it up")
	M.set_mode(source.mode_now, source.mode_name)
	M.set_tone(tonumber(saved.bass or ""), tonumber(saved.treble or ""))
	M.set_speakers(call("mcu", "status"))
	M.set_distances(distances_now or M.saved_distances(saved))
	call("dsp", "set", { input = source.input_now, fs = source.fs_now, lfe_level = lfe_pcm, av_sync = av_now,
			     late_night = late_now, loudness = loudness_now, dynamic_volume = adsy_now.dynamic_volume or 0,
			     meter = source.input_now == "network" })
	M.set_eq()
end

events.on("mcu.listening_mode", function(msg)
	-- the name can come without the MCU's byte, after a restart
	M.set_mode(msg.mode or source.mode_now, msg.name)
end)
events.on("mcu.volume", function(msg)
	if msg.volume then
		M.set_volume(msg.volume)
	end
end)
events.on("mcu.audyssey", function(msg) M.set_audyssey(msg) end)
events.on("mcu.late_night", function(msg) M.set_late_night(msg.dolby) end)
events.on("mcu.av_sync", function(msg)
	if msg.ms then
		M.set_av_sync(msg.ms)
	end
end)
events.on("mcu.lfe_level", function(msg) M.set_lfe(msg.format, msg.level) end)
events.on("mcu.testtone", function(msg) M.set_testtone_channel(msg.channel) end)
events.on("mcu.tone", function(msg) M.set_tone(msg.bass, msg.treble) end)
events.on("mcu.distance", function(msg) M.set_distances(msg.distances) end)
events.on("mcu.speakers", function(msg) M.set_speakers(msg) end)
events.on("mcu.equalizer", function(msg) M.set_eq(msg.equalizer) end)

return M

-- SPDX-License-Identifier: BSD-2-Clause
-- What the source is and the listening mode: the parts that more than one module reads

return {
	input_now = nil,	-- "analog", "digital", "multichannel", "network"; nil: not known yet
	channels_now = nil,	-- the source's channel allocation (CEA-861)
	fs_now = nil,		-- Hz, the source's sample rate
	mode_now = nil,		-- the MCU's listening mode byte
	mode_name = nil,	-- and the name of its ISCP code
}

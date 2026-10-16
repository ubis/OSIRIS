-- SPDX-License-Identifier: BSD-2-Clause
-- The ubus events: each module registers what it handles, in the order the
-- modules load; more than one may handle the same event

local uloop = require "uloop"

local M = {}

local handlers = {}		-- event name -> functions

function M.on(name, fn)
	handlers[name] = handlers[name] or {}
	table.insert(handlers[name], fn)
end

-- The binding delivers events inside a running conn:call; a handler that
-- calls out would then run inside another one's call and crash LuaJIT.
-- Queue the events and handle them from the main loop.
function M.listen(conn)
	local events, events_timer, queued = {}, nil, {}

	events_timer = uloop.timer(function()
		while #events > 0 do
			local ev = table.remove(events, 1)
			for _, fn in ipairs(handlers[ev[1]]) do
				fn(ev[2])
			end
		end
	end)

	for name in pairs(handlers) do
		queued[name] = function(msg)
			events[#events + 1] = { name, msg }
			events_timer:set(0)
		end
	end
	conn:listen(queued)
end

return M

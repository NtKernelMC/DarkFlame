-- Inject this once through the usual Lua Injector on the local test server.
-- Do not toggle hideFunctionCall/dfTrapScope: observe the existing lifetime.
local trace = dfTraceHidden
assert(type(trace) == "function", "DLL with dfTraceHidden is required")

trace("probe:initial-chunk")

local element = createElement("darkflame-hidden-lifetime-probe")
assert(element, "could not create local probe element")
local event = "darkflame:hiddenLifetimeProbe"
-- The local event name may already exist after a previous probe run.
addEvent(event, false)
assert(addEventHandler(event, element, function(label)
    trace("probe:event:" .. label)
end), "could not register probe event handler")

local function tick(label)
    -- First action on callback entry; the event stays entirely client-side.
    trace("probe:timer:" .. label)
    assert(triggerEvent(event, element, label), "local probe event failed")
end

assert(setTimer(tick, 250, 1, "250ms"), "could not register early timer")
assert(setTimer(tick, 2500, 1, "2500ms"), "could not register late timer")
-- ManagedChunk tracks both timers, the handler and element for unload cleanup.

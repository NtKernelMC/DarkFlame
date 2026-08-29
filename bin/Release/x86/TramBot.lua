-- DarkFlame native API used by the bot.
local api = {
    triggerServerEvent = dfTriggerServerEvent,
    triggerEvent = dfTriggerEvent,
    addEvent = dfAddEvent,
    addEventHandler = dfAddEventHandler,
    removeEventHandler = dfRemoveEventHandler,
    catchServerEvent = dfCatchServerEvent,
    removeEventCatcher = dfRemoveEventCatcher,
    emulateKey = dfEmulateKey,
    alert = dfPlayAlertSignal,
    takeCommand = dfTramTakeCommand,
    updateState = dfTramUpdate,
    log = dfTramLog,
    menuOpen = dfMenuOpen,
}

for name, fn in pairs(api) do
    if type(fn) ~= "function" then
        error("DarkFlame API is missing: " .. name, 0)
    end
end

local SPEEDS = {
    0.10, 0.15, 0.20, 0.25, 0.30, 0.35,
    0.40, 0.45, 0.50, 0.55, 0.60, 0.65,
}

-- Route points copied from the original TramBot.
local ROUTES = {
    neva100 = {
        name = "ТП Нева-100",
        server = {3, 4},
        forward = {
            {676.12872314453, "traffic", "3"},
            {774.5732421875, "marker", "228"},
            {1452.7437744141, "marker", "228"},
            {2091.0568847656, "traffic", "3"},
            {2313.2888183594, "marker", "228"},
        },
        backward = {
            {2486.728515625, "marker", "228"},
            {2612.3110351562, "traffic", "3"},
            {3314.8630371094, "marker", "228"},
            {3992.236328125, "marker", "228"},
            {4017.5578613281, "traffic", "3"},
        },
    },
    priva2 = {
        name = "ТП Прива-2",
        server = {1, 1},
        forward = {
            {1023.3134765625, "marker", "228"},
            {1663.2995605469, "marker", "228"},
            {2514.4660644531, "marker", "228"},
        },
        backward = {
            {3141.4267578125, "marker", "228"},
            {120.79351043701, "marker", "228"},
        },
    },
    priva8 = {
        name = "ТП Прива-8",
        server = {4, 2},
        forward = {
            {3701.1926269531, "marker", "228"},
            {215.81587219238, "traffic", "3"},
            {360.34118652344, "marker", "228"},
            {500.49160766602, "traffic", "3"},
            {649.27587890625, "marker", "228"},
            {753.90014648438, "traffic", "3"},
            {1124.0673828125, "traffic", "0"},
            {1544.6889648438, "marker", "228"},
        },
        backward = {
            {1793.6618652344, "traffic", "3"},
            {2148.4543457031, "traffic", "3"},
            {2315.9724121094, "marker", "228"},
            {2425.62109375, "traffic", "3"},
            {2620.6437988281, "marker", "228"},
            {2708.7893066406, "traffic", "3"},
            {3077.4848632812, "marker", "228"},
        },
    },
    mirka8 = {
        name = "ТП Мирка-8",
        server = {2, 3},
        forward = {
            {163.36724853516, "marker", "228"},
            {179.87634277344, "traffic", "0"},
            {570.44390869141, "marker", "228"},
            {738.84997558594, "marker", "228"},
            {2581.1796875, "marker", "228"},
            {2766.76171875, "marker", "228"},
            {2827.8947753906, "traffic", "0"},
            {3142.962890625, "marker", "228"},
            {3214.697265625, "traffic", "0"},
            {3401.5144042969, "traffic", "0"},
            {3564.2797851562, "marker", "228"},
            {3799.7331542969, "traffic", "3"},
            {4361.201171875, "marker", "228"},
            {5042.8876953125, "traffic", "3"},
            {5324.8471679688, "marker", "228"},
            {5451.4389648438, "traffic", "3"},
            {5629.0048828125, "traffic", "0"},
            {5768.4736328125, "marker", "228"},
            {6017.5708007812, "traffic", "0"},
            {6129.5776367188, "marker", "228"},
            {6327.4619140625, "marker", "228"},
            {8167.1977539062, "marker", "228"},
            {8329.7255859375, "marker", "228"},
            {8531.05859375, "marker", "228"},
            {8757.0673828125, "traffic", "3"},
        },
        backward = {
            {9216.388671875, "marker", "228"},
            {9452.7509765625, "traffic", "3"},
            {9723.7314453125, "marker", "228"},
            {9957.822265625, "marker", "228"},
            {10127.663085938, "marker", "228"},
            {11951.051757812, "marker", "228"},
            {12156.563476562, "marker", "228"},
            {12200.796875, "traffic", "0"},
            {12516.03125, "marker", "228"},
            {12588.5546875, "traffic", "0"},
            {12773.041015625, "traffic", "0"},
            {12935.9609375, "marker", "228"},
            {13167.912109375, "traffic", "3"},
            {13737.702148438, "marker", "228"},
            {14414.516601562, "traffic", "3"},
            {14687.794921875, "marker", "228"},
            {14823.716796875, "traffic", "3"},
            {15000.133789062, "traffic", "0"},
            {15134.524414062, "marker", "228"},
            {15392.872070312, "traffic", "0"},
            {15502.51171875, "marker", "228"},
            {15695.481445312, "marker", "228"},
            {17527.6328125, "marker", "228"},
            {17700.49609375, "marker", "228"},
            {18066.822265625, "marker", "228"},
            {18070.896484375, "traffic", "0"},
        },
    },
    priva7 = {
        name = "ТП Прива-7",
        server = {4, 1},
        forward = {
            {4428.7016601562, "marker", "228"},
            {4434.6005859375, "traffic", "3"},
            {4689.4692382812, "traffic", "3"},
            {4739.935546875, "marker", "228"},
            {4740.8540039062, "traffic", "3"},
            {5112.6791992188, "marker", "228"},
            {5161.283203125, "traffic", "3"},
            {5416.8559570312, "traffic", "0"},
            {5596.037109375, "marker", "228"},
            {5608.9169921875, "traffic", "3"},
            {6137.0087890625, "marker", "228"},
            {6648.4453125, "marker", "228"},
            {214.89573669434, "marker", "228"},
        },
        backward = {
            {754.47229003906, "marker", "228"},
            {1432.4641113281, "marker", "228"},
            {1862.4923095703, "marker", "228"},
            {2339.1965332031, "marker", "228"},
            {2362.7390136719, "traffic", "3"},
            {2561.046875, "traffic", "3"},
            {2809.2077636719, "traffic", "3"},
            {2894.2058105469, "marker", "228"},
            {3256.4936523438, "marker", "228"},
            {3270.13671875, "traffic", "3"},
            {3529.1765136719, "marker", "228"},
            {3540.1071777344, "traffic", "3"},
        },
    },
    mirka3 = {
        name = "ТП Мирка-3",
        server = {2, 1},
        forward = {
            {2213.8408203125, "marker", "228"},
            {2227.3937988281, "traffic", "0"},
            {2416.3046875, "traffic", "3"},
            {2705.0671386719, "marker", "228"},
            {2716.2592773438, "traffic", "3"},
            {2985.6801757812, "marker", "228"},
            {2996.2321777344, "traffic", "0"},
            {3487.2329101562, "marker", "228"},
            {3498.1760253906, "traffic", "0"},
            {3898.9384765625, "marker", "228"},
            {3919.7004394531, "traffic", "0"},
            {4369.6616210938, "marker", "228"},
            {4568.3178710938, "marker", "228"},
            {5139.568359375, "marker", "228"},
        },
        backward = {
            {5548.875, "marker", "228"},
            {5815.9487304688, "marker", "228"},
            {6151.2221679688, "traffic", "0"},
            {6232.37890625, "marker", "228"},
            {6598.0844726562, "marker", "228"},
            {6598.0844726562, "traffic", "0"},
            {183.11145019531, "marker", "228"},
            {184.27436828613, "traffic", "0"},
            {381.50466918945, "traffic", "3"},
            {665.04156494141, "marker", "228"},
            {666.08337402344, "traffic", "3"},
            {950.44158935547, "traffic", "0"},
        },
    },
}

local function profile(values)
    local result = {}
    for index, speed in ipairs(SPEEDS) do
        result[speed] = values[index]
    end
    return result
end

-- Braking distances for every supported tram model and route.
local BRAKES = {
    neva100 = profile({4.16, 9.28, 16.36, 25.37, 36.27, 49.01, 63.58, 79.91, 98.07, 98.01, 98.08, 98.05}),
    mirka3_gray = profile({2.61, 5.65, 10.03, 15.47, 22.16, 30.02, 39.03, 49.20, 60.46, 60.45, 60.44, 60.45}),
    mirka8_gray = profile({3.36, 7.51, 13.18, 20.47, 29.28, 39.67, 51.46, 64.75, 79.47, 79.50, 79.48, 79.46}),
    mirka3_red = profile({2.74, 5.67, 9.98, 15.49, 22.18, 30.00, 39.15, 49.21, 60.46, 60.44, 60.48, 60.45}),
    mirka8_red = profile({2.53, 5.64, 9.99, 15.56, 22.16, 30.04, 39.02, 49.17, 60.49, 60.49, 60.50, 60.32}),
    mirka3_blue = profile({2.56, 5.65, 9.97, 15.48, 22.18, 30.05, 39.02, 49.19, 60.48, 60.49, 60.48, 60.46}),
    mirka8_blue_pair = profile({3.37, 7.47, 13.17, 20.46, 29.27, 39.62, 51.45, 64.79, 79.46, 79.48, 79.49, 79.48}),
    mirka8_blue_single = profile({2.59, 5.64, 9.96, 15.44, 22.11, 30.26, 38.99, 49.22, 60.36, 60.38, 60.39, 60.40}),
    priva_583 = profile({2.74, 5.67, 9.98, 15.49, 22.18, 30.00, 39.15, 49.21, 60.46, 60.44, 60.48, 60.45}),
    priva_611 = profile({2.66, 5.65, 9.96, 15.46, 22.16, 30.03, 39.05, 49.22, 60.49, 60.43, 60.44, 60.44}),
    priva_604 = profile({2.61, 5.65, 9.99, 15.50, 22.19, 30.05, 39.05, 49.20, 60.65, 60.65, 60.55, 60.48}),
}

-- Mutable runtime state.
local handlers = {}
local timers = {}
local routeTable = {}
local activeRoute = {}
local measuredBrakeData = {}
local routeKey
local direction = "forward"
local botState = "IDLE"
local lastBotState
local train
local currentPoint
local markerCol
local markerDebug
local markerEntered = false
local botEnabled = false
local ignoreTraffic = false
local debugEnabled = false
local sirenEnabled = true
local forceStopper = false
local passengerAlerted = false
local lastCollisionTick = 0
local collisionContacts = setmetatable({}, {__mode="k"})
local stopFrameCount = 0
local lastPosition = 0
local stoppedTick
local expectedBrakeDistance
local brakeStartPosition
local money = "0"
local cleaning = false
local eventCatcher
local keyState = {}
local debugMemory = {}
local debugTicks = {}
local MARKER_BRAKE_MARGIN = 2
local doorGeneration = 0
local doorPhase = "idle"

local updateNativeMenu
local stopBot
local cancelCalibration
local onMarkerHit

-- Shared helpers and event registration.
local function notify(text, errorMessage)
    outputChatBox((errorMessage and "#FF5555[TramBot] " or "#55FF88[TramBot] ") .. tostring(text), 255, 255, 255, true)
end

local function debugOutput(text)
    if debugEnabled then
        text = tostring(text)
        outputChatBox("#E6A15C[TramBot debug] #FFFFFF" .. text, 255, 255, 255, true)
        api.log(text)
    end
end

local function debugChange(key, value, text)
    if not debugEnabled then return end
    value = tostring(value)
    if debugMemory[key] == value then return end
    debugMemory[key] = value
    debugOutput(text)
end

local function debugRate(key, delay, text)
    if not debugEnabled then return end
    local now = getTickCount()
    if debugTicks[key] and now - debugTicks[key] < delay then return end
    debugTicks[key] = now
    debugOutput(text)
end

local function pointText(point)
    if not point then return "нет" end
    return string.format("%s@%.2f state=%s", tostring(point[2]),
        tonumber(point[1]) or 0, tostring(point[3] or "-"))
end

local function schedule(callback, delay, times, ...)
    local arguments = {...}
    local timer
    timer = setTimer(function()
        if times == 1 then
            timers[timer] = nil
        end
        callback(unpack(arguments))
    end, delay, times)
    if timer then
        timers[timer] = true
    end
    return timer
end

local function killTrackedTimer(timer)
    if timer and isTimer(timer) then
        killTimer(timer)
    end
    timers[timer] = nil
end

local function addHandler(eventName, attachedTo, callback, ...)
    local ok = api.addEventHandler(eventName, attachedTo, callback, ...)
    if ok then
        handlers[#handlers + 1] = {eventName, attachedTo, callback}
    end
    return ok
end

local function removeHandler(eventName, attachedTo, callback)
    api.removeEventHandler(eventName, attachedTo, callback)
    for index = #handlers, 1, -1 do
        local item = handlers[index]
        if item[1] == eventName and item[2] == attachedTo and item[3] == callback then
            table.remove(handlers, index)
            break
        end
    end
end

local function copyTable(source)
    local result = {}
    for key, value in pairs(source) do
        result[key] = value
    end
    return result
end

local function copyRoute(source)
    local result = {}
    for index, point in ipairs(source or {}) do
        result[index] = {point[1], point[2], point[3]}
    end
    return result
end

local function routeByServer(depot, line)
    for key, route in pairs(ROUTES) do
        if route.server[1] == depot and route.server[2] == line then
            return key
        end
    end
end

local function generateRoute(newDirection)
    local route = routeKey and ROUTES[routeKey]
    if not route then
        notify("Маршрут не определён.", true)
        return false
    end
    direction = newDirection or direction
    activeRoute = copyRoute(route[direction])
    debugOutput(string.format("Маршрут %s/%s: %d точек", route.name, direction, #activeRoute))
    if #activeRoute > 0 then
        debugOutput("Границы маршрута: первая " .. pointText(activeRoute[1])
            .. ", последняя " .. pointText(activeRoute[#activeRoute]))
    end
    if updateNativeMenu then
        updateNativeMenu()
    end
    return #activeRoute > 0
end

local function selectRoute(key, announce)
    local route = ROUTES[key]
    if not route then
        return false
    end
    routeKey = key
    debugOutput(string.format("Выбор маршрута: key=%s, server=%s/%s",
        key, tostring(route.server[1]), tostring(route.server[2])))
    measuredBrakeData = {}
    direction = "forward"
    currentPoint = nil
    activeRoute = {}
    generateRoute(direction)
    if announce then
        notify("Выбран " .. route.name .. ".")
    end
    return true
end

local function determineRoute(depot, line)
    debugOutput(string.format("Catcher получил Tram:onJobAccepted: depot=%s, line=%s",
        tostring(depot), tostring(line)))
    local key = routeByServer(tonumber(depot), tonumber(line))
    if key then
        selectRoute(key, true)
        money = "0"
        debugOutput("Маршрут перехвачен C++ catcher-ом")
    else
        notify("Неизвестный маршрут: " .. tostring(depot) .. "/" .. tostring(line), true)
    end
end

-- Braking profile selection and interpolation.
local function chooseBrakeProfile(vehicle)
    if not isElement(vehicle) or getVehicleType(vehicle) ~= "Train" then
        return false
    end
    local model = getElementModel(vehicle)
    local selected
    local description
    if model == 476 then
        selected, description = BRAKES.neva100, "Нева-100"
    elseif model == 572 then
        selected = routeKey == "mirka8" and BRAKES.mirka8_gray or BRAKES.mirka3_gray
        description = routeKey == "mirka8" and "Мирка-8 серая" or "Мирка-3 серая"
    elseif model == 577 then
        selected = routeKey == "mirka8" and BRAKES.mirka8_red or BRAKES.mirka3_red
        description = routeKey == "mirka8" and "Мирка-8 красная" or "Мирка-3 красная"
    elseif model == 18593 then
        if routeKey == "mirka8" then
            selected = getVehicleTowedByVehicle(vehicle) and BRAKES.mirka8_blue_pair or BRAKES.mirka8_blue_single
            description = getVehicleTowedByVehicle(vehicle) and "Мирка-8 сине-зелёная, 2 вагона" or "Мирка-8 сине-зелёная"
        else
            selected, description = BRAKES.mirka3_blue, "Мирка-3 сине-зелёная"
        end
    elseif model == 583 then
        selected, description = BRAKES.priva_583, routeKey == "priva8" and "Прива-8" or "Прива-7"
    elseif model == 611 then
        selected, description = BRAKES.priva_611, "Прива-7 тип 2"
    elseif model == 604 then
        selected, description = BRAKES.priva_604, "Прива-2/8"
    end
    if not selected then
        notify("Нет профиля торможения для модели " .. tostring(model) .. ". Запусти калибровщик.", true)
        return false
    end
    measuredBrakeData = copyTable(selected)
    notify("Профиль торможения: " .. description .. ".")
    return true
end

local function brakeDistance(speed)
    speed = math.abs(tonumber(speed) or 0)
    if measuredBrakeData[speed] then
        return measuredBrakeData[speed]
    end
    local lower = SPEEDS[1]
    local upper = SPEEDS[#SPEEDS]
    for index, sample in ipairs(SPEEDS) do
        if speed < sample then
            lower = SPEEDS[index - 1] or sample
            upper = sample
            break
        end
    end
    local lowDistance = measuredBrakeData[lower]
    local highDistance = measuredBrakeData[upper]
    if not lowDistance or not highDistance then
        return 0
    end
    if lower == upper then
        return lowDistance
    end
    return lowDistance + (speed - lower) * (highDistance - lowDistance) / (upper - lower)
end

local function setDriveKey(key, pressed, force)
    local repeatDrivePress = pressed and (key == "W" or key == "S")
    if not force and not repeatDrivePress and keyState[key] == pressed then
        return true
    end
    local ok = api.emulateKey(key, pressed)
    if ok then
        keyState[key] = pressed
    end
    return ok
end

local function releaseKeys()
    for _, key in ipairs({"W", "S", "2", "K", "L"}) do
        setDriveKey(key, false, true)
    end
    keyState = {}
end

local function inputBlocked()
    return api.menuOpen() or isChatBoxInputActive()
end

local function safeDriveKey(key, pressed)
    if inputBlocked() then
        return false
    end
    return setDriveKey(key, pressed)
end

local function updateMoney(value)
    money = tostring(value or "0")
end

local function destroyMarker(resetEntered)
    if markerCol and isElement(markerCol) then
        local x, y, z = getElementPosition(markerCol)
        debugOutput(string.format("Удаляю колшейп: %.2f, %.2f, %.2f; resetEntered=%s",
            x or 0, y or 0, z or 0, tostring(resetEntered == true)))
        removeHandler("onClientColShapeHit", markerCol, onMarkerHit)
        destroyElement(markerCol)
    end
    markerCol = nil
    markerDebug = nil
    if resetEntered then
        markerEntered = false
    end
end

onMarkerHit = function(element)
    if element ~= train then
        return
    end
    markerEntered = true
    debugOutput("Трамвай вошёл в колшейп остановки; цель=" .. pointText(currentPoint))
    if currentPoint and currentPoint.forceStop then
        setDriveKey("W", false, true)
        setDriveKey("S", false, true)
        setTrainSpeed(train, 0)
        botState = "STOPPED"
        currentPoint.forceStopped = true
        debugOutput("Доводка завершена: W/S отпущены, speed=0; после остановки старт будет через W")
    end
    destroyMarker(false)
end

local function createMarkerCollider()
    destroyMarker(true)
    if not isElement(train) then
        return false
    end
    local tx, ty, tz = getElementPosition(train)
    local trainDimension = getElementDimension(train)
    local trainInterior = getElementInterior(train)
    local closest
    local closestDistance = math.huge
    local candidates = 0
    local rejected = 0
    for _, marker in ipairs(getElementsByType("marker")) do
        local radius = getMarkerSize(marker)
        local sameWorld = getElementDimension(marker) == trainDimension
            and getElementInterior(marker) == trainInterior
        if getMarkerType(marker) == "checkpoint" and radius > 10 and sameWorld then
            candidates = candidates + 1
            local x, y, z = getElementPosition(marker)
            local distance = getDistanceBetweenPoints3D(tx, ty, tz, x, y, z)
            if distance < closestDistance then
                closest = marker
                closestDistance = distance
            end
        elseif getMarkerType(marker) == "checkpoint" and radius > 10 then
            rejected = rejected + 1
        end
    end
    if not closest or closestDistance >= 150 then
        debugOutput(string.format("Колшейп не создан: кандидатов=%d, ближайший=%s м",
            candidates, closest and string.format("%.1f", closestDistance) or "нет"))
        debugOutput(string.format("Мир трамвая: dimension=%d interior=%d; чужих checkpoint=%d",
            trainDimension, trainInterior, rejected))
        return false
    end
    local x, y, z = getElementPosition(closest)
    local radius = getMarkerSize(closest)
    markerCol = createColSphere(x, y, z, radius)
    markerEntered = false
    if markerCol then
        setElementDimension(markerCol, trainDimension)
        setElementInterior(markerCol, trainInterior)
        markerDebug = {x=x, y=y, z=z, radius=radius}
        local hooked = addHandler("onClientColShapeHit", markerCol, onMarkerHit)
        debugOutput(string.format(
            "Колшейп создан: xyz=%.2f/%.2f/%.2f radius=%.1f distance=%.1f candidates=%d "
                .. "rejected=%d dim=%d int=%d handler=%s",
            x, y, z, radius, closestDistance, candidates, rejected,
            trainDimension, trainInterior, tostring(hooked == true)))
        return true
    end
    debugOutput("createColSphere вернул nil для checkpoint " .. pointText(currentPoint))
    return false
end

local function drawDebugCollider()
    if not debugEnabled or not markerDebug then return end
    local x, y, z = markerDebug.x, markerDebug.y, markerDebug.z
    local radius = markerDebug.radius
    local color = tocolor(255, 90, 210, 220)
    local segments = 32
    for index = 0, segments - 1 do
        local first = index * math.pi * 2 / segments
        local second = (index + 1) * math.pi * 2 / segments
        local c1, s1 = math.cos(first) * radius, math.sin(first) * radius
        local c2, s2 = math.cos(second) * radius, math.sin(second) * radius
        dxDrawLine3D(x + c1, y + s1, z, x + c2, y + s2, z, color, 2)
        dxDrawLine3D(x + c1, y, z + s1, x + c2, y, z + s2, color, 2)
        dxDrawLine3D(x, y + c1, z + s1, x, y + c2, z + s2, color, 2)
    end
end

local function pulseKey(key, after)
    local pressed = safeDriveKey(key, true)
    debugOutput(string.format("Импульс %s: down=%s", key, tostring(pressed == true)))
    if not pressed then
        return false
    end
    schedule(function()
        local released = setDriveKey(key, false, true)
        debugOutput(string.format("Импульс %s: up=%s", key, tostring(released == true)))
        if after then
            after()
        end
    end, math.random(30, 100), 1)
    return true
end

local function handleDoors(continueRoute, generation)
    generation = generation or doorGeneration
    if generation ~= doorGeneration or not botEnabled then
        debugOutput("Двери: отменяю устаревшее действие generation=" .. tostring(generation))
        return
    end
    local doorPoint = currentPoint
    debugOutput(string.format("Двери: эмулирую 2, continueRoute=%s phase=%s generation=%d",
        tostring(continueRoute == true), doorPhase, generation))
    local pulsed = pulseKey("2", function()
        if generation ~= doorGeneration or not botEnabled then
            debugOutput("Двери: результат импульса устарел, продолжение отменено")
            return
        end
        if not continueRoute then
            doorPhase = "open"
            debugOutput("Двери: команда открытия завершена")
            return
        end
        schedule(function()
            if generation ~= doorGeneration or doorPoint ~= currentPoint then
                debugOutput("Двери: запуск отменён новой дверной операцией")
                return
            end
            if botState ~= "STOPPED" or not currentPoint or currentPoint[2] ~= "marker" then
                return
            end
            if not currentPoint.closeRequested then
                debugOutput("Отказываюсь трогаться: сервер не запрашивал закрытие дверей")
                return
            end
            doorPhase = "closed"
            local removed = table.remove(activeRoute, 1)
            debugOutput("Двери закрыты, защитная пауза пройдена; удаляю остановку " .. pointText(removed)
                .. "; осталось точек=" .. tostring(#activeRoute))
            currentPoint.forceStopped = nil
            currentPoint.forceStop = nil
            currentPoint.correctionReversed = nil
            currentPoint.retryCount = nil
            destroyMarker(true)
            currentPoint = nil
            botState = "MOVING"
            stoppedTick = nil
        end, math.random(500, 800), 1)
    end)
    if not pulsed and generation == doorGeneration then
        debugRate("door_input_blocked", 1000,
            "Двери: импульс не прошёл, повторю через 250 мс")
        schedule(function()
            handleDoors(continueRoute, generation)
        end, 250, 1)
    end
end

local function queueDoorAction(continueRoute, minimumDelay, maximumDelay, reason)
    doorGeneration = doorGeneration + 1
    local generation = doorGeneration
    local delay = math.random(minimumDelay, maximumDelay)
    debugOutput(string.format("Двери: ставлю %s через %d мс, generation=%d, reason=%s",
        continueRoute and "закрытие" or "открытие", delay, generation,
        tostring(reason or "server")))
    schedule(function()
        handleDoors(continueRoute, generation)
    end, delay, 1)
end

local function getButtonByText(text)
    if type(hdxGetElementData) ~= "function" then
        return false
    end
    for _, button in ipairs(getElementsByType("hdxButton")) do
        local data = hdxGetElementData(button)
        if data and data.text == text and data.fontSize == 12.78 then
            return button
        end
    end
    return false
end

local function tramContinue()
    if not botEnabled then
        return
    end
    debugOutput("Сервер запросил Tram:AskToContinue; ищу кнопку 'Да'")
    schedule(function()
        local button = getButtonByText("Да")
        if not button then
            notify("Не найдена кнопка продолжения (#BRG9021).", true)
            return
        end
        api.triggerEvent("onHdxElementPressed", button, "left", true)
        debugOutput("Нажимаю HDX-кнопку продолжения маршрута")
        schedule(function()
            api.triggerEvent("onHdxElementPressed", button, "left", false)
        end, math.random(30, 70), 1)
    end, math.random(1500, 3000), 1)
end

local function samePoint(first, second)
    return first and second and first[2] == second[2] and math.abs(first[1] - second[1]) < 1
end

local function recoverRoute()
    local route = routeKey and ROUTES[routeKey]
    local position = isElement(train) and getTrainPosition(train)
    if not route or not position then
        debugOutput("Восстановление маршрута отменено: route=" .. tostring(route ~= nil)
            .. ", position=" .. tostring(position))
        return
    end
    debugOutput(string.format("Восстановление маршрута: position=%.2f direction=%s active=%d",
        position, direction, #activeRoute))
    local original = route[direction]
    local closest
    local closestDistance = math.huge
    for _, point in ipairs(original) do
        if point[2] == "marker" then
            local distance = math.abs(point[1] - position)
            if distance < closestDistance then
                closest = point
                closestDistance = distance
            end
        end
    end
    local removedCount = 0
    if closest then
        debugOutput(string.format("Ближайшая исходная остановка: %s, distance=%.2f",
            pointText(closest), closestDistance))
        for index, point in ipairs(activeRoute) do
            if samePoint(point, closest) then
                for _ = 1, index do
                    table.remove(activeRoute, 1)
                    removedCount = removedCount + 1
                end
                break
            end
        end
    else
        debugOutput("В исходном маршруте не найдена остановка для восстановления")
    end
    destroyMarker(true)
    currentPoint = nil
    botState = "MOVING"
    debugOutput(string.format("Маршрут восстановлен: удалено=%d, осталось=%d, следующая=%s",
        removedCount, #activeRoute, pointText(activeRoute[1])))
end

local function extractMoney(text)
    return tostring(string.match(text or "", "%d+") or "0")
end

local function onNotification(message)
    if not botEnabled or type(message) ~= "string" then
        return
    end
    debugOutput("Уведомление province_tram: " .. message)
    if string.find(message, "Заработано:", 1, true) then
        updateMoney(extractMoney(message))
        debugOutput("Обновлён заработок: " .. money)
    end
    if string.find(message, "Откройте двери и подождите пассажиров", 1, true) then
        if currentPoint and currentPoint[2] == "marker" then
            currentPoint.serverConfirmed = true
            currentPoint.closeRequested = nil
            debugOutput("Сервер подтвердил остановку: разрешено открыть двери")
        end
        doorPhase = "opening"
        debugOutput("Решение: открыть двери и ждать пассажиров")
        queueDoorAction(false, 600, 1200, "сервер запросил открытие")
    elseif string.find(message, "Закройте двери и продолжайте маршрут", 1, true) then
        if currentPoint and currentPoint[2] == "marker" then
            currentPoint.serverConfirmed = true
            currentPoint.closeRequested = true
            debugOutput("Сервер подтвердил остановку: разрешено продолжить маршрут")
        end
        doorPhase = "closing"
        debugOutput("Решение: закрыть двери и удалить текущую остановку")
        queueDoorAction(true, 80, 180, "сервер запросил закрытие")
    elseif string.find(message, "Вы уехали не закрыв дверь", 1, true)
        and currentPoint and currentPoint[2] == "marker" then
        debugOutput("Ошибка двери во время остановки: маршрут НЕ двигаю, отменяю старый таймер и повторяю закрытие")
        setDriveKey("W", false, true)
        setDriveKey("S", false, true)
        setTrainSpeed(train, 0)
        botState = "STOPPED"
        currentPoint.serverConfirmed = true
        currentPoint.closeRequested = true
        doorPhase = "closing"
        queueDoorAction(true, 80, 180, "повтор после ошибки двери")
    elseif string.find(message, "Вы пропустили остановку", 1, true)
        or string.find(message, "Вы тронулись слишком быстро", 1, true)
        or string.find(message, "Вы открыли двери раньше времени", 1, true) then
        debugOutput("Решение: восстановить маршрут после ошибки остановки")
        recoverRoute()
    end
end

local function onVehicleCollision(collider)
    if source ~= getPedOccupiedVehicle(localPlayer) or not collider or not botEnabled then
        return
    end
    local kind = getElementType(collider)
    if kind ~= "player" and kind ~= "ped" and kind ~= "vehicle" then
        return
    end
    local now = getTickCount()
    local previous = collisionContacts[collider]
    collisionContacts[collider] = now
    if previous and now - previous < 1500 then
        return
    end
    debugOutput(string.format("Новый контакт ДТП: type=%s, element=%s, siren=%s",
        tostring(kind), tostring(collider), tostring(sirenEnabled)))
    if not sirenEnabled then
        debugOutput("ДТП: сирена отключена в настройках")
        return
    end
    local remaining = 30000 - (now - lastCollisionTick)
    if lastCollisionTick ~= 0 and remaining > 0 then
        debugOutput(string.format("ДТП: глобальный cooldown, осталось %d мс", remaining))
        return
    end
    lastCollisionTick = now
    notify("Бот попал в ДТП!", true)
    local played = api.alert()
    debugOutput("ДТП: вызов сирены вернул " .. tostring(played == true))
end

local function extractId(text)
    return string.match(text or "", "%[(.-)%]")
end

local function onChatMessage(text, red, green, blue, messageType)
    if not botEnabled then
        return
    end
    if string.find(text, "Светофоры переведены в ночной режим", 1, true) then
        ignoreTraffic = true
        debugOutput("Чат сервера: ночной режим, светофоры игнорируются")
    elseif string.find(text, "Светофоры переведены в дневной режим", 1, true) then
        ignoreTraffic = false
        debugOutput("Чат сервера: дневной режим, светофоры учитываются")
    end
    local id = extractId(text)
    if not id then
        return
    end
    for _, player in ipairs(getElementsByType("player")) do
        if tonumber(getElementData(player, "id") or 0) == tonumber(id) then
            local x, y, z = getElementPosition(player)
            local lx, ly, lz = getElementPosition(localPlayer)
            local distance = getDistanceBetweenPoints3D(x, y, z, lx, ly, lz)
            if distance <= 50 then
                notify(string.format("Игрок %s рядом: %.1f м, dimension %s",
                    getPlayerNametagText(player), distance, tostring(getElementDimension(player))), true)
                api.alert()
            end
            break
        end
    end
end

local function trafficGreen(expected)
    local actual = tonumber(getTrafficLightState())
    local wanted = tonumber(expected)
    local green = actual == wanted
    local point = currentPoint or activeRoute[1]
    local position = isElement(train) and getTrainPosition(train)
    local distance = point and position and math.abs(point[1] - position) or -1
    local signature = string.format("%s:%s:%s", tostring(point and point[1]),
        tostring(wanted), tostring(actual))
    debugChange("traffic", signature, string.format(
        "Вижу светофор %s: фактический=%s, ожидаемый=%s, distance=%.1f -> %s",
        pointText(point), tostring(actual), tostring(wanted), distance,
        green and "ЗЕЛЁНЫЙ" or "СТОП"))
    return green
end

local function scanPassengers()
    local vehicle = train
    local safety = 1
    while isElement(vehicle) and safety < 64 do
        local occupants = getVehicleOccupants(vehicle)
        if type(occupants) == "table" then
            for seat, player in pairs(occupants) do
                if player ~= localPlayer then
                    if not passengerAlerted then
                        notify(getPlayerNametagText(player) .. " вошёл в состав, место " .. tostring(seat) .. ".")
                        api.alert()
                        passengerAlerted = true
                    end
                    return
                end
            end
        end
        vehicle = getVehicleTowedByVehicle(vehicle)
        safety = safety + 1
    end
    passengerAlerted = false
end

local lastPassengerScan = 0

local function removeCurrentPoint(reason)
    local removed = table.remove(activeRoute, 1)
    debugOutput((reason or "Точка удалена") .. ": " .. pointText(removed)
        .. "; осталось=" .. tostring(#activeRoute)
        .. "; следующая=" .. pointText(activeRoute[1]))
    debugMemory.traffic = nil
    debugMemory.target = nil
    currentPoint = nil
    expectedBrakeDistance = nil
    brakeStartPosition = nil
    stoppedTick = nil
end

local function beginBraking(point, position, distance, triggerDistance)
    botState = "BRAKING"
    currentPoint = point
    stopFrameCount = 0
    lastPosition = position
    expectedBrakeDistance = distance
    brakeStartPosition = position
    local actualDistance = math.abs((tonumber(point[1]) or position) - position)
    debugOutput(string.format(
        "Начинаю торможение: target=%s, train=%.2f, до точки=%.2f, тормоз=%.2f, порог=%.2f, speed=%.3f",
        pointText(point), position, actualDistance, distance,
        triggerDistance or distance,
        math.abs(getTrainSpeed(train) or 0)))
    if point[2] == "marker" and not markerCol then
        createMarkerCollider()
    end
end

-- Main driving state machine.
local function onBrakeRender()
    if not botEnabled then
        return
    end
    if not isElement(train) or getPedOccupiedVehicle(localPlayer) ~= train then
        stopBot("Вероятно, вас высадил администратор.")
        api.alert()
        return
    end
    local blocked = inputBlocked()
    debugChange("input", blocked, blocked and
        "Управление приостановлено: открыто меню DarkFlame или чат"
        or "Управление доступно: меню и чат закрыты")
    if blocked then
        setDriveKey("W", false)
        setDriveKey("S", false)
        return
    end

    local position = getTrainPosition(train)
    local speed = math.abs(getTrainSpeed(train) or 0)
    if not position then
        return
    end
    local stoppingDistance = brakeDistance(speed)
    local desiredW = false
    local desiredS = false
    local now = getTickCount()

    if now - lastPassengerScan >= 1000 then
        scanPassengers()
        lastPassengerScan = now
    end

    if botState ~= lastBotState then
        debugOutput(string.format("Состояние: %s -> %s; target=%s",
            tostring(lastBotState or "nil"), tostring(botState),
            pointText(currentPoint or activeRoute[1])))
        lastBotState = botState
        if updateNativeMenu then
            updateNativeMenu()
        end
    end

    local observedPoint = currentPoint or activeRoute[1]
    local observedDistance = observedPoint and math.abs(observedPoint[1] - position) or -1
    debugChange("target", tostring(observedPoint) .. ":" .. botState,
        string.format("Текущая цель: %s; remaining=%d; distance=%.2f; speed=%.3f",
            pointText(observedPoint), #activeRoute, observedDistance, speed))
    debugRate("telemetry", 3000, string.format(
        "Телеметрия: state=%s position=%.2f speed=%.3f brake=%.2f target=%s distance=%.2f",
        botState, position, speed, stoppingDistance, pointText(observedPoint), observedDistance))

    if botState == "MOVING" then
        desiredW = true
        if #activeRoute == 0 then
            debugOutput("Маршрут закончился; меняю направление " .. direction .. " -> "
                .. (direction == "forward" and "backward" or "forward"))
            generateRoute(direction == "forward" and "backward" or "forward")
        end
        local point = activeRoute[1]
        if point then
            if ignoreTraffic and point[2] == "traffic" then
                removeCurrentPoint("Светофор проигнорирован")
            else
                local distance = math.abs(point[1] - position)
                if point[2] == "marker" and point.forceStop then
                    if not markerCol and (not point.colliderRetryTick
                        or now - point.colliderRetryTick >= 1000) then
                        point.colliderRetryTick = now
                        createMarkerCollider()
                    end
                    local delta = point[1] - position
                    local reverse = delta < -1
                    point.correctionReversed = reverse
                    desiredW = not reverse
                    desiredS = reverse
                    debugChange("correction", tostring(point) .. ":" .. tostring(reverse),
                        string.format("Доводка к колшейпу: target=%.2f position=%.2f delta=%.2f, жму %s",
                            point[1], position, delta, reverse and "S" or "W"))
                else
                    local triggerDistance = stoppingDistance
                        + (point[2] == "marker" and MARKER_BRAKE_MARGIN or 0)
                    if distance <= triggerDistance then
                        if point[2] == "traffic" and trafficGreen(point[3]) then
                            removeCurrentPoint("Светофор зелёный")
                        else
                            beginBraking(point, position, stoppingDistance, triggerDistance)
                        end
                    end
                end
            end
        end
    elseif botState == "BRAKING" then
        desiredS = true
        if currentPoint and currentPoint[2] == "traffic"
            and (ignoreTraffic or trafficGreen(currentPoint[3])) then
            botState = "MOVING"
            removeCurrentPoint("Светофор разрешил движение")
        elseif currentPoint and currentPoint[2] == "marker"
            and not markerEntered and speed <= 0.04 then
            local remaining = math.abs(currentPoint[1] - position)
            currentPoint.forceStop = true
            currentPoint.retryCount = (currentPoint.retryCount or 0) + 1
            expectedBrakeDistance = nil
            brakeStartPosition = nil
            botState = "MOVING"
            desiredS = false
            desiredW = true
            debugOutput(string.format(
                "Предварительную остановку отменяю: колшейп не задет, remaining=%.2f speed=%.3f; перехожу на доводку",
                remaining, speed))
        elseif speed <= 0.01 then
            if math.abs(position - lastPosition) <= 0.1 then
                stopFrameCount = stopFrameCount + 1
            else
                stopFrameCount = 0
            end
            lastPosition = position
            if stopFrameCount >= 5 then
                botState = "STOPPED"
                setTrainSpeed(train, 0)
                stoppedTick = now
                debugOutput(string.format("Полная остановка: position=%.2f target=%s",
                    position, pointText(currentPoint)))
                if expectedBrakeDistance and brakeStartPosition then
                    local actual = math.abs(position - brakeStartPosition)
                    debugOutput(string.format(
                        "Проверка торможения: ожидалось=%.2f, фактически=%.2f, ошибка=%.2f",
                        expectedBrakeDistance, actual, actual - expectedBrakeDistance))
                    if actual - expectedBrakeDistance >= 9 then
                        if currentPoint and currentPoint[2] == "marker" then
                            debugOutput("Перелёт остановки: точку НЕ удаляю, включаю доводку по колшейпу")
                            markerEntered = false
                            currentPoint.forceStop = true
                            createMarkerCollider()
                            botState = "MOVING"
                        else
                            removeCurrentPoint("Светофор пройден с перелётом")
                            destroyMarker(true)
                            botState = "MOVING"
                        end
                    end
                    expectedBrakeDistance = nil
                    brakeStartPosition = nil
                end
            end
        else
            stopFrameCount = 0
        end
    elseif botState == "STOPPED" then
        if currentPoint and currentPoint[2] == "traffic" then
            local green = ignoreTraffic or trafficGreen(currentPoint[3])
            local timeout = stoppedTick and now - stoppedTick > 20000
            if green or timeout then
                debugOutput("Продолжаю после светофора: причина="
                    .. (ignoreTraffic and "игнор" or green and "зелёный" or "таймаут 20с"))
                removeCurrentPoint("Светофор завершён")
                botState = "MOVING"
            end
        elseif currentPoint and currentPoint[2] == "marker" then
            if not markerEntered and markerCol then
                currentPoint.retryCount = (currentPoint.retryCount or 0) + 1
                currentPoint.forceStop = true
                botState = "MOVING"
                debugOutput("Остановка вне колшейпа: включаю доводку, попытка="
                    .. tostring(currentPoint.retryCount))
            elseif not markerEntered and not markerCol then
                currentPoint.forceStop = true
                createMarkerCollider()
                botState = "MOVING"
                debugOutput("Колшейп потерян: пересоздан, включаю доводку")
            elseif stoppedTick and now - stoppedTick > 20000 then
                debugOutput("Сервер не подтвердил остановку за 20с: повторяю вход в колшейп")
                markerEntered = false
                currentPoint.forceStop = true
                createMarkerCollider()
                botState = "MOVING"
            end
        end
    end

    debugChange("drive", tostring(desiredW) .. ":" .. tostring(desiredS),
        string.format("Команды движения: W=%s, S=%s", tostring(desiredW), tostring(desiredS)))
    safeDriveKey("W", desiredW)
    safeDriveKey("S", desiredS)
end

local lightSequence = 0

local function queueLightSequence(keys)
    lightSequence = lightSequence + 1
    local generation = lightSequence
    local index = 1

    local function pressNext()
        if generation ~= lightSequence or index > #keys then
            return
        end
        if inputBlocked() then
            schedule(pressNext, 100, 1)
            return
        end

        local key = keys[index]
        if not pulseKey(key, function()
            index = index + 1
            schedule(pressNext, 80, 1)
        end) then
            schedule(pressNext, 100, 1)
        end
    end

    pressNext()
end

local function startCruiseLights()
    queueLightSequence({"L", "L", "K"})
end

local function stopCruiseLights()
    queueLightSequence({"L", "K"})
end

stopBot = function(reason)
    local wasEnabled = botEnabled
    debugOutput("Остановка бота: reason=" .. tostring(reason or "ручная")
        .. ", state=" .. tostring(botState) .. ", target=" .. pointText(currentPoint))
    if wasEnabled then
        botEnabled = false
        removeHandler("onClientRender", root, onBrakeRender)
    end
    botState = "IDLE"
    doorGeneration = doorGeneration + 1
    doorPhase = "idle"
    releaseKeys()
    if wasEnabled then
        stopCruiseLights()
    end
    destroyMarker(true)
    currentPoint = nil
    if reason then
        notify(reason, true)
    else
        notify("Бот выключен.")
    end
    if updateNativeMenu then
        updateNativeMenu()
    end
end

local function startBot()
    local vehicle = getPedOccupiedVehicle(localPlayer)
    if not vehicle or getVehicleType(vehicle) ~= "Train" then
        notify("Сядьте в трамвай.", true)
        return false
    end
    if not routeKey then
        notify("Сначала выберите маршрут или примите работу.", true)
        return false
    end
    train = vehicle
    debugOutput(string.format("Запуск: model=%s route=%s direction=%s activePoints=%d",
        tostring(getElementModel(train)), tostring(routeKey), direction, #activeRoute))
    if not next(measuredBrakeData) and not chooseBrakeProfile(train) then
        return false
    end
    if #activeRoute == 0 and not generateRoute(direction) then
        return false
    end
    botEnabled = true
    botState = "MOVING"
    stopFrameCount = 0
    lastBotState = nil
    addHandler("onClientRender", root, onBrakeRender)
    startCruiseLights()
    notify("Бот включен.")
    debugOutput("Бот запущен; первая цель=" .. pointText(activeRoute[1]))
    if updateNativeMenu then
        updateNativeMenu()
    end
    return true
end

local function onForceStop()
    if forceStopper and botEnabled and isElement(train) then
        setTrainSpeed(train, 0)
    end
end

-- Integrated braking calibrator.
local calibration = {
    active = false,
    index = 0,
    data = {},
    stopFrames = 0,
}

local onCalibrationWait
local onCalibrationStop

cancelCalibration = function(message)
    if not calibration.active then
        return
    end
    calibration.active = false
    removeHandler("onClientRender", root, onCalibrationWait)
    removeHandler("onClientRender", root, onCalibrationStop)
    setDriveKey("S", false, true)
    if message then
        notify(message, true)
    end
    if updateNativeMenu then
        updateNativeMenu()
    end
end

local function finishCalibration()
    calibration.active = false
    measuredBrakeData = copyTable(calibration.data)
    setDriveKey("S", false, true)
    notify("Калибровка завершена для модели " .. tostring(getElementModel(train)) .. ".")
    for _, speed in ipairs(SPEEDS) do
        outputConsole(string.format("[TramBot calibration] %.2f m/s -> %.2f m", speed, measuredBrakeData[speed]))
    end
    if updateNativeMenu then
        updateNativeMenu()
    end
end

local function calibrationNext()
    if not calibration.active or not isElement(train) then
        cancelCalibration("Калибровка отменена: трамвай потерян.")
        return
    end
    calibration.index = calibration.index + 1
    if calibration.index > #SPEEDS then
        finishCalibration()
        return
    end
    calibration.speed = SPEEDS[calibration.index]
    setTrainSpeed(train, calibration.speed)
    notify(string.format("Калибровка %d/%d: %.2f m/s", calibration.index, #SPEEDS, calibration.speed))
    addHandler("onClientRender", root, onCalibrationWait)
    if updateNativeMenu then
        updateNativeMenu()
    end
end

onCalibrationWait = function()
    if not calibration.active or not isElement(train) then
        cancelCalibration("Калибровка отменена.")
        return
    end
    local speed = math.abs(getTrainSpeed(train) or 0)
    if speed < calibration.speed * 0.98 then
        return
    end
    removeHandler("onClientRender", root, onCalibrationWait)
    calibration.startPosition = getTrainPosition(train)
    calibration.lastPosition = calibration.startPosition
    calibration.lastDrift = nil
    calibration.wrapDetected = false
    calibration.wrapStart = nil
    calibration.stopFrames = 0
    setDriveKey("S", true, true)
    addHandler("onClientRender", root, onCalibrationStop)
end

onCalibrationStop = function()
    if not calibration.active or not isElement(train) then
        cancelCalibration("Калибровка отменена.")
        return
    end
    local speed = math.abs(getTrainSpeed(train) or 0)
    local position = getTrainPosition(train)
    if not calibration.wrapDetected and calibration.lastPosition and position < calibration.lastPosition then
        calibration.wrapDetected = true
        calibration.wrapStart = calibration.lastPosition
    end
    calibration.lastPosition = position
    if speed > 0.01 then
        calibration.stopFrames = 0
        calibration.lastDrift = position
        return
    end
    if calibration.lastDrift and math.abs(position - calibration.lastDrift) <= 0.1 then
        calibration.stopFrames = calibration.stopFrames + 1
    else
        calibration.stopFrames = 0
    end
    calibration.lastDrift = position
    if calibration.stopFrames < 5 then
        return
    end
    removeHandler("onClientRender", root, onCalibrationStop)
    setDriveKey("S", false, true)
    local distance
    if calibration.wrapDetected then
        distance = math.abs((calibration.wrapStart - calibration.startPosition) + position)
    else
        distance = math.abs(position - calibration.startPosition)
    end
    calibration.data[calibration.speed] = distance
    notify(string.format("%.2f m/s -> %.2f м", calibration.speed, distance))
    schedule(calibrationNext, 2000, 1)
end

local function startCalibration()
    if calibration.active then
        notify("Калибровка уже идёт.", true)
        return false
    end
    local vehicle = getPedOccupiedVehicle(localPlayer)
    if not vehicle or getVehicleType(vehicle) ~= "Train" then
        notify("Для калибровки сядьте в трамвай.", true)
        return false
    end
    if botEnabled then
        stopBot()
    end
    train = vehicle
    calibration.active = true
    calibration.index = 0
    calibration.data = {}
    calibrationNext()
    return true
end

-- Route recorder and native DarkFlame menu commands.
local function saveRoutePoint(kind, state)
    local vehicle = getPedOccupiedVehicle(localPlayer)
    if not vehicle or getVehicleType(vehicle) ~= "Train" then
        notify("Вы не в трамвае.", true)
        return
    end
    local position = getTrainPosition(vehicle)
    if not position then
        notify("Не удалось получить участок пути.", true)
        return
    end
    routeTable[#routeTable + 1] = {position, kind, tostring(state or "228")}
    notify(kind == "marker" and "Участок сохранён." or "Светофор сохранён.")
    debugOutput(string.format("Запись маршрута #%d: %s", #routeTable,
        pointText(routeTable[#routeTable])))
end

local function dumpRouteTable()
    debugOutput("Дамп routeTable: точек=" .. tostring(#routeTable))
    outputConsole("local route_table = {")
    for _, point in ipairs(routeTable) do
        outputConsole(string.format("    { %.14g, %q, %q },", point[1], point[2], point[3]))
    end
    outputConsole("}")
    notify("Таблица выведена в консоль.")
end

local function startJob(key)
    local route = ROUTES[key]
    if not route or not selectRoute(key, true) then
        return
    end
    local resource = getResourceFromName("province_tram")
    local resourceRoot = resource and getResourceRootElement(resource)
    if not resourceRoot then
        notify("Ресурс province_tram не найден.", true)
        return
    end
    debugOutput(string.format("Отправляю Tram:onJobAccepted: route=%s depot=%s line=%s",
        key, tostring(route.server[1]), tostring(route.server[2])))
    api.triggerServerEvent("Tram:onJobAccepted", resourceRoot, route.server[1], route.server[2])
end

local function pushNativeState()
    local routeName = routeKey and ROUTES[routeKey].name or "не выбран"
    local state = botState
    if calibration.active then
        state = string.format("CALIBRATION %d/%d", calibration.index, #SPEEDS)
    end
    api.updateState("loaded", "1")
    api.updateState("bot", botEnabled and "1" or "0")
    api.updateState("traffic", ignoreTraffic and "1" or "0")
    api.updateState("debug", debugEnabled and "1" or "0")
    api.updateState("siren", sirenEnabled and "1" or "0")
    api.updateState("force", forceStopper and "1" or "0")
    api.updateState("route", routeName)
    api.updateState("direction", direction)
    api.updateState("status", state)
    api.updateState("money", money)
    api.updateState("catcher", eventCatcher and "активен" or "ошибка")
end

updateNativeMenu = pushNativeState

local function handleCommand(command)
    local name, value = tostring(command):match("^([^:]+):?(.*)$")
    if name ~= "debug" then
        debugOutput("Команда ImGui: " .. tostring(name) .. ":" .. tostring(value))
    end
    if name == "bot" then
        if value == "1" then
            startBot()
        else
            stopBot()
        end
    elseif name == "traffic" then
        ignoreTraffic = value == "1"
    elseif name == "debug" then
        local enabled = value == "1"
        if debugEnabled and not enabled then
            debugOutput("Отладка выключена пользователем")
        end
        debugEnabled = enabled
        debugMemory = {}
        debugTicks = {}
        if debugEnabled then
            debugOutput(string.format(
                "Отладка включена: bot=%s state=%s route=%s direction=%s points=%d",
                tostring(botEnabled), botState, tostring(routeKey), direction, #activeRoute))
        end
    elseif name == "siren" then
        sirenEnabled = value == "1"
    elseif name == "force" then
        forceStopper = value == "1"
    elseif name == "route" then
        startJob(value)
    elseif name == "save_marker" then
        saveRoutePoint("marker", "228")
    elseif name == "save_traffic" then
        saveRoutePoint("traffic", value ~= "" and value or "3")
    elseif name == "clear_route" then
        debugOutput("Очистка routeTable: удаляется точек=" .. tostring(#routeTable))
        routeTable = {}
        notify("Таблица маршрута очищена.")
    elseif name == "dump_route" then
        dumpRouteTable()
    elseif name == "test_traffic" then
        notify("Состояние светофора: " .. tostring(getTrafficLightState()))
    elseif name == "calibrate" then
        schedule(startCalibration, 200, 1)
    elseif name == "test_siren" then
        local played = api.alert()
        debugOutput("Тест сирены: dfPlayAlertSignal вернул " .. tostring(played == true))
        if not played then
            notify("Не удалось запустить сирену — смотри DarkFlame.log", true)
        end
    elseif name == "admin" then
        outputChatBox("#00FF00[ТРЕВОГА] Админ крикнул: #FF6600" .. value, 255, 255, 255, true)
    elseif name == "force_stop" and isElement(train) then
        debugOutput("Force-stop из ImGui: setTrainSpeed(0)")
        setTrainSpeed(train, 0)
    end
    pushNativeState()
end

local function pollNativeMenu()
    for _ = 1, 32 do
        local command = api.takeCommand()
        if not command then
            break
        end
        handleCommand(command)
    end
    pushNativeState()
end

-- Bootstrap lifecycle.
local function cleanup()
    if cleaning then
        return
    end
    cleaning = true
    botEnabled = false
    calibration.active = false
    releaseKeys()
    if eventCatcher then
        api.removeEventCatcher(eventCatcher)
        eventCatcher = nil
    end
    unbindKey("space", "down", onForceStop)
    for timer in pairs(timers) do
        if isTimer(timer) then
            killTimer(timer)
        end
    end
    timers = {}
    if markerCol and isElement(markerCol) then
        destroyElement(markerCol)
    end
    markerCol = nil
    markerDebug = nil
    for index = #handlers, 1, -1 do
        local item = handlers[index]
        api.removeEventHandler(item[1], item[2], item[3])
    end
    handlers = {}
    api.updateState("loaded", "0")
end

math.randomseed(getTickCount())

api.addEvent("Tram:AskToContinue", true)
api.addEvent("province:sendNotification", true)
addHandler("Tram:AskToContinue", root, tramContinue)
addHandler("province:sendNotification", root, onNotification)
addHandler("onClientVehicleCollision", root, onVehicleCollision)
addHandler("onClientChatMessage", root, onChatMessage)
addHandler("onClientRender", root, drawDebugCollider)
addHandler("onClientResourceStop", root, function(stoppedResource)
    if stoppedResource == getThisResource() or source == resourceRoot then
        cleanup()
    end
end)

-- skip=1 omits the source element, so the callback receives depot and line.
eventCatcher = api.catchServerEvent("Tram:onJobAccepted", 1, determineRoute)

bindKey("space", "down", onForceStop)
schedule(pollNativeMenu, 100, 0)
pushNativeState()

if type(onUnload) == "function" then
    onUnload(cleanup)
end

notify("Загружен. Event catcher: "
    .. (eventCatcher and "OK" or "ошибка"), not eventCatcher)

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
local markerEntered = false
local botEnabled = false
local ignoreTraffic = false
local debugEnabled = false
local sirenEnabled = true
local forceStopper = false
local passengerAlerted = false
local lastCollisionTick = 0
local stopFrameCount = 0
local lastPosition = 0
local stoppedTick
local expectedBrakeDistance
local brakeStartPosition
local money = "0"
local cleaning = false
local eventCatcher
local keyState = {}

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
        outputChatBox("#E6A15C[TramBot debug] #FFFFFF" .. tostring(text), 255, 255, 255, true)
    end
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
    local continuous = key == "W" or key == "S"
    if not force and not continuous and keyState[key] == pressed then
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
        removeHandler("onClientColShapeHit", markerCol, onMarkerHit)
        destroyElement(markerCol)
    end
    markerCol = nil
    if resetEntered then
        markerEntered = false
    end
end

onMarkerHit = function(element)
    if element ~= train then
        return
    end
    markerEntered = true
    if currentPoint and currentPoint.forceStop then
        setTrainSpeed(train, 0)
        botState = "STOPPED"
        currentPoint.forceStopped = true
        debugOutput("Force-stop внутри маркера")
    end
    destroyMarker(false)
end

local function createMarkerCollider()
    destroyMarker(true)
    if not isElement(train) then
        return false
    end
    local tx, ty, tz = getElementPosition(train)
    local closest
    local closestDistance = math.huge
    for _, marker in ipairs(getElementsByType("marker")) do
        local radius = getMarkerSize(marker)
        if getMarkerType(marker) == "checkpoint" and radius > 10 then
            local x, y, z = getElementPosition(marker)
            local distance = getDistanceBetweenPoints3D(tx, ty, tz, x, y, z)
            if distance < closestDistance then
                closest = marker
                closestDistance = distance
            end
        end
    end
    if not closest or closestDistance >= 150 then
        debugOutput("Checkpoint для collider не найден")
        return false
    end
    local x, y, z = getElementPosition(closest)
    markerCol = createColSphere(x, y, z, getMarkerSize(closest))
    markerEntered = false
    if markerCol then
        addHandler("onClientColShapeHit", markerCol, onMarkerHit)
        return true
    end
    return false
end

local function pulseKey(key, after)
    if not safeDriveKey(key, true) then
        return false
    end
    schedule(function()
        setDriveKey(key, false, true)
        if after then
            after()
        end
    end, math.random(30, 100), 1)
    return true
end

local function handleDoors(continueRoute)
    pulseKey("2", function()
        if not continueRoute then
            return
        end
        schedule(function()
            if botState ~= "STOPPED" or not currentPoint or currentPoint[2] ~= "marker" then
                return
            end
            local removed = table.remove(activeRoute, 1)
            debugOutput("Остановка удалена: " .. tostring(removed and removed[1]))
            currentPoint.forceStopped = nil
            currentPoint.forceStop = nil
            currentPoint.retryCount = nil
            destroyMarker(true)
            currentPoint = nil
            botState = "MOVING"
            stoppedTick = nil
        end, math.random(30, 100), 1)
    end)
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
    schedule(function()
        local button = getButtonByText("Да")
        if not button then
            notify("Не найдена кнопка продолжения (#BRG9021).", true)
            return
        end
        api.triggerEvent("onHdxElementPressed", button, "left", true)
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
        return
    end
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
    if closest then
        for index, point in ipairs(activeRoute) do
            if samePoint(point, closest) then
                for _ = 1, index do
                    table.remove(activeRoute, 1)
                end
                break
            end
        end
    end
    destroyMarker(true)
    currentPoint = nil
    botState = "MOVING"
end

local function extractMoney(text)
    return tostring(string.match(text or "", "%d+") or "0")
end

local function onNotification(message)
    if not botEnabled or type(message) ~= "string" then
        return
    end
    if string.find(message, "Заработано:", 1, true) then
        updateMoney(extractMoney(message))
    end
    if string.find(message, "Откройте двери и подождите пассажиров", 1, true) then
        schedule(function() handleDoors(false) end, math.random(600, 1500), 1)
    elseif string.find(message, "Закройте двери и продолжайте маршрут", 1, true) then
        schedule(function() handleDoors(true) end, math.random(600, 1500), 1)
    elseif string.find(message, "Вы пропустили остановку", 1, true)
        or string.find(message, "Вы тронулись слишком быстро", 1, true)
        or string.find(message, "Вы уехали не закрыв дверь", 1, true)
        or string.find(message, "Вы открыли двери раньше времени", 1, true) then
        recoverRoute()
        if string.find(message, "двер", 1, true) then
            handleDoors(false)
        end
    end
end

local function onVehicleCollision(collider)
    if source ~= getPedOccupiedVehicle(localPlayer) or not collider or not botEnabled or not sirenEnabled then
        return
    end
    local kind = getElementType(collider)
    if kind ~= "player" and kind ~= "ped" and kind ~= "vehicle" then
        return
    end
    local now = getTickCount()
    if now - lastCollisionTick >= 30000 then
        notify("Бот попал в ДТП!", true)
        api.alert()
        lastCollisionTick = now
    end
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
    elseif string.find(text, "Светофоры переведены в дневной режим", 1, true) then
        ignoreTraffic = false
    end
    if messageType == 0 and red == 255 and green == 164 and blue == 104
        and string.find(text, "Администратор", 1, true) then
        api.alert()
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
    return tonumber(getTrafficLightState()) == tonumber(expected)
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
    debugOutput((reason or "Точка удалена") .. ": " .. tostring(removed and removed[1]))
    currentPoint = nil
    expectedBrakeDistance = nil
    brakeStartPosition = nil
    stoppedTick = nil
end

local function beginBraking(point, position, distance)
    botState = "BRAKING"
    currentPoint = point
    stopFrameCount = 0
    lastPosition = position
    expectedBrakeDistance = distance
    brakeStartPosition = position
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
    if inputBlocked() then
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
        debugOutput("Состояние: " .. tostring(botState))
        lastBotState = botState
        if updateNativeMenu then
            updateNativeMenu()
        end
    end

    if botState == "MOVING" then
        desiredW = true
        if #activeRoute == 0 then
            generateRoute(direction == "forward" and "backward" or "forward")
        end
        local point = activeRoute[1]
        if point then
            if ignoreTraffic and point[2] == "traffic" then
                removeCurrentPoint("Светофор проигнорирован")
            else
                local distance = math.abs(point[1] - position)
                if point[2] == "marker" and point.forceStop then
                    if not markerCol then
                        createMarkerCollider()
                    end
                elseif distance <= stoppingDistance then
                    if point[2] == "traffic" and trafficGreen(point[3]) then
                        removeCurrentPoint("Светофор зелёный")
                    else
                        beginBraking(point, position, stoppingDistance)
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
                if expectedBrakeDistance and brakeStartPosition then
                    local actual = math.abs(position - brakeStartPosition)
                    if actual - expectedBrakeDistance >= 9 then
                        removeCurrentPoint("Точка пройдена с перелётом")
                        destroyMarker(true)
                        botState = "MOVING"
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
            if ignoreTraffic or trafficGreen(currentPoint[3]) or stoppedTick and now - stoppedTick > 20000 then
                removeCurrentPoint("Светофор завершён")
                botState = "MOVING"
            end
        elseif currentPoint and currentPoint[2] == "marker" then
            if not markerEntered and markerCol then
                if not currentPoint.retryCount then
                    currentPoint.retryCount = 1
                    botState = "MOVING"
                    debugOutput("Недоезд до маркера, повторная попытка")
                else
                    currentPoint.forceStop = true
                    botState = "MOVING"
                    createMarkerCollider()
                    debugOutput("Повторный недоезд, force-stop")
                end
            elseif stoppedTick and now - stoppedTick > 20000 then
                removeCurrentPoint("Остановка зависла")
                destroyMarker(true)
                botState = "MOVING"
            end
        end
    end

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
    if wasEnabled then
        botEnabled = false
        removeHandler("onClientRender", root, onBrakeRender)
    end
    botState = "IDLE"
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
end

local function dumpRouteTable()
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
    if name == "bot" then
        if value == "1" then
            startBot()
        else
            stopBot()
        end
    elseif name == "traffic" then
        ignoreTraffic = value == "1"
    elseif name == "debug" then
        debugEnabled = value == "1"
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
        routeTable = {}
        notify("Таблица маршрута очищена.")
    elseif name == "dump_route" then
        dumpRouteTable()
    elseif name == "test_traffic" then
        notify("Состояние светофора: " .. tostring(getTrafficLightState()))
    elseif name == "calibrate" then
        schedule(startCalibration, 200, 1)
    elseif name == "test_siren" then
        api.alert()
    elseif name == "admin" then
        outputChatBox("#00FF00[ТРЕВОГА] Админ крикнул: #FF6600" .. value, 255, 255, 255, true)
    elseif name == "force_stop" and isElement(train) then
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

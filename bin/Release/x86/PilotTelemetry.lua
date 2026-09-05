-- BEGIN EMBEDDED PILOT CONTROLLER
local PilotController = (function()
-- Pure controller: no game APIs, resource loading or server events.
local Controller = {}
Controller.__index = Controller
Controller.version = "0.1.10"

local function finite(v) return type(v) == "number" and v == v and math.abs(v) < math.huge end
local function clamp(v, low, high) return math.max(low, math.min(high, v)) end
local function angle(v) return (v + 180) % 360 - 180 end
local function elapsed(now, before) return before and (now - before) % 4294967296 or 0 end
local function neutral() return {throttle = 0, brake = 0, rudder = 0, aileron = 0, elevator = 0, handbrake = false} end
local function vector(v) return type(v) == "table" and finite(v[1]) and finite(v[2]) and finite(v[3]) end

function Controller.new()
    return setmetatable({enabled = false, instruction = "unknown", phase = "off", status = "Выключен",
        output = neutral(), telemetry = true, waypoint = 0}, Controller)
end

function Controller:stop(reason)
    self.enabled, self.output = false, neutral()
    self.phase, self.status = "off", reason or "Остановлен"
    return self.output
end

function Controller:notify(text, now)
    if type(text) ~= "string" then return end
    if text:find("Вы выполнили рейс", 1, true) then
        self.terminal = true
        self:stop("Рейс выполнен")
        return "completed"
    end
    for _, word in ipairs({"уволены", "уволен", "Уволен", "увольнен", "увольнены", "Вы не успели", "работа завершена"}) do
        if text:find(word, 1, true) then
            self.terminal = true
            self:stop("Работа завершена / увольнение")
            return "job_ended"
        end
    end
    local mode
    if text:find("Ваш пункт назначения", 1, true) then mode = "boarding_move"; self.terminal = false
    elseif text:find("Двигайтесь аккуратно назад", 1, true) then mode = "reverse"
    elseif text:find("Двигайтесь медленно", 1, true) then mode = "taxi"
    elseif text:find("Приготовьтесь к взлету", 1, true) or text:find("Приготовьтесь к взлёту", 1, true) then mode = "wait_clearance"
    elseif text:find("Взлет разрешен", 1, true) or text:find("Взлёт разрешен", 1, true) then mode = "takeoff"
    elseif text:find("Ожидайте", 1, true) and text:find("пассажир", 1, true) then mode = "passengers"
    elseif text:find("Выпустите шасси", 1, true) then self.landing = true
    end
    if mode == "takeoff" then self.takeoffPermit = now or self.lastTick or 0 end
    if mode and mode ~= self.instruction then
        self.pushbackPending = self.enabled and self.instruction == "reverse" and mode == "taxi" or false
        self.pushbackNoticeTick, self.pushback = now or self.lastTick, false
        self.parkingHold = nil
        self.taxiTarget, self.taxiReentry = nil, nil
        self.instruction = mode
        if mode == "takeoff" or mode == "reverse" or mode == "boarding_move" then
            self.finalLanding, self.landingHeading = false, nil
            self.flightSeen, self.landing = false, false
            self.takeoffRolling = false
            self.descendingWaypoints = 0
        end
    end
    return mode
end

function Controller:start(data, now)
    if self.terminal then return false, "Работа закончена. Сначала начни новый рейс." end
    if not data or not data.vehicle or data.driver ~= true or data.model ~= 519 then
        return false, "Нужен самолёт модели 519 и место пилота."
    end
    if not finite(data.heading_deg) or not finite(data.speed_kmh) or not finite(data.pitch_deg) or not finite(data.roll_deg) then
        return false, "Нет достоверных данных положения самолёта."
    end
    self.enabled, self.vehicle, self.world = true, data.vehicle, tostring(data.dimension) .. ":" .. tostring(data.interior)
    self.lastTick, self.started, self.navPosition, self.navId = now, now, nil, nil
    self.groundSince, self.airSince, self.missingSince = nil, nil, nil
    self.airborne = data.on_ground == false and finite(data.agl_terrain_m) and data.agl_terrain_m > 3
    self.flightSeen = self.airborne or false
    self.waypoint, self.output, self.clearSince = 0, neutral(), nil
    self.closest, self.passedSince, self.blocked = nil, nil, false
    self.lastPosition, self.filteredRates = data.position_m, {yaw = 0, pitch = 0, roll = 0}
    self.descendingWaypoints = 0
    self.takeoffRolling = data.speed_kmh >= 70
    local forward = data.velocity_body_rfu_mps and data.velocity_body_rfu_mps[2] or 0
    self.groundDirection = forward < -0.25 and -1 or forward > 0.25 and 1 or self.instruction == "reverse" and -1 or 1
    self.directionSettled, self.pushbackPending, self.pushback = nil, false, false
    self.parkingHold = nil
    self.taxiTarget, self.taxiReentry = nil, nil
    self.turnDirection, self.ringPass = nil, nil
    self.groundSteerReleased = nil
    self.finalLanding, self.landingHeading = false, nil
    self.contactAgl = data.on_ground == true and finite(data.agl_terrain_m)
        and clamp(data.agl_terrain_m, 0.5, 3) or 1.15
    if self.airborne and data.landing_gear_down == true and data.speed_kmh < 210
        and finite(data.climb_mps) and data.climb_mps < -1 and data.agl_terrain_m < 100 then self.landing = true end
    self.phase, self.status = "armed", "Включён"
    return true
end

local function stopMotion(out, data)
    local forward = data.velocity_body_rfu_mps and data.velocity_body_rfu_mps[2]
    if finite(forward) and forward < -0.3 then out.throttle = 0.4
    elseif data.speed_kmh > 1 then out.brake = 0.55 end
    out.handbrake = data.speed_kmh < 3
end

function Controller:taxiReentryIntent(data, now)
    local nav = data.navigation
    local color = type(nav) == "table" and nav.color_rgba or {}
    if data.on_ground ~= true or self.airborne or type(nav) ~= "table" or nav.marker_type ~= "checkpoint"
        or not finite(color[1]) or not finite(color[2]) or not finite(color[3])
        or color[1] < 200 or color[2] > 80 or color[3] > 80 then
        self.taxiTarget, self.taxiReentry = nil, nil
        return
    end
    local size = nav.marker_size_m
    if not finite(size) or size <= 0 or not vector(nav.position) or not vector(data.position_m) then return end
    local target = self.taxiTarget
    local changed = not target or target.id ~= nav.id or target.size ~= size
    if not changed then
        for i = 1, 3 do if math.abs(target.position[i] - nav.position[i]) > 0.25 then changed = true end end
    end
    if changed then
        target = {id = nav.id, position = {nav.position[1], nav.position[2], nav.position[3]}, size = size}
        self.taxiTarget, self.taxiReentry = target, nil
    end
    local recovery = self.taxiReentry
    if not recovery and nav.marker_inside == true then
        recovery = {stage = "wait", since = now, reason = changed and "target_appeared_inside" or "entry_not_acknowledged"}
        self.taxiReentry = recovery
    end
    if not recovery then return end
    if recovery.stage == "wait" and elapsed(now, recovery.since) >= 1000 then
        if nav.marker_inside ~= true then recovery.failure = "Нет подтверждения наземной метки после выхода"
        elseif size > 60 then recovery.failure = "Слишком большая метка для повторного захода"
        else
            local heading = math.rad(data.heading_deg)
            local fx, fy = math.sin(heading), math.cos(heading)
            local dx, dy = nav.position[1] - data.position_m[1], nav.position[2] - data.position_m[2]
            local along, cross = dx * fx + dy * fy, dx * fy - dy * fx
            local radius = size + 2
            local half = math.sqrt(math.max(0, radius * radius - cross * cross))
            local direction = along < 0 and 1 or -1
            if math.abs(along) < 0.5 then direction = self.groundDirection or 1 end
            recovery.stage, recovery.since = "exit", now
            recovery.origin, recovery.heading = {data.position_m[1], data.position_m[2]}, data.heading_deg
            recovery.fx, recovery.fy, recovery.direction = fx, fy, direction
            recovery.exitDistance, recovery.bestProgress, recovery.progressTick = half + direction * along, 0, now
            recovery.radius, recovery.size = radius, size
        end
    end
    local direction, steering, speed = self.groundDirection or 1, 0, 0
    if recovery.origin then
        local dx, dy = data.position_m[1] - recovery.origin[1], data.position_m[2] - recovery.origin[2]
        local along = dx * recovery.fx + dy * recovery.fy
        local cross = dx * recovery.fy - dy * recovery.fx
        local progress = along * recovery.direction
        local remaining = recovery.exitDistance - progress
        recovery.progress, recovery.cross, recovery.remaining = progress, cross, remaining
        if recovery.stage == "exit" and remaining <= 0.35 and nav.marker_inside == false then
            recovery.stage, recovery.since = "return", now
            recovery.returnFrom, recovery.bestProgress, recovery.progressTick = progress, 0, now
        elseif recovery.stage == "return" and nav.marker_inside == true then
            recovery.stage, recovery.since = "ack", now
        end
        direction = (recovery.stage == "return" or recovery.stage == "ack") and -recovery.direction or recovery.direction
        steering = angle(recovery.heading - data.heading_deg - clamp(cross * direction * 3, -8, 8))
        if recovery.stage == "exit" then
            speed = math.min(10, math.sqrt(2 * 1.4 * math.max(0, remaining)) * 3.6)
        elseif recovery.stage == "return" then speed = 8 end
        if recovery.stage == "exit" or recovery.stage == "return" then
            local advance = recovery.stage == "exit" and progress or recovery.returnFrom - progress
            if advance >= recovery.bestProgress + 0.4 then recovery.bestProgress, recovery.progressTick = advance, now end
            if elapsed(now, recovery.progressTick) > 8000 then recovery.failure = "Нет прогресса повторного захода 8 с" end
            if elapsed(now, recovery.since) > 45000 then recovery.failure = "Истекло время повторного захода" end
            if math.abs(cross) > 3 or math.abs(angle(recovery.heading - data.heading_deg)) > 20 then
                recovery.failure = "Самолёт вышел из линии повторного захода"
            end
            if progress > recovery.exitDistance + 4 or progress < -4 then recovery.failure = "Превышен путь повторного захода" end
        end
    end
    if recovery.stage == "ack" and elapsed(now, recovery.since) > 2500 then
        recovery.failure = "Повторный вход в метку не подтверждён заданием"
    end
    if recovery.failure or type(nav.marker_inside) ~= "boolean" then speed = 0 end
    return {direction = direction, steering = steering, speed = speed, recovery = recovery}
end

function Controller:groundIntent(data, now)
    local nav = data.navigation
    local error = type(nav) == "table" and finite(nav.heading_error_deg) and nav.heading_error_deg or 0
    if self.finalLanding and self.flightSeen and data.on_ground == true and data.speed_kmh > 35 and self.landingHeading then
        local steering = angle(self.landingHeading - data.heading_deg)
        local rate = self.filteredRates and self.filteredRates.yaw or 0
        local rudder = clamp((clamp(steering * 0.8, -3, 3) - rate * 0.3) / 8.8, -0.3, 0.3)
        return {direction = 1, steering = steering, speed = 0, yaw = 8.8 * rudder,
            rudder = rudder, landing_rollout = true}
    end
    local reentry = self:taxiReentryIntent(data, now)
    local pending = not reentry and self.pushbackPending and elapsed(now, self.pushbackNoticeTick) < 250
    local align = not reentry and self.instruction == "taxi" and not pending
        and (self.pushback and math.abs(error) > 50 or self.pushbackPending and math.abs(error) > 60)
    local reverse = self.instruction == "reverse" or align or pending
    -- On the new taxi leg, align the nose while still rolling backwards.
    local steering = reverse and not align and angle(error - 180) or error
    local speed = pending and 0 or align and 6 or reverse and 8 or math.abs(steering) > 60 and 4
        or math.abs(steering) > 35 and 7 or math.abs(steering) > 15 and 12 or math.abs(steering) > 5 and 18 or 26
    if reentry then reverse, steering, speed = reentry.direction < 0, reentry.steering, reentry.speed end
    local entryLimit
    if not reentry and not reverse and math.abs(steering) <= 5 then
        speed = 35
        if type(nav) == "table" and nav.marker_type == "checkpoint" and finite(nav.marker_size_m)
            and nav.marker_size_m > 0 and finite(nav.distance_2d_m) then
            local remaining = math.max(0, nav.distance_2d_m - nav.marker_size_m - data.speed_kmh / 3.6 * 0.35)
            entryLimit = math.sqrt((18 / 3.6)^2 + 2 * 1.8 * remaining) * 3.6
            speed = math.min(speed, entryLimit)
        end
    end
    local runway = not self.airborne and not self.flightSeen and self.instruction == "takeoff"
        and self.takeoffPermit ~= nil and elapsed(now, self.takeoffPermit) < 60000
        and type(nav) == "table" and nav.marker_type == "ring"
    local rate = self.filteredRates and self.filteredRates.yaw or 0
    local yaw = pending and 0 or clamp(steering * 0.7, -8, 8)
    local runwayAlign, runwayRudder
    if runway then
        runwayAlign = not self.takeoffRolling and data.speed_kmh < 70 and (math.abs(error) > 2.5 or math.abs(rate) > 2)
        local wantedYaw = clamp(error * 0.8, -6, 6)
        local limit = runwayAlign and 0.8 or data.speed_kmh < 140 and 0.55 or 0.4
        runwayRudder = clamp((wantedYaw + (wantedYaw - rate) * 0.3) / 8.8, -limit, limit)
        yaw = 8.8 * runwayRudder
        if runwayAlign then speed = math.abs(error) > 20 and 8 or 14 end
    end
    local heldRudder, predicted
    if not runway then
        predicted = steering - rate * 0.45
        local previous = self.output.rudder or 0
        local held = previous == 0 and 0 or (previous > 0 and 1 or -1) * (reverse and -1 or 1)
        local direction = 0
        if not pending then
            if held ~= 0 then
                if steering * held > 0.65 and predicted * held > 0.45 then direction = held end
            elseif math.abs(predicted) > 2.25 and math.abs(steering) > 1.4 and predicted * steering > 0
                and (not self.groundSteerReleased or elapsed(now, self.groundSteerReleased) >= 180) then
                direction = steering > 0 and 1 or -1
            end
        end
        heldRudder = direction * (reverse and -1 or 1)
        yaw = direction * 8.8
    end
    return {direction = reverse and -1 or 1, steering = steering, speed = speed,
        align = align, pending = pending, yaw = yaw, runway = runway, runway_align = runwayAlign, rudder = runwayRudder,
        reentry = reentry and reentry.recovery,
        marker_entry_speed_limit = entryLimit, held_rudder = heldRudder, predicted_steering_error = predicted}
end

function Controller:update(data, now, pathClear, probeStatus)
    if not self.enabled then return self.output end
    local dt = elapsed(now, self.lastTick) / 1000
    self.lastTick = now
    if dt <= 0 then return self.output end
    local frameGap = dt
    local resync = dt > 0.3 and data and (data.window_minimized == true or data.window_restored == true)
    if dt > 0.3 and not resync then return self:stop("Пауза кадров больше 300 мс") end
    if not data or data.vehicle ~= self.vehicle or data.driver ~= true then return self:stop("Потеря самолёта / места пилота") end
    if tostring(data.dimension) .. ":" .. tostring(data.interior) ~= self.world then return self:stop("Смена мира") end
    for _, name in ipairs({"heading_deg", "pitch_deg", "roll_deg", "speed_kmh", "climb_mps"}) do
        if not finite(data[name]) then return self:stop("Нет данных: " .. name) end
    end
    if data.blown == true or data.in_water == true or (finite(data.health) and data.health < 300) then return self:stop("Повреждение самолёта / вода") end
    if type(data.on_ground) ~= "boolean" then return self:stop("Неизвестен контакт с землёй") end
    if resync then
        dt = 0.05
        self.filteredRates, self.output = {yaw = 0, pitch = 0, roll = 0}, neutral()
    end
    if vector(data.position_m) and vector(self.lastPosition) then
        local d = 0
        for i = 1, 3 do d = d + (data.position_m[i] - self.lastPosition[i])^2 end
        if math.sqrt(d) > math.max(30, data.speed_kmh / 3.6 * frameGap * 4) then return self:stop("Скачок позиции самолёта") end
    end
    self.lastPosition = data.position_m
    if data.on_ground then
        self.groundSince, self.airSince = self.groundSince or now, nil
        if elapsed(now, self.groundSince) >= 300 then self.airborne = false end
    else
        self.airSince, self.groundSince = self.airSince or now, nil
        if elapsed(now, self.airSince) >= 300 and ((finite(data.agl_terrain_m) and data.agl_terrain_m > 2) or data.speed_kmh > 140) then
            self.airborne, self.flightSeen = true, true
        end
    end
    if self.airborne then self.takeoffPermit = nil end
    local out = neutral()
    local nav = type(data.navigation) == "table" and data.navigation or nil
    local centeredGround = nav and data.on_ground == true and nav.marker_type == "checkpoint"
        and finite(nav.distance_2d_m) and nav.distance_2d_m <= 0.01
    if nav and (not finite(nav.distance_2d_m) or not finite(nav.distance_3d_m)
        or not centeredGround and (not finite(nav.heading_error_deg) or not finite(nav.bearing_deg))
        or not finite(nav.altitude_error_m) or not vector(nav.position)) then nav = nil end
    self.detail = {waypoint = self.waypoint, instruction = self.instruction, path_clear = pathClear, probe_status = probeStatus}
    self.detail.frame_gap_ms, self.detail.timing_resynced = frameGap * 1000, resync or false
    if not self.airborne and data.frozen == true then
        self.phase = self.instruction == "passengers" and "passengers" or self.instruction == "wait_clearance" and "wait_clearance" or "frozen_wait"
        self.status = self.instruction == "passengers" and "Ожидание пассажиров" or "Самолёт удерживается работой"
        self.detail.controls_suspended, self.output = true, out
        return out
    end
    local takeoffAllowed = self.instruction == "takeoff" and self.takeoffPermit ~= nil and elapsed(now, self.takeoffPermit) < 60000
    local noClearance = nav and nav.marker_type == "ring" and not self.flightSeen and not takeoffAllowed
    if not self.airborne and (self.instruction == "passengers" or self.instruction == "wait_clearance" or noClearance) then
        self.phase = noClearance and "wait_clearance" or self.instruction
        self.status = self.instruction == "passengers" and "Ожидание пассажиров" or "Ожидание разрешения на взлёт"
        stopMotion(out, data)
        out.gear_down = true
        self.output = out
        return out
    end
    if not nav then
        if self.parkingHold and not self.airborne then
            self.phase, self.status = "boarding_hold", "Стоянка: ожидание задания после входа в маркер"
            self.detail.parking_hold_reason = self.parkingHold
            stopMotion(out, data)
            out.gear_down, self.output = true, out
            return out
        end
        self.missingSince = self.missingSince or now
        if elapsed(now, self.missingSince) > 1000 then return self:stop("Нет текущего маркера") end
        self.phase, self.status = "waiting_marker", "Ожидание следующего маркера"
        if not self.airborne then stopMotion(out, data) end
        self.output = out
        return out
    end
    self.missingSince = nil
    if nav.ambiguous then return self:stop("Неоднозначный выбор маркера") end
    local changed = nav.id ~= self.navId or nav.marker_type ~= self.navType
    if not changed and self.navPosition then
        for i = 1, 3 do if math.abs(nav.position[i] - self.navPosition[i]) > 0.25 then changed = true end end
    end
    if changed then
        if self.navPosition then
            local heightStep = nav.position[3] - self.navPosition[3]
            if heightStep < -3 then self.descendingWaypoints = self.descendingWaypoints + 1
            elseif heightStep > 3 then self.descendingWaypoints = 0 end
        end
        self.waypoint = self.waypoint + 1
        self.navId, self.navType = nav.id, nav.marker_type
        self.navPosition = {nav.position[1], nav.position[2], nav.position[3]}
        self.closest, self.passedSince, self.ringPass = nil, nil, nil
        self.parkingHold = nil
    end
    self.detail.waypoint, self.detail.marker_changed = self.waypoint, changed
    local error = finite(nav.heading_error_deg) and nav.heading_error_deg or 0
    local yawRate = finite(data.heading_rate_dps) and data.heading_rate_dps or 0
    local pitchRate = finite(data.pitch_rate_dps) and data.pitch_rate_dps or 0
    local rollRate = finite(data.roll_rate_dps) and data.roll_rate_dps or 0
    local smoothing = dt / (0.10 + dt)
    local filtered = self.filteredRates
    filtered.yaw = filtered.yaw + smoothing * (yawRate - filtered.yaw)
    filtered.pitch = filtered.pitch + smoothing * (pitchRate - filtered.pitch)
    filtered.roll = filtered.roll + smoothing * (rollRate - filtered.roll)
    yawRate, pitchRate, rollRate = filtered.yaw, filtered.pitch, filtered.roll
    local agl = finite(data.agl_terrain_m) and data.agl_terrain_m or nil
    local ground = not self.airborne
    if self.finalLanding then ground = data.on_ground == true end
    local takeoff = ground and takeoffAllowed and not self.flightSeen and nav.marker_type == "ring"
    if ground and not takeoff then
        local intent = self:groundIntent(data, now)
        if intent.reentry then
            local recovery = intent.reentry
            self.detail.taxi_reentry_stage, self.detail.taxi_reentry_reason = recovery.stage, recovery.reason
            self.detail.taxi_reentry_stage_ms, self.detail.taxi_reentry_heading_deg = elapsed(now, recovery.since), recovery.heading
            self.detail.taxi_reentry_exit_distance_m, self.detail.taxi_reentry_progress_m = recovery.exitDistance, recovery.progress
            self.detail.taxi_reentry_remaining_m, self.detail.taxi_reentry_cross_track_m = recovery.remaining, recovery.cross
            self.detail.taxi_reentry_inside, self.detail.taxi_reentry_failure = nav.marker_inside, recovery.failure
            if recovery.failure then return self:stop(recovery.failure .. ": ручной перехват") end
        end
        if intent.landing_rollout then
            self.phase, self.status = "landing_rollout", "Касание: торможение по направлению полосы"
            out.brake, out.rudder, out.gear_down = 1, intent.rudder, true
            self.detail.landing_stage, self.detail.landing_heading_deg = "rollout", self.landingHeading
            self.detail.goal_speed_kmh, self.detail.steering_error_deg = 0, intent.steering
            self.detail.goal_yaw_dps = intent.yaw
            self.output = out
            return out
        end
        if self.finalLanding then self.finalLanding, self.landingHeading = false, nil end
        local reverse, steeringError = intent.direction < 0, intent.steering
        if intent.align and not self.pushback then
            self.pushbackTick, self.pushbackPosition = now, data.position_m
            self.pushbackLastPosition, self.pushbackTravel = data.position_m, 0
            self.pushbackStartError, self.pushbackBestError = math.abs(steeringError), math.abs(steeringError)
            self.pushbackActiveMs, self.pushbackNoProgressMs = 0, 0
        end
        self.pushback = intent.align
        if not intent.pending then self.pushbackPending = false end
        self.phase = intent.align and "pushback_align" or reverse and "reverse" or self.flightSeen and "rollout" or "taxi"
        local pushbackDistance = 0
        if intent.align and vector(self.pushbackPosition) and vector(data.position_m) then
            for i = 1, 2 do pushbackDistance = pushbackDistance + (data.position_m[i] - self.pushbackPosition[i])^2 end
            pushbackDistance = math.sqrt(pushbackDistance)
            if vector(self.pushbackLastPosition) then
                local dx, dy = data.position_m[1] - self.pushbackLastPosition[1], data.position_m[2] - self.pushbackLastPosition[2]
                self.pushbackTravel = self.pushbackTravel + math.sqrt(dx * dx + dy * dy)
            end
            self.pushbackLastPosition = data.position_m
            local monitoring = pathClear == true and not self.blocked and self.groundDirection == -1
            if monitoring then
                self.pushbackActiveMs = self.pushbackActiveMs + dt * 1000
                self.pushbackNoProgressMs = self.pushbackNoProgressMs + dt * 1000
            end
            -- Count useful improvement, not tiny oscillations or time spent waiting for a clear path.
            if math.abs(steeringError) <= self.pushbackBestError - 2 then
                self.pushbackBestError, self.pushbackNoProgressMs = math.abs(steeringError), 0
            end
            self.detail.pushback_align, self.detail.pushback_distance_m = true, pushbackDistance
            self.detail.pushback_travel_m = self.pushbackTravel
            self.detail.pushback_elapsed_ms = elapsed(now, self.pushbackTick)
            self.detail.pushback_active_ms, self.detail.pushback_no_progress_ms = self.pushbackActiveMs, self.pushbackNoProgressMs
            self.detail.pushback_start_error_deg, self.detail.pushback_best_error_deg = self.pushbackStartError, self.pushbackBestError
            self.detail.pushback_progress_deg = self.pushbackStartError - self.pushbackBestError
            self.detail.pushback_remaining_deg = math.max(0, math.abs(steeringError) - 50)
            self.detail.pushback_monitoring = monitoring
            if self.pushbackTravel > 18 or pushbackDistance > 18 then
                self.detail.pushback_stop_reason = "distance_limit"
                return self:stop("Превышен путь разворота задним ходом (18 м): ручной перехват")
            elseif self.pushbackNoProgressMs >= 6000 then
                self.detail.pushback_stop_reason = "no_progress"
                return self:stop("Нет прогресса разворота задним ходом 6 с: ручной перехват")
            end
        end
        local size = finite(nav.marker_size_m) and nav.marker_size_m > 0 and nav.marker_size_m or nil
        local c = nav.color_rgba or {}
        local yellow = finite(c[1]) and finite(c[2]) and finite(c[3]) and c[1] > 200 and c[2] > 160 and c[3] < 80
        local parking = yellow and nav.marker_type == "checkpoint" and not reverse
        local goalSpeed = intent.speed
        if parking then
            if self.parkingSize ~= size then self.parkingHold = nil end
            self.parkingSize = size
            if size then
                local inset = math.min(1.5, size * 0.05)
                local stopRadius = size - inset
                local remaining = math.max(0, nav.distance_3d_m - stopRadius)
                -- v*t + v^2/(2*a) <= remaining; include response time before braking.
                local deceleration, response = 2.0, 0.35
                local speedLimit = (math.sqrt((deceleration * response)^2 + 2 * deceleration * remaining)
                    - deceleration * response) * 3.6
                goalSpeed = math.min(goalSpeed, 16, speedLimit)
                self.detail.parking_stop_radius_m, self.detail.parking_remaining_m = stopRadius, remaining
                self.detail.parking_boundary_distance_m = nav.distance_3d_m - size
                self.detail.parking_speed_limit_kmh = math.min(16, speedLimit)
                if nav.marker_inside == true then self.parkingHold = self.parkingHold or "marker_entry"
                elseif remaining <= 0.15 then self.parkingHold = self.parkingHold or "inner_boundary" end
            else
                goalSpeed = 0
            end
            if self.parkingHold then goalSpeed = 0 end
            self.detail.parking_marker_size_m, self.detail.parking_inside = size, nav.marker_inside
            self.detail.parking_hold_reason = self.parkingHold
        else
            self.parkingHold = nil
        end
        if self.blocked then
            if pathClear == true then self.clearSince = self.clearSince or now else self.clearSince = nil end
            if self.clearSince and elapsed(now, self.clearSince) >= 1000 then self.blocked = false end
        elseif pathClear == false then self.blocked, self.clearSince = true, nil end
        local forward = data.velocity_body_rfu_mps and data.velocity_body_rfu_mps[2] or 0
        local switching = self.groundDirection ~= intent.direction or forward * intent.direction < -0.25
        if switching then
            if data.speed_kmh < 0.8 and math.abs(forward) < 0.18 then
                self.directionSettled = self.directionSettled or now
                if elapsed(now, self.directionSettled) >= 200 then
                    self.groundDirection, switching, self.directionSettled = intent.direction, false, nil
                end
            else self.directionSettled = nil end
        else self.directionSettled = nil end
        if self.blocked then
            goalSpeed = 0
            self.phase, self.status = "obstacle_hold", "Препятствие на траектории: торможение"
        elseif pathClear == nil then
            goalSpeed = 0
            self.phase, self.status = "probe_wait", "Ожидание проверки пути"
        elseif switching or intent.pending then
            goalSpeed = 0
            self.phase, self.status = "direction_change", "Остановка перед сменой направления"
        elseif intent.reentry then
            self.phase = "taxi_reentry_" .. intent.reentry.stage
            self.status = intent.reentry.stage == "exit" and "Повторный заход: выход за границу метки"
                or intent.reentry.stage == "return" and "Повторный заход: возврат в метку"
                or "Ожидание подтверждения наземной метки"
        elseif parking and (self.parkingHold or not size) then
            self.phase, self.status = "boarding_hold", size and "Стоянка у границы маркера: ожидание задания"
                or "Стоянка: нет достоверного размера маркера"
        elseif parking then
            self.phase, self.status = "boarding_approach", "Медленный подход к границе маркера посадки"
        else self.status = intent.align and "Задний ход: разворот носа к рулёжной дорожке"
            or reverse and "Медленный задний ход" or "Рулёжка по маркерам" end
        if goalSpeed < 0.5 then stopMotion(out, data)
        elseif reverse then
            local speedError = goalSpeed + forward * 3.6
            if speedError > 0.5 then out.brake = clamp(speedError * 0.10, 0, 0.35)
            elseif speedError < -1 then out.throttle = clamp(-speedError * 0.04, 0, 0.35) end
        else
            local speedError = goalSpeed - forward * 3.6
            if speedError > 0.5 then out.throttle = clamp(speedError * 0.06, 0, math.abs(steeringError) > 60 and 0.12 or 0.35)
            elseif speedError < -1 then out.brake = clamp(-speedError * 0.045, 0, 0.6) end
        end
        if pathClear == true and not self.blocked and goalSpeed > 0 and data.speed_kmh > 0.7 then
            out.rudder = intent.held_rudder or 0
        end
        if self.output.rudder ~= 0 and out.rudder == 0 then self.groundSteerReleased = now end
        out.gear_down = true
        self.detail.goal_speed_kmh, self.detail.steering_error_deg = goalSpeed, steeringError
        self.detail.motion_direction, self.detail.direction_change = intent.direction, switching
        self.detail.pushback_align, self.detail.pushback_distance_m = intent.align, pushbackDistance
        self.detail.goal_yaw_dps, self.detail.forward_speed_mps = intent.yaw, forward
        self.detail.marker_entry_speed_limit_kmh = intent.marker_entry_speed_limit
        self.detail.rudder_control, self.detail.predicted_steering_error_deg = "hold", intent.predicted_steering_error
    elseif takeoff then
        local intent = self:groundIntent(data, now)
        if pathClear ~= true then
            self.phase = pathClear == false and "obstacle_hold" or "probe_wait"
            self.status = pathClear == false and "Препятствие на траектории разбега" or "Проверка пути на полосе"
            stopMotion(out, data)
        elseif intent.runway_align then
            self.phase, self.status = "takeoff_align", "Выравнивание перед разбегом"
            local speedError = intent.speed - data.speed_kmh
            if speedError > 0.5 then out.throttle = clamp(speedError * 0.06, 0, 0.3)
            elseif speedError < -1 then out.brake = clamp(-speedError * 0.045, 0, 0.6) end
            if data.speed_kmh > 0.7 then out.rudder = intent.rudder end
            self.detail.goal_speed_kmh = intent.speed
        else
            self.takeoffRolling = true
            self.phase, self.status = "takeoff", "Разбег; разрешение получено"
            out.throttle = 1
            out.rudder = intent.rudder
            local pitchGoal = data.speed_kmh >= 150 and 6 or -1
            out.elevator = data.speed_kmh >= 145 and clamp((pitchGoal - data.pitch_deg) * 0.06 - pitchRate * 0.025, -0.25, 0.45) or 0
            out.aileron = clamp(-data.roll_deg * 0.04 - rollRate * 0.02, -0.3, 0.3)
            self.detail.goal_pitch_deg = pitchGoal
        end
        self.detail.runway_align, self.detail.runway_rolling = intent.runway_align, self.takeoffRolling
        self.detail.goal_yaw_dps, self.detail.steering_error_deg = intent.yaw, error
        out.gear_down = true
    else
        self.closest = math.min(self.closest or math.huge, nav.distance_3d_m)
        local descendingTarget = finite(nav.altitude_error_m) and nav.altitude_error_m < -2
        if self.flightSeen and self.descendingWaypoints >= 3 and agl and agl < 170 and descendingTarget
            and finite(data.surface and data.surface.ground_z_m)
            and nav.position[3] - data.surface.ground_z_m < 90 then self.landing = true end
        self.detail.descending_waypoints = self.descendingWaypoints
        local groundCheckpoint = nav.marker_type == "checkpoint" and finite(data.surface and data.surface.ground_z_m)
            and math.abs(nav.position[3] - data.surface.ground_z_m) < 15
        if self.landing and agl and not self.finalLanding and (agl < 7 or groundCheckpoint and agl < 15) then
            self.finalLanding = true
            self.landingHeading = math.abs(error) < 25 and nav.bearing_deg or data.heading_deg
        end
        self.phase, self.status = self.landing and "approach" or "flight", self.landing and "Заход на посадку" or "Полёт к центру кольца"
        local courseError = error
        if finite(nav.track_error_deg) and data.speed_kmh > 100 then courseError = angle(error + angle(nav.track_error_deg - error) * 0.7) end
        if self.finalLanding then
            courseError = angle(self.landingHeading - (finite(data.track_deg) and data.track_deg or data.heading_deg))
        end
        local horizontalSpeed = finite(data.horizontal_speed_kmh) and data.horizontal_speed_kmh / 3.6 or data.speed_kmh / 3.6
        local trim = data.speed_kmh > 220 and -1.4 or -0.7
        local captureMargin = finite(nav.marker_size_m) and math.max(0, nav.marker_size_m * 0.45) or 0
        local track = finite(data.track_deg) and data.track_deg
            or finite(nav.track_error_deg) and angle(nav.bearing_deg - nav.track_error_deg) or data.heading_deg
        local trackError = angle(nav.bearing_deg - track)
        local alongTrack = nav.distance_2d_m * math.cos(math.rad(trackError))
        local crossTrack = nav.distance_2d_m * math.sin(math.rad(trackError))
        local verticalMiss = nav.altitude_error_m - data.climb_mps * alongTrack / math.max(1, horizontalSpeed)
        local miss3d = math.sqrt(crossTrack^2 + verticalMiss^2)
        if not self.ringPass and nav.marker_type == "ring" and not self.finalLanding and vector(data.position_m)
            and captureMargin > 0 and data.speed_kmh > 100 and alongTrack > 0
            and nav.distance_2d_m < math.max(captureMargin * 2, horizontalSpeed * 0.75)
            and math.abs(trackError) < 10 and math.abs(data.roll_deg) < 12 and math.abs(yawRate) < 4
            and miss3d < captureMargin then
            -- Keep the incoming course through the capture corridor until the server moves the ring.
            self.ringPass = {heading = track, pitch = clamp(math.deg(math.atan2(nav.altitude_error_m,
                nav.distance_2d_m)) + trim, -23, 22), started = now}
        end
        local pass = self.ringPass
        if pass and (not vector(data.position_m) or self.finalLanding or captureMargin <= 0) then
            self.ringPass, pass = nil, nil
        end
        if pass then
            local heading = math.rad(pass.heading)
            local along = (nav.position[1] - data.position_m[1]) * math.sin(heading)
                + (nav.position[2] - data.position_m[2]) * math.cos(heading)
            if not pass.crossed and (miss3d > captureMargin * 1.5 or math.abs(angle(pass.heading - track)) > 20
                or elapsed(now, pass.started) > 2500) then
                self.ringPass, pass = nil, nil
            else
                if along <= 0 then pass.crossed = pass.crossed or now end
                self.detail.ring_along_m, self.detail.ring_exit_wait_ms = along, elapsed(now, pass.crossed)
                self.detail.ring_path_heading_deg, self.detail.ring_path_pitch_deg = pass.heading, pass.pitch
                if pass.crossed and elapsed(now, pass.crossed) > 1500 then
                    return self:stop("Нет следующего кольца после пролёта: ручной перехват")
                end
            end
        end
        self.detail.ring_flythrough, self.detail.predicted_miss_3d_m = pass ~= nil, miss3d
        if not pass and not (self.landing and nav.marker_type == "checkpoint")
            and nav.distance_3d_m > self.closest + 15 and math.abs(error) > 95 then
            self.passedSince = self.passedSince or now
            if elapsed(now, self.passedSince) > 350 then return self:stop("Маркер остался позади: ручной перехват") end
        else self.passedSince = nil end
        local interceptDistance = math.max(40, nav.distance_2d_m, horizontalSpeed * 1.2)
        local turnRate = 2 * horizontalSpeed / interceptDistance * math.sin(math.rad(clamp(courseError, -75, 75)))
        local wantedYaw = math.deg(turnRate)
        if changed or not self.turnDirection then self.turnDirection = courseError < 0 and -1 or 1 end
        local ringCapture = nav.marker_type == "ring" and captureMargin > 0 and math.abs(courseError) < 15
            and nav.distance_2d_m < horizontalSpeed * 1.2 and math.abs(crossTrack) < captureMargin
            and courseError * self.turnDirection < 0
        -- Inside the capture corridor, roll out instead of reversing bank to chase centimetres.
        if ringCapture then turnRate, wantedYaw = 0, 0 end
        if pass then
            local drift = angle(pass.heading - track)
            wantedYaw = clamp((drift < 0 and -1 or 1) * math.max(0, math.abs(drift) - 0.75) * 1.5, -3, 3)
            turnRate, ringCapture = math.rad(wantedYaw), true
            if pass.crossed then self.status = "Пролёт кольца: ожидание новой цели" end
        end
        local bankLimit = self.landing and 25 or agl and clamp(12 + math.max(0, agl - 5) * 0.8, 12, 50) or 25
        local requiredBank = math.deg(math.atan2(horizontalSpeed * turnRate, 9.81))
        local goalRoll = clamp(requiredBank, -bankLimit, bankLimit)
        local lookahead = 0.35
        local elevation = math.deg(math.atan2(nav.altitude_error_m - data.climb_mps * lookahead,
            math.max(20, nav.distance_2d_m - horizontalSpeed * math.cos(math.rad(courseError)) * lookahead)))
        local goalPitch = pass and pass.pitch or clamp(elevation + trim, -23, 22)
        local goalSpeed = self.landing and agl and clamp(125 + math.max(0, agl - 8) * 1.25, 125, 245)
            or clamp(255 - math.max(0, math.abs(goalRoll) - 15) * 1.2, 210, 255)
        local cruiseBlend = not self.landing and not self.finalLanding
            and clamp((15 - math.max(math.abs(goalRoll), math.abs(data.roll_deg))) / 10, 0, 1)
                * clamp((12 - math.abs(courseError)) / 6, 0, 1) or 0
        goalSpeed = goalSpeed + 15 * cruiseBlend
        if self.finalLanding and agl then
            local clearance = math.max(0, agl - self.contactAgl)
            local predictedClearance = math.max(0, clearance + data.climb_mps)
            local goalClimb = -clamp(0.9 + predictedClearance * 0.55, 0.9, 3)
            local flightPath = math.deg(math.atan2(data.climb_mps, math.max(20, horizontalSpeed)))
            local goalPath = math.deg(math.atan2(goalClimb, math.max(20, horizontalSpeed)))
            goalPitch = clamp(data.pitch_deg + (goalPath - flightPath) * 1.6, -7, 3)
            goalRoll, goalSpeed = clamp(goalRoll, -5, 5), 110
            self.status = "Снижение до касания по направлению полосы"
            self.detail.landing_stage, self.detail.landing_heading_deg = "touchdown", self.landingHeading
            self.detail.wheel_clearance_m, self.detail.goal_climb_mps = clearance, goalClimb
            self.detail.predicted_wheel_clearance_m = predictedClearance
            self.detail.flight_path_deg, self.detail.goal_flight_path_deg = flightPath, goalPath
        end
        local recovery = math.abs(data.roll_deg) > 65
        if recovery then
            goalRoll, goalPitch = 0, clamp(goalPitch, -3, 5)
            self.status = "Выравнивание большого крена"
        end
        if math.abs(data.roll_deg) > 75 or data.pitch_deg < -45 then return self:stop("Неустойчивое положение: ручной перехват") end
        local bank, pitch = math.rad(data.roll_deg), math.rad(data.pitch_deg)
        local pitchCommand = clamp((goalPitch - data.pitch_deg) * 1.4 - pitchRate * 0.6, -14, 14)
        local yawCommand = wantedYaw + clamp((wantedYaw - yawRate) * 0.4, -3, 3)
        local achievableYaw = math.deg(9.81 * math.tan(math.rad(bankLimit)) / math.max(25, horizontalSpeed))
        yawCommand = clamp(yawCommand, -achievableYaw, achievableYaw)
        if recovery then yawCommand = 0 end
        -- Rotate desired world pitch/heading rates into the banked aircraft axes.
        local turnPull = yawCommand * math.cos(pitch) * math.sin(bank)
        local pitchAxis = pitchCommand * math.cos(bank) + turnPull
        local yawAxis = yawCommand * math.cos(pitch) * math.cos(bank) - pitchCommand * math.sin(bank)
        local rollProportional, rollDamping = (goalRoll - data.roll_deg) * 0.040, -rollRate * 0.008
        out.aileron = clamp(rollProportional + rollDamping, -0.85, 0.85)
        out.elevator = clamp(pitchAxis / (pitchAxis >= 0 and 30.5 or 21), -0.7, 0.7)
        out.rudder = recovery and 0 or clamp(yawAxis / 12.4, -0.35, 0.35)
        local speedError = goalSpeed - data.speed_kmh
        if speedError > -2 then out.throttle = clamp(0.65 + speedError * 0.04, 0, 1)
        elseif speedError < -5 then out.brake = clamp(-speedError * 0.025, 0, self.landing and 0.8 or 0.35) end
        if self.finalLanding then out.throttle = 0 end
        if self.landing then out.gear_down = true
        elseif agl and agl > 8 and elapsed(now, self.airSince) > 1000 then out.gear_down = false end
        self.detail.goal_roll_deg, self.detail.goal_pitch_deg, self.detail.goal_speed_kmh = goalRoll, goalPitch, goalSpeed
        self.detail.cruise_speed_blend = cruiseBlend
        self.detail.guidance, self.detail.course_error_deg = pass and "ring_flythrough" or "curvature_intercept", courseError
        self.detail.goal_turn_rate_dps, self.detail.command_turn_rate_dps = wantedYaw, yawCommand
        self.detail.required_bank_deg, self.detail.bank_limit_deg = requiredBank, bankLimit
        self.detail.intercept_distance_m = interceptDistance
        self.detail.predicted_miss_m = nav.distance_2d_m * math.abs(math.sin(math.rad(courseError)))
        self.detail.turn_pull_dps, self.detail.pitch_axis_dps, self.detail.yaw_axis_dps = turnPull, pitchAxis, yawAxis
        self.detail.bank_recovery = recovery
        self.detail.ring_capture, self.detail.capture_margin_m = ringCapture, captureMargin
        self.detail.turn_direction = self.turnDirection
        self.detail.roll_error_deg, self.detail.roll_rate_filtered_dps = goalRoll - data.roll_deg, rollRate
        self.detail.roll_proportional, self.detail.roll_damping = rollProportional, rollDamping
    end
    for _, key in ipairs({"aileron", "elevator"}) do
        out[key] = clamp(out[key], (self.output[key] or 0) - dt * 3, (self.output[key] or 0) + dt * 3)
    end
    self.output = out
    return out
end

return Controller
end)()
-- END EMBEDDED PILOT CONTROLLER

-- Flight recorder and opt-in local-player controller. Navigation uses live elements.
local VERSION = "1.2.14"
local native = {log = dfPilotLog, update = dfPilotUpdate, command = dfPilotTakeCommand,
    alert = dfPlayAlertSignal, alertMonitor = dfSetAlertMonitorEnabled}
for _, name in ipairs({"log", "update", "command", "alert", "alertMonitor"}) do
    assert(type(native[name]) == "function", "PilotTelemetry: missing bridge " .. name)
end
if type(_G.__DarkFlamePilotCleanup) == "function" then _G.__DarkFlamePilotCleanup() end
local lease = native.update("attach", "")
assert(type(lease) == "string" and lease ~= "", "PilotTelemetry: incompatible native bridge")
local bridge = {
    log = function(text, force) return native.log(text, force, lease) end,
    update = function(key, value) return native.update(key, value, lease) end,
    command = function() return native.command(lease) end,
}

local NULL = {}
local autopilot = PilotController.new()
local state = {
    recording = false, interval = 50, phase = "manual", samples = 0, sequence = 0,
    buffer = {}, bufferBytes = 0, hooks = {}, failures = {}, lastSample = nil,
    lastUi = nil, lastFlush = nil, lastScan = nil, lastEnvironment = nil,
    candidates = {}, candidateByElement = {}, ids = setmetatable({}, {__mode = "k"}),
    nextId = 0, frame = 0, frames = 0, fps = 0, lastFps = nil,
    notificationCount = 0, notificationSeen = setmetatable({}, {__mode = "k"}),
    observerErrors = {}, observerErrorCount = 0,
    controlSince = {}, hudEnabled = false, autonomy = false,
    safetyAlerts = {}, safetyContacts = setmetatable({}, {__mode = "k"}),
    safetyNearby = setmetatable({}, {__mode = "k"}),
    status = "Готов к ручному полёту. Нажми «Начать запись».",
}
local pollNotifications, installNotificationObservers, cleanup
local stopAutopilot, applyAutopilot, updateAutopilot
local controls = {"accelerate", "brake_reverse", "vehicle_left", "vehicle_right",
    "steer_forward", "steer_back", "vehicle_look_left", "vehicle_look_right",
    "handbrake", "sub_mission"}
local analogControls = {"accelerate", "brake_reverse", "vehicle_left", "vehicle_right",
    "steer_forward", "steer_back", "vehicle_look_left", "vehicle_look_right"}
local flightKeys = {"w", "s", "a", "d", "q", "e", "arrow_u", "arrow_d", "arrow_l",
    "arrow_r", "2", "space", "num_8", "num_2", "num_4", "num_6"}
local keySet = {}
for _, key in ipairs(flightKeys) do keySet[key] = true end

local function elapsed(now, previous)
    return previous and (now - previous) % 4294967296 or math.huge
end

local function finite(value)
    return type(value) == "number" and value == value and math.abs(value) < math.huge
end

local function quote(value)
    return '"' .. value:gsub('[%z\1-\31\\"]', function(char)
        if char == '"' then return '\\"' end
        if char == '\\' then return '\\\\' end
        return string.format("\\u%04x", string.byte(char))
    end) .. '"'
end

local function json(value, depth)
    depth = depth or 0
    if value == NULL or value == nil then return "null" end
    if type(value) == "boolean" then return tostring(value) end
    if type(value) == "number" then return finite(value) and string.format("%.10g", value) or "null" end
    if type(value) == "string" then return quote(value) end
    if type(value) ~= "table" or depth > 10 then return quote(tostring(value)) end
    local output = {}
    if #value > 0 then
        for i = 1, #value do output[i] = json(value[i], depth + 1) end
        return "[" .. table.concat(output, ",") .. "]"
    end
    local keys = {}
    for key in pairs(value) do keys[#keys + 1] = key end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    for _, key in ipairs(keys) do
        output[#output + 1] = quote(tostring(key)) .. ":" .. json(value[key], depth + 1)
    end
    return "{" .. table.concat(output, ",") .. "}"
end

local function read(name, ...)
    local fn = _G[name]
    if type(fn) ~= "function" then state.failures[name] = "unavailable"; return nil end
    local ok, a, b, c, d, e, f = pcall(fn, ...)
    if not ok then
        state.failures[name] = tostring(a):sub(1, 160)
        return nil
    end
    state.failures[name] = nil
    return a, b, c, d, e, f
end

local function number(value)
    return finite(value) and value or nil
end

local function vector(name, ...)
    local x, y, z = read(name, ...)
    if finite(x) and finite(y) and finite(z) then return {x, y, z} end
    return nil
end

local function norm(v)
    return math.sqrt(v[1]^2 + v[2]^2 + v[3]^2)
end

local function dot(a, b)
    return a[1]*b[1] + a[2]*b[2] + a[3]*b[3]
end

local function sub(a, b)
    return {a[1]-b[1], a[2]-b[2], a[3]-b[3]}
end

local function scale(v, k)
    return {v[1]*k, v[2]*k, v[3]*k}
end

local function angle(value)
    return (value + 180) % 360 - 180
end

local function bearing(x, y)
    return math.deg(math.atan2(x, y)) % 360
end

local function elementId(element)
    if not element then return nil end
    if not state.ids[element] then
        state.nextId = state.nextId + 1
        state.ids[element] = "e" .. state.nextId
    end
    return state.ids[element]
end

local function valid(element)
    return element and read("isElement", element) == true
end

local function flush(force)
    local now = getTickCount()
    if not force and elapsed(now, state.lastFlush) < 250 and state.bufferBytes < 48000 then return true end
    local ok, message = bridge.log(table.concat(state.buffer), force or false)
    if not ok then
        state.recording = false
        state.recordingOwner = nil
        state.status = "Запись остановлена: ошибка файла. " .. tostring(message)
        bridge.update("recording", "0")
        bridge.update("status", state.status)
        return false
    end
    state.buffer, state.bufferBytes, state.lastFlush = {}, 0, now
    return true
end

local function emit(kind, payload, force)
    if not state.recording then return end
    state.sequence = state.sequence + 1
    local now = getTickCount()
    local line = json({type = kind, run = state.run, seq = state.sequence, tick_ms = now,
        elapsed_ms = elapsed(now, state.started), data = payload}) .. "\n"
    if #line > 60000 then
        line = json({type = "record_oversize", run = state.run, tick_ms = now,
            data = {original_type = kind, bytes = #line}}) .. "\n"
    end
    if state.bufferBytes + #line > 60000 and not flush(true) then return end
    state.buffer[#state.buffer + 1] = line
    state.bufferBytes = state.bufferBytes + #line
    flush(force)
end

local SERVER_ADMINS = {
    ["185.71.66.80:22003"] = [[
Emily_Quincy Dmitriy_Ogonkov Alim_Komarov Jack_Morozov Nestor_Rutherford Ivan_Prahodskiy
Vladimir_Smash Adrian_Litvintsev Aleksey_Elin Alex_Morrison Andrey_Valyaev Artem_Pankov
Artemiy_Lisiuk Egor_Sobakevich Eric_Morris Ethan_Santoro Kamilla_Florenz Melody_Wayne
Nick_Kotik Sergey_Gromenko Tyler_Quincy Wolfgang_Schneiderhan Stepan_Vorobyev Diego_Yezhov
Ivan_Lambert Kirill_Yezhov Kiyotaki_Darkness Milan_Mikasso Milka_Morris Yve_Furley
Ayato_Enfield Egor_Svarov Fedor_Salnikov]],
    ["185.71.66.70:22003"] = [[
Maria_Alekseeva Alexander_Krutov Aleksandr_Grozniy Aleksandr_Kartavtsev Alexey_Krutov Daniil_Lantratov
Crow_Green Avrora_Groznaya Dmitry_Pretty Don_Vice Jack_Turner Leo_King
Danila_Flaneks Roman_Lisov Vladislav_Townley Vyacheslav_Rublev Mihail_Tomato Mayson_McKenzy
Aleksandr_Biketov Illya_Santiz Daniil_Burdin Tony_King Mia_Krutova Rodion_Topolskiy
Aleksey_Korsakov Egor_Tkach Mauricio_Garcia Max_Gyf Vyacheslav_Lefortnikov Mark_Thompson
Igor_Samarskiy Andrei_King Vladislav_Kraskov Arseniy_Maltsev Alexander_Gasanov Antonio_Zubenko
Dmitriy_Ostrovskiy Franklin_White Maxim_Gornadzorov Pablo_King]],
    ["185.71.66.79:22003"] = [[
Arnold_Fenix Eric_Collins Danil_Astov Dmitriy_Scheglov Eduard_Vysotskiy Fedor_Khalifa
Prokhor_Lukoyanov Rodion_Vistnik Vladislav_Sutagin Alex_Trushin Alexandr_Yankee Alina_Solntsevskaya
Dmitriy_Dennica Ivan_Ryzov Kirill_Mirnyy Nika_Ryzova Ramiz_White Richard_Volsky
Sergei_Black Thomas_Freiman Vladislav_Macalister Dmitrii_Mihailov Dmitriy_Eagle Ilya_Sovietsky
Leonardo_Eliseev Andrew_Harin Artur_Eclipse Kirill_Shpilkov Markus_Haineken]],
    ["185.71.66.64:22003"] = [[
Claus_Nevskiy Anastasia_MacAlister Pavel_Morello Melissa_Witty Augustine_Morgan Evgeniy_Holmes
Artem_Fedukov Matthew_Esposito Mirella_Mayers Pavel_Belin Anna_Tverskaya Chad_Morgan
Kaito_Watanabe Saburo_Itto Anthony_Manrique Vasiliy_Tverskoy Varlam_Bobko Dmitriy_Prostorov
Robert_Dobrov Nick_Tverskoy Pablo_Moore Noah_Don Avram_Gagarin Oleg_Reall
Matthew_Dobrov Hiroyuki_Sanada Eric_Grand Egor_Derugin Anthony_Morgan Alexandr_Prospectiv
Darya_Roy Tihon_Medvedev]],
    ["185.71.66.66:22003"] = [[
Platon_Seven Kai_Mironov Alexandr_Silych Artem_Tyhkanov Daniil_Caffrey Dmitriy_Polanski
Alena_Loginova Alex_Gutmann Alexandr_Venevtsev Alexandr_Wigman Diana_Creighton Igor_Wallker
James_Moriarty Jesus_Lauren Konstantin_Dort Kristina_Kozlovskaya Mason_Montana Rostislav_Imenov
Ryan_Price Akim_Deville Alexander_McCartney Artem_Krasnovsky Robert_Sychev Konstantin_Quincy
Nikolay_Lesnoi Ylia_Rios Yuriy_Kalashnikov Andrey_James Ekaterina_MacCartney Kimi_Benzo]],
    ["185.71.66.81:22003"] = [[
Denis_Manafort Andrey_Novak Markus_Berg Elizaveta_Berg Georgiy_Zhilin Arthur_Daniels
Artem_Darmin Oliver_Capone Sergius_Vorobeyov Osiris_Reinhardt Yuriy_Topolskin Hugo_Wolf
Lee_Capone Monte_Good Vadim_Good Paul_Hegg Bavar_Bavarskiy Tyler_Hamilton
Artem_Watkowski Ruby_Bavarskiy Rudolf_Bavarskiy Han_Manarskiy Vladislav_Berg Ralph_Versace
Averardo_Manarskiy Andrey_Tambovskiy Mike_Fisher Sergey_Berg Astride_Capone Oscar_Nellson
Mino_Damone Gottschalk_Reinhardt Yaroslav_Laskov Ademar_Manarskiy Rem_Hiyama Ksenia_Atevon
Toti_Tykan Luka_Kakhovsky Maksim_Benz Marty_McCoy Neo_McCoy Saron_McCoy
Vasilii_Sokolov Averardo_Capone]],
    ["185.71.66.88:22003"] = [[
Anthony_Paris Artemiy_Kornyakov Igor_Navarro Pavel_Borushko Nikita_Kavalev Leonid_Bosow
Ivan_Homyakov Anton_Marshalov Denis_Milize Anatoliy_Mayskiy Nikolay_Bosow Felix_Kogut
Sergey_Sheremetev Aleksiy_Kotz Juster_Hillton Amina_Muver Mitrofan_Prostakov Kevin_Kasper
Victor_Ellington Evgeniy_Stepanov Otto_Vlasov Denis_Fadeev Potap_Pride August_Verstappen
Vladislav_Rotov Anton_Zalutcki Daniel_Harrington Kevin_Reichelderfer Mars_Holmes Maxim_Sharganov
Nikita_Muver Aquamarine_Vercetti Daniele_Homyakov Pavel_Homyakov]],
}
local knownAdmins = {}
for nick in tostring(SERVER_ADMINS[read("getServerIp", true)] or ""):gmatch("%S+") do
    knownAdmins[nick] = true
end

local adminPatterns = {
    "администратором%s+([A-Za-z]+_[A-Za-z0-9]+)",
    "администратор%s+([A-Za-z]+_[A-Za-z0-9]+)",
    "Администратор%s+([A-Za-z]+_[A-Za-z0-9]+)",
    "Admin%s+([A-Za-z]+_[A-Za-z0-9]+)",
    "заблокирован%s+([A-Za-z]+_[A-Za-z0-9]+)",
    "наказан%s+([A-Za-z]+_[A-Za-z0-9]+)",
    "предупреждён%s+([A-Za-z]+_[A-Za-z0-9]+)",
    "предупрежден%s+([A-Za-z]+_[A-Za-z0-9]+)",
}

local function safetyActive()
    return state.apSessionActive or autopilot.enabled or state.nextJob ~= nil
end

local function syncSafetyMonitor()
    local active = safetyActive()
    if state.safetyMonitorActive == active then return end
    state.safetyMonitorActive = active
    native.alertMonitor(active)
    if not active then state.safetyNearby = setmetatable({}, {__mode = "k"}) end
end

local function safetyAlert(key, cooldown, text, data)
    if not safetyActive() then return false end
    local now = getTickCount()
    if state.safetyAlerts[key] and elapsed(now, state.safetyAlerts[key]) < cooldown then return false end
    state.safetyAlerts[key] = now
    local played = native.alert() == true
    if text then outputChatBox("#FF5555[Pilot Safety] #FFFFFF" .. text, 255, 255, 255, true) end
    emit("safety_alert", {kind = key, text = text or NULL, played = played, context = data or NULL}, true)
    return played
end

local function cleanNick(value)
    return tostring(value or ""):gsub("#%x%x%x%x%x%x", "")
end

local function playerNick(player)
    return cleanNick(read("getPlayerNametagText", player) or read("getPlayerName", player) or "?")
end

local function sameWorld(player)
    return read("getElementDimension", player) == read("getElementDimension", localPlayer)
        and read("getElementInterior", player) == read("getElementInterior", localPlayer)
end

local function streamedPlayers()
    return read("getElementsByType", "player", root, true) or {}
end

local function nearbyPlayerById(id)
    for _, player in ipairs(streamedPlayers()) do
        if player ~= localPlayer and sameWorld(player)
            and tostring(read("getElementData", player, "id") or "") == tostring(id) then
            return player
        end
    end
end

local function isAdminPresenceMessage(text)
    for _, phrase in ipairs({"зашёл", "зашел", "подключился", "вышел", "покинул сервер",
        "отключился", "joined the server", "left the server", "connected", "disconnected"}) do
        if text:find(phrase, 1, true) then return true end
    end
    return false
end

local function safetyChat(text, r, g, b, messageType)
    if not safetyActive() or type(text) ~= "string" or messageType ~= 0 then return end
    local plain = text:gsub("#%x%x%x%x%x%x", "")
    local presence = isAdminPresenceMessage(plain)
    for _, pattern in ipairs(adminPatterns) do
        local nick = plain:match(pattern)
        if nick then knownAdmins[nick] = true; break end
    end
    if presence then return end

    local nick, id = plain:match("^([A-Za-z_][A-Za-z0-9_]*)%[([^%]]+)%]:")
    local nearby = id and nearbyPlayerById(id)
    local admin = nick and knownAdmins[nick] == true
    local adminSystem = r == 255 and g == 164 and b == 104
        and plain:find("Администратор", 1, true) ~= nil
    if not admin and not adminSystem and not nearby then return end

    local reason = (admin or adminSystem) and "сообщение администратора"
        or "сообщение игрока в зоне прорисовки"
    safetyAlert("chat", 5000, reason .. ": " .. plain,
        {admin = admin or adminSystem, sender = nick or NULL,
            nearby = nearby and playerNick(nearby) or NULL})
end

local function safetyCollision(hit)
    if not safetyActive() or not valid(hit) then return end
    local kind = read("getElementType", hit)
    if kind ~= "player" and kind ~= "ped" and kind ~= "vehicle" then return end
    local now = getTickCount()
    if state.safetyContacts[hit] and elapsed(now, state.safetyContacts[hit]) < 1500 then return end
    state.safetyContacts[hit] = now
    safetyAlert("collision", 30000, "ДТП: столкновение с " .. tostring(kind),
        {hit = elementId(hit), hit_kind = kind})
end

local function scanSafetyPlayers(now)
    if not safetyActive() then
        state.safetyNearby = setmetatable({}, {__mode = "k"})
        return
    end
    local current = setmetatable({}, {__mode = "k"})
    local nearest, nearestDistance
    local px, py, pz = read("getElementPosition", localPlayer)
    if not finite(px) or not finite(py) or not finite(pz) then return end
    for _, player in ipairs(streamedPlayers()) do
        if player ~= localPlayer and valid(player) and sameWorld(player) then
            local x, y, z = read("getElementPosition", player)
            if finite(x) and finite(y) and finite(z) then
                local distance = math.sqrt((x - px)^2 + (y - py)^2 + (z - pz)^2)
                if distance <= 200 then
                    current[player] = true
                    if not state.safetyNearby[player] and (not nearestDistance or distance < nearestDistance) then
                        nearest, nearestDistance = player, distance
                    end
                end
            end
        end
    end
    state.safetyNearby = current
    state.lastSafetyScan = now
    if nearest then
        safetyAlert("player_near", 30000,
            string.format("Человек рядом: %s (%.1f м)", playerNick(nearest), nearestDistance),
            {player = elementId(nearest), name = playerNick(nearest), distance_m = nearestDistance})
    end
end

local function inputSnapshot()
    local input = {digital = {}, analog = {}, raw_analog = {}, enabled = {}, keys = {}}
    input.chat = read("isChatBoxInputActive")
    input.console = read("isConsoleActive")
    input.cursor = read("isCursorShowing")
    input.menu = read("dfMenuOpen")
    input.window_active = read("isMTAWindowActive")
    for _, name in ipairs(controls) do
        local pressed = read("getControlState", name)
        input.digital[name] = pressed == nil and NULL or pressed
        local enabled = read("isControlEnabled", name)
        input.enabled[name] = enabled == nil and NULL or enabled
    end
    for _, name in ipairs(analogControls) do
        input.analog[name] = number(read("getAnalogControlState", name)) or NULL
        input.raw_analog[name] = number(read("getAnalogControlState", name, true)) or NULL
    end
    if not input.chat and not input.console then
        for _, key in ipairs(flightKeys) do input.keys[key] = read("getKeyState", key) end
    end
    return input
end

local function sameInput(a, b)
    if not b then return false end
    for key, value in pairs(a) do
        local previous = b[key]
        if type(value) == "table" then
            if type(previous) ~= "table" then return false end
            for name, stateValue in pairs(value) do if previous[name] ~= stateValue then return false end end
            for name in pairs(previous) do if value[name] == nil then return false end end
        elseif previous ~= value then return false end
    end
    for key in pairs(b) do if a[key] == nil then return false end end
    return true
end

local function ownership(element)
    local pilotRoot = state.pilotRoot
    local chain = {}
    for _ = 1, 12 do
        if not valid(element) or element == root then break end
        if element == pilotRoot then return "province_pilot", true, chain end
        local kind = read("getElementType", element)
        local id = read("getElementID", element)
        chain[#chain + 1] = {type = kind, id = id}
        if kind == "resource" then return id or "unknown", false, chain end
        element = read("getElementParent", element)
    end
    return "unknown", false, chain
end

local function describeCandidate(element, kind, position, dimension, interior)
    if read("getElementDimension", element) ~= dimension
        or read("getElementInterior", element) ~= interior then return nil end
    local p = vector("getElementPosition", element)
    if not p then return nil end
    local distance = norm(sub(p, position))
    if distance > 5000 then return nil end
    local owner, pilot, parents = ownership(element)
    local item = {id = elementId(element), kind = kind, position = p, distance_m = distance,
        owner = owner, pilot_ancestor = pilot, parents = parents, dimension = dimension,
        interior = interior, streamed = read("isElementStreamedIn", element), rank = 9}
    if kind == "marker" then
        item.marker_type = read("getMarkerType", element)
        item.size_m = read("getMarkerSize", element)
        local r, g, b, a = read("getMarkerColor", element)
        item.color = {r or NULL, g or NULL, b or NULL, a or NULL}
        item.alpha = read("getElementAlpha", element)
        if item.marker_type == "ring" or item.marker_type == "checkpoint" then
            item.next_position = vector("getMarkerTarget", element) or NULL
            if number(a) and a > 0 and number(item.alpha) and item.alpha > 0 then
                item.rank = pilot and 0 or item.marker_type == "ring" and 2 or 3
            end
        end
    else
        item.icon = read("getBlipIcon", element)
        item.size = read("getBlipSize", element)
        item.visible_distance = read("getBlipVisibleDistance", element)
        item.ordering = read("getBlipOrdering", element)
        local r, g, b, a = read("getBlipColor", element)
        item.color = {r or NULL, g or NULL, b or NULL, a or NULL}
        local attached = read("getElementAttachedTo", element)
        item.attached_to = attached and elementId(attached) or NULL
        if not attached and number(a) and a > 0 and number(r) and number(g) and number(b)
            and r > g * 1.4 and r > b * 1.4 then item.rank = pilot and 1 or 4 end
    end
    return item
end

local function scan(position, dimension, interior, now)
    local began = getTickCount()
    local candidates, lookup, elements = {}, {}, {}
    local inspected = 0
    if valid(state.pilotRoot) then
        for _, kind in ipairs({"marker", "blip"}) do
            for _, element in ipairs(read("getElementsByType", kind, state.pilotRoot) or {}) do
                inspected = inspected + 1
                local item = describeCandidate(element, kind, position, dimension, interior)
                if item then
                    candidates[#candidates + 1] = item
                    lookup[element], elements[item.id] = item, element
                end
            end
        end
    end
    table.sort(candidates, function(a, b)
        if a.rank ~= b.rank then return a.rank < b.rank end
        if a.distance_m ~= b.distance_m then return a.distance_m < b.distance_m end
        return a.id < b.id
    end)
    local total = #candidates
    while #candidates > 96 do
        local item = table.remove(candidates)
        lookup[elements[item.id]] = nil
        elements[item.id] = nil
    end
    local selected = candidates[1]
    if selected and selected.rank == 9 then selected = nil end
    local previous = state.target and lookup[state.target]
    if selected and previous and previous.rank == selected.rank then selected = previous end
    local target = selected and elements[selected.id] or nil
    local choices = 0
    if selected then
        for _, item in ipairs(candidates) do if item.rank == selected.rank then choices = choices + 1 end end
    end
    state.candidates, state.candidateByElement = candidates, lookup
    state.targetInfo = selected
    state.ambiguous = choices > 1 or (selected and not selected.pilot_ancestor) or false
    if target ~= state.target then
        emit("target_change", {old = state.target and elementId(state.target) or NULL,
            new = selected or NULL, ambiguous = state.ambiguous, equal_rank_candidates = choices}, true)
        state.target = target
        state.targetSince = now
    end
    state.lastScan = now
    emit("navigation", {candidates = candidates, total_in_range = total,
        omitted = total - #candidates, radius_m = 5000, ambiguous = state.ambiguous,
        scope = "province_pilot", inspected = inspected})
    state.lastScanCost = elapsed(getTickCount(), began)
    state.maxScanCost = math.max(state.maxScanCost or 0, state.lastScanCost)
    state.inspected = inspected
end

local function navigationSample(position, velocity, basis, now)
    if not valid(state.target) then return NULL end
    local targetPosition = vector("getElementPosition", state.target)
    if not targetPosition then return NULL end
    local delta = sub(targetPosition, position)
    local distance = norm(delta)
    local horizontal = math.sqrt(delta[1]^2 + delta[2]^2)
    local desired = horizontal > 0.01 and bearing(delta[1], delta[2]) or nil
    local closing = velocity and distance > 0.01 and dot(velocity, delta) / distance or nil
    local nav = {id = elementId(state.target), kind = state.targetInfo.kind,
        position = targetPosition, delta_world_m = delta, distance_3d_m = distance,
        distance_2d_m = horizontal, altitude_error_m = delta[3], bearing_deg = desired or NULL,
        elevation_deg = math.deg(math.atan2(delta[3], horizontal)), closing_mps = closing or NULL,
        eta_linear_s = closing and closing > 0.1 and distance / closing or NULL,
        target_age_ms = elapsed(now, state.targetSince), scan_age_ms = elapsed(now, state.lastScan),
        ambiguous = state.ambiguous, selection = "sticky_rank_then_nearest_observer"}
    local r, g, b, a = read(state.targetInfo.kind == "marker" and "getMarkerColor" or "getBlipColor", state.target)
    nav.color_rgba = {r or NULL, g or NULL, b or NULL, a or NULL}
    if state.targetInfo.kind == "marker" then
        nav.element_alpha = read("getElementAlpha", state.target)
        nav.marker_type = read("getMarkerType", state.target)
        nav.marker_size_m = read("getMarkerSize", state.target)
        local playerInside = read("isElementWithinMarker", localPlayer, state.target)
        local vehicleInside
        if valid(state.vehicle) then vehicleInside = read("isElementWithinMarker", state.vehicle, state.target) end
        nav.player_inside_marker = playerInside == nil and NULL or playerInside
        nav.vehicle_inside_marker = vehicleInside == nil and NULL or vehicleInside
        if playerInside == true or vehicleInside == true then nav.marker_inside = true
        elseif playerInside == false and vehicleInside == false then nav.marker_inside = false
        else nav.marker_inside = NULL end
    end
    local old = state.waypointSnapshot
    local changed = not old or old.id ~= nav.id or old.marker_type ~= nav.marker_type
        or norm(sub(old.position, targetPosition)) > 0.25
    if changed then
        state.waypointNumber = (state.waypointNumber or 0) + 1
        state.targetSince = now
        state.waypointSnapshot = {id = nav.id, marker_type = nav.marker_type, position = targetPosition}
        emit("waypoint_change", {generation = state.waypointNumber, previous = old or NULL,
            current = state.waypointSnapshot, color_rgba = nav.color_rgba}, true)
    end
    nav.waypoint_generation, nav.target_age_ms = state.waypointNumber, elapsed(now, state.targetSince)
    if basis then
        local body = {dot(delta, basis[1]), dot(delta, basis[2]), dot(delta, basis[3])}
        nav.delta_body_rfu_m = body
        nav.current_heading_deg = basis.heading
        nav.heading_error_deg = desired and angle(desired - basis.heading) or NULL
        local error = nav.heading_error_deg
        nav.turn_remaining_deg = error ~= NULL and math.abs(error) or NULL
        nav.turn_direction = error == NULL and "unknown" or math.abs(math.abs(error) - 180) < 0.000001 and "either"
            or error > 0 and "right" or error < 0 and "left" or "aligned"
        local bodyHorizontal = math.sqrt(body[1]^2 + body[2]^2)
        nav.body_yaw_to_center_deg = bodyHorizontal > 0.01 and math.deg(math.atan2(body[1], body[2])) or NULL
        nav.body_pitch_to_center_deg = distance > 0.01 and math.deg(math.atan2(body[3], bodyHorizontal)) or NULL
    end
    if velocity and math.sqrt(velocity[1]^2 + velocity[2]^2) > 0.1 then
        nav.track_deg = bearing(velocity[1], velocity[2])
        nav.track_error_deg = desired and angle(desired - nav.track_deg) or NULL
    end
    return nav
end

local function sample(now, frameMs, input)
    local vehicle = read("getPedOccupiedVehicle", localPlayer)
    if not valid(vehicle) then vehicle = nil end
    if state.vehicle ~= vehicle then
        emit("vehicle_change", {old = state.vehicle and elementId(state.vehicle) or NULL,
            new = vehicle and elementId(vehicle) or NULL}, true)
        state.vehicle, state.previous, state.lastScan = vehicle, nil, nil
        state.lastEnvironment, state.handlingJson, state.surface = nil, nil, nil
        state.contact, state.contactSince = nil, nil
        state.gearState, state.gearSince = nil, nil
        if state.target then emit("target_change", {old = elementId(state.target), new = NULL, reason = "vehicle_change"}, true) end
        state.target, state.targetInfo = nil, nil
        state.candidates, state.candidateByElement = {}, {}
    end
    local item = {frame = state.frame, frame_ms = frameMs, fps_observed = state.fps,
        requested_interval_ms = state.interval, sample_dt_ms = state.lastSample and elapsed(now, state.lastSample) or NULL,
        phase = state.phase, controls = input, vehicle = vehicle and elementId(vehicle) or NULL,
        window_minimized = state.minimized == true, window_restored = state.windowRestored == true,
        update_source = state.backgroundTick and "background_timer" or "pre_render"}
    item.control_held_ms = {}
    for _, name in ipairs(controls) do
        if input.digital[name] == true and state.controlSince[name] then
            item.control_held_ms[name] = elapsed(now, state.controlSince[name])
        end
    end
    if not vehicle then
        state.previous = nil
        return item
    end
    item.model = read("getElementModel", vehicle)
    item.vehicle_type = read("getVehicleType", vehicle)
    item.seat = read("getPedOccupiedVehicleSeat", localPlayer)
    item.driver = read("getVehicleController", vehicle) == localPlayer
    item.dimension = read("getElementDimension", vehicle)
    item.interior = read("getElementInterior", vehicle)
    item.position_m = vector("getElementPosition", vehicle) or NULL
    item.rotation_mta_deg = vector("getElementRotation", vehicle, "ZXY") or NULL
    local raw = vector("getElementVelocity", vehicle)
    local velocity = raw and scale(raw, 50)
    item.velocity_raw, item.velocity_world_mps = raw or NULL, velocity or NULL
    item.angular_velocity_raw = vector("getElementAngularVelocity", vehicle) or NULL
    item.health = read("getElementHealth", vehicle)
    item.engine = read("getVehicleEngineState", vehicle)
    item.landing_gear_down = read("getVehicleLandingGearDown", vehicle)
    if type(item.landing_gear_down) ~= "boolean" then item.landing_gear_down = NULL end
    item.landing_gear_state = item.landing_gear_down == true and "down"
        or item.landing_gear_down == false and "up" or "unknown"
    item.on_ground = read("isVehicleOnGround", vehicle)
    item.in_water = read("isElementInWater", vehicle)
    item.frozen = read("isElementFrozen", vehicle)
    item.collisions = read("getElementCollisionsEnabled", vehicle)
    item.blown = read("isVehicleBlown", vehicle)
    item.current_gear = read("getVehicleCurrentGear", vehicle)
    item.gravity_vector = vector("getVehicleGravity", vehicle) or NULL
    local matrix = read("getElementMatrix", vehicle, false)
    local basis
    if type(matrix) == "table" and type(matrix[1]) == "table" and type(matrix[2]) == "table"
        and type(matrix[3]) == "table" then
        local right = {matrix[1][1], matrix[1][2], matrix[1][3]}
        local forward = {matrix[2][1], matrix[2][2], matrix[2][3]}
        local up = {matrix[3][1], matrix[3][2], matrix[3][3]}
        local good = true
        for _, v in ipairs({right, forward, up}) do
            for i = 1, 3 do if not finite(v[i]) then good = false end end
        end
        if good then
            basis = {right, forward, up, heading = bearing(forward[1], forward[2])}
            item.basis_world_rfu = {right, forward, up}
            item.heading_deg = basis.heading
            item.pitch_deg = math.deg(math.atan2(forward[3], math.sqrt(forward[1]^2 + forward[2]^2)))
            item.roll_deg = math.deg(math.atan2(-right[3], up[3]))
            item.attitude_near_vertical = math.abs(item.pitch_deg) > 85
            if velocity then
                item.velocity_body_rfu_mps = {dot(velocity, right), dot(velocity, forward), dot(velocity, up)}
            end
        end
    end
    if velocity then
        item.speed_mps = norm(velocity)
        item.speed_kmh = item.speed_mps * 3.6
        item.horizontal_speed_kmh = math.sqrt(velocity[1]^2 + velocity[2]^2) * 3.6
        item.climb_mps = velocity[3]
        if item.horizontal_speed_kmh > 0.36 then
            item.track_deg = bearing(velocity[1], velocity[2])
            item.drift_deg = basis and angle(item.track_deg - basis.heading) or NULL
        end
        if item.velocity_body_rfu_mps and item.speed_mps > 0.1 then
            local body = item.velocity_body_rfu_mps
            item.velocity_body_elevation_deg = math.deg(math.atan2(body[3], body[2]))
            item.velocity_body_sideslip_deg = math.deg(math.atan2(body[1], math.sqrt(body[2]^2 + body[3]^2)))
        end
    end
    if item.landing_gear_state ~= state.gearState then
        emit("landing_gear_change", {previous = state.gearState or NULL, state = item.landing_gear_state,
            landing_gear_down = item.landing_gear_down, initial_observation = state.gearState == nil,
            previous_duration_ms = state.gearSince and elapsed(now, state.gearSince) or NULL,
            vehicle = item.vehicle, position_m = item.position_m, speed_kmh = item.speed_kmh,
            on_ground = item.on_ground, controls = input}, true)
        state.gearState, state.gearSince = item.landing_gear_state, now
    end
    item.landing_gear_state_age_ms = elapsed(now, state.gearSince)
    if type(item.on_ground) == "boolean" then
        if state.contact ~= item.on_ground then
            local old = state.contact
            emit("ground_contact_change", {previous = old == nil and NULL or old,
                on_ground = item.on_ground, previous_duration_ms = state.contactSince and elapsed(now, state.contactSince) or NULL,
                position_m = item.position_m, speed_kmh = item.speed_kmh, climb_mps = item.climb_mps,
                landing_gear_down = item.landing_gear_down, pitch_deg = item.pitch_deg, roll_deg = item.roll_deg,
                controls = input, phase = state.phase}, true)
            state.contact, state.contactSince = item.on_ground, now
        end
        item.ground_contact_duration_ms = elapsed(now, state.contactSince)
        item.motion_observation = item.on_ground and ((item.speed_kmh or 0) > 1 and "ground_moving" or "ground_still")
            or ((item.climb_mps or 0) > 0.5 and "airborne_climbing" or (item.climb_mps or 0) < -0.5 and "airborne_descending" or "airborne_level")
    else
        item.motion_observation = "ground_contact_unknown"
        state.contact, state.contactSince = nil, nil
    end
    local position = item.position_m ~= NULL and item.position_m or nil
    if position then
        if (state.recording or autopilot.enabled) and elapsed(now, state.lastScan) >= 250 then
            scan(position, item.dimension, item.interior, now)
        end
        item.navigation = navigationSample(position, velocity, basis, now)
        if not state.surface or elapsed(now, state.surface.tick_ms) >= 200 then
            state.surface = {tick_ms = now,
                ground_z_m = number(read("getGroundPosition", position[1], position[2], position[3] + 1)) or NULL,
                water_z_m = number(read("getWaterLevel", position[1], position[2], position[3])) or NULL}
        end
        item.surface = state.surface
        item.agl_terrain_m = state.surface.ground_z_m ~= NULL and position[3] - state.surface.ground_z_m or NULL
        local previous = state.previous
        local dt = previous and elapsed(now, previous.tick) / 1000 or nil
        local continuous = previous and dt > 0 and dt <= 0.5 and previous.dimension == item.dimension
            and previous.interior == item.interior and previous.position and velocity and previous.velocity
        if continuous then
            continuous = norm(sub(position, previous.position)) <= math.max(30, math.max(norm(velocity), norm(previous.velocity)) * dt * 4)
        end
        item.derivatives_valid = continuous and true or false
        if continuous then
            item.horizontal_speed_rate_mps2 = (math.sqrt(velocity[1]^2 + velocity[2]^2)
                - math.sqrt(previous.velocity[1]^2 + previous.velocity[2]^2)) / dt
            item.acceleration_world_mps2 = scale(sub(velocity, previous.velocity), 1 / dt)
            item.velocity_position_delta_mps = scale(sub(position, previous.position), 1 / dt)
            if basis then
                item.acceleration_body_rfu_mps2 = {dot(item.acceleration_world_mps2, basis[1]),
                    dot(item.acceleration_world_mps2, basis[2]), dot(item.acceleration_world_mps2, basis[3])}
            end
            if basis and previous.heading and not item.attitude_near_vertical and not previous.nearVertical then
                item.heading_rate_dps = angle(item.heading_deg - previous.heading) / dt
                item.pitch_rate_dps = angle(item.pitch_deg - previous.pitch) / dt
                item.roll_rate_dps = angle(item.roll_deg - previous.roll) / dt
            end
            local nav, oldNav = item.navigation, previous.navigation
            if nav and nav ~= NULL and oldNav and oldNav ~= NULL and nav.waypoint_generation == oldNav.waypoint_generation
                and finite(nav.heading_error_deg) and finite(oldNav.heading_error_deg)
                and not item.attitude_near_vertical and not previous.nearVertical then
                nav.heading_error_rate_dps = angle(nav.heading_error_deg - oldNav.heading_error_deg) / dt
                nav.turn_remaining_rate_dps = (math.abs(nav.heading_error_deg) - math.abs(oldNav.heading_error_deg)) / dt
            end
        elseif previous then
            emit("discontinuity", {dt_s = dt, old_position = previous.position, new_position = position})
        end
        state.previous = {tick = now, position = position, velocity = velocity, dimension = item.dimension,
            interior = item.interior, heading = item.heading_deg, pitch = item.pitch_deg,
            roll = item.roll_deg, nearVertical = item.attitude_near_vertical, navigation = item.navigation}
    else
        state.previous = nil
    end
    item.taxi = NULL
    if item.on_ground == true then
        local body = item.velocity_body_rfu_mps
        local speed = item.horizontal_speed_kmh and item.horizontal_speed_kmh / 3.6
        item.taxi = {forward_speed_mps = body and body[2] or NULL, sideways_speed_mps = body and body[1] or NULL,
            direction = not body and "unknown" or body[2] > 0.1 and "forward" or body[2] < -0.1 and "reverse"
                or speed and speed > 0.1 and "sideways" or "stationary",
            yaw_rate_dps = item.heading_rate_dps or NULL,
            speed_change_mps2 = item.horizontal_speed_rate_mps2 or NULL,
            forward_accel_mps2 = item.acceleration_body_rfu_mps2 and item.acceleration_body_rfu_mps2[2] or NULL,
            sideways_accel_mps2 = item.acceleration_body_rfu_mps2 and item.acceleration_body_rfu_mps2[1] or NULL,
            yaw_radius_estimate_m = speed and speed > 0.1 and finite(item.heading_rate_dps)
                and math.abs(item.heading_rate_dps) > 0.1 and speed / math.rad(math.abs(item.heading_rate_dps)) or NULL}
    end
    if state.recording and elapsed(now, state.lastEnvironment) >= 1000 then
        local handling = read("getVehicleHandling", vehicle)
        local encoded = json(handling)
        if encoded ~= state.handlingJson then
            emit("vehicle_handling", {vehicle = elementId(vehicle), model = item.model, handling = handling or NULL})
            state.handlingJson = encoded
        end
        emit("environment", {game_speed = read("getGameSpeed"), gravity_raw = read("getGravity"),
            wind_velocity_raw = vector("getWindVelocity") or NULL, weather = read("getWeather"),
            fps_limit = read("getFPSLimit"), unavailable_or_errors = state.failures})
        state.lastEnvironment = now
    end
    return item
end

local function format(value, suffix)
    return finite(value) and string.format("%.2f%s", value, suffix or "") or "n/a"
end

local function updateUi(item)
    syncSafetyMonitor()
    bridge.update("heartbeat", "1")
    bridge.update("recording", state.recording and "1" or "0")
    bridge.update("status", state.status)
    bridge.update("samples", tostring(state.samples))
    bridge.update("interval_ms", tostring(state.interval))
    bridge.update("notification", state.lastJobNotification or state.lastNotification or "Ожидание задания")
    bridge.update("notification_count", tostring(state.jobNotificationCount or 0))
    bridge.update("observer_errors", tostring(state.observerErrorCount))
    bridge.update("observer_error", state.lastObserverError or "")
    bridge.update("collector_error", state.collectorError or "")
    bridge.update("destination", state.destination or "ещё не назначен")
    bridge.update("resource_ready", state.pilotReady and "1" or "0")
    bridge.update("autopilot", autopilot.enabled and "1" or "0")
    bridge.update("autopilot_telemetry", autopilot.telemetry and "1" or "0")
    bridge.update("autopilot_hud", state.hudEnabled and "1" or "0")
    bridge.update("autopilot_autonomy", state.autonomy and "1" or "0")
    bridge.update("autopilot_waiting", state.nextJob and "1" or "0")
    bridge.update("autopilot_hud_error", state.hudError or "")
    bridge.update("autopilot_status", autopilot.status)
    bridge.update("autopilot_phase", autopilot.phase)
    local p = item and item.position_m
    local nav = item and item.navigation or {}
    local function dash(value, pattern) return finite(value) and string.format(pattern or "%.0f", value) or "--" end
    bridge.update("dashboard", table.concat({dash(item and item.speed_kmh), dash(item and item.heading_deg),
        dash(p and p[3]), dash(item and item.climb_mps, "%+.1f"), dash(item and item.agl_terrain_m),
        dash(item and item.pitch_deg, "%+.1f"), dash(item and item.roll_deg, "%+.1f"),
        dash(nav.heading_error_deg, "%+.1f"), dash(nav.distance_3d_m),
        item and item.landing_gear_state == "down" and "ВЫПУЩЕНЫ" or item and item.landing_gear_state == "up" and "УБРАНЫ" or "--",
        item and item.on_ground == true and "ЗЕМЛЯ" or item and item.on_ground == false and "ВОЗДУХ" or "--",
        type(nav.marker_type) == "string" and nav.marker_type or "--",
        dash(autopilot.enabled and autopilot.detail and autopilot.detail.goal_speed_kmh),
        dash(autopilot.enabled and autopilot.output.throttle * 100), dash(autopilot.enabled and autopilot.output.brake * 100),
        dash(nav.altitude_error_m, "%+.0f")}, "|"))
    if item and item.vehicle ~= NULL and p and p ~= NULL then
        bridge.update("flight", "Модель: " .. tostring(item.model) .. " / " .. tostring(item.vehicle_type)
            .. "\nСкорость: " .. format(item.speed_kmh, " км/ч") .. " | Vz: " .. format(item.climb_mps, " м/с")
            .. "\nXYZ: " .. format(p[1]) .. ", " .. format(p[2]) .. ", " .. format(p[3])
            .. "\nAGL: " .. format(item.agl_terrain_m, " м") .. " | курс: " .. format(item.heading_deg, "°")
            .. "\nТангаж: " .. format(item.pitch_deg, "°") .. " | крен: " .. format(item.roll_deg, "°")
            .. "\nШасси: " .. (item.landing_gear_state == "down" and "выпущены"
                or item.landing_gear_state == "up" and "убраны" or "неизвестно") .. " | земля: " .. tostring(item.on_ground)
            .. "\nПоворот: " .. format(item.heading_rate_dps, " °/с")
            .. " | Δскорости: " .. format(item.horizontal_speed_rate_mps2, " м/с²"))
    else
        bridge.update("flight", "Ожидание самолёта. Запись может быть включена заранее.")
    end
    local nav = item and item.navigation
    if nav and nav ~= NULL then
        bridge.update("target", tostring(nav.id) .. " / " .. tostring(nav.kind)
            .. (nav.ambiguous and " / НЕОДНОЗНАЧНО" or " / ресурс пилота")
            .. "\nДальность: " .. format(nav.distance_3d_m, " м") .. " | ΔZ: " .. format(nav.altitude_error_m, " м")
            .. "\nКурс: " .. format(nav.current_heading_deg, "°") .. " → маркер: " .. format(nav.bearing_deg, "°")
            .. "\nДоворот: " .. format(nav.heading_error_deg, "°") .. " (+ вправо / − влево)"
            .. "\nСближение: " .. format(nav.closing_mps, " м/с")
            .. "\nВозраст: " .. format(nav.target_age_ms / 1000, " с")
            .. "\nRGBA: " .. table.concat({tostring(nav.color_rgba[1]), tostring(nav.color_rgba[2]), tostring(nav.color_rgba[3]), tostring(nav.color_rgba[4])}, ", ")
            .. "\nКандидатов: " .. #state.candidates)
    else bridge.update("target", "Ориентир ещё не определён. Координаты считываются в игре.") end
    local pressed = {}
    if item and item.controls then
        for _, name in ipairs(controls) do
            if item.controls.digital[name] == true then pressed[#pressed + 1] = name end
        end
    end
    local errors = 0
    for _ in pairs(state.failures) do errors = errors + 1 end
    bridge.update("controls", "Фаза: " .. state.phase .. "\nНажато: " .. (#pressed > 0 and table.concat(pressed, ", ") or "—")
        .. "\nFPS: " .. format(state.fps) .. " | кадр: " .. format(item and item.frame_ms, " мс")
        .. "\nИнтервал: " .. state.interval .. " мс | API n/a: " .. errors
        .. "\nСборщик max: " .. format(state.maxFrameCost, " мс") .. " | скан: " .. tostring(state.inspected or 0)
        .. "\n" .. (autopilot.enabled and "Автопилот: " .. autopilot.phase .. " | W/S/A/D/Q/E — перехват" or "Ручное управление"))
end

local function flushCollisionBurst()
    if not state.collisionSuppressed or state.collisionSuppressed == 0 then return end
    emit("collision_burst", {count = state.collisionSuppressed, peak_force_raw = state.collisionPeak,
        last_contact = state.collisionLast, peak_contact = state.collisionStrongest,
        since_tick_ms = state.lastCollisionLog, until_tick_ms = getTickCount(),
        detail_limit_ms = 100, reason = "bounded_collision_callback_cost"}, true)
    state.collisionSuppressed, state.collisionPeak, state.collisionLast, state.collisionStrongest = 0, 0, nil, nil
end

local function stop(reason)
    if state.recording then
        flushCollisionBurst()
        emit("recording_stop", {reason = reason, samples = state.samples, owner = state.recordingOwner}, true)
        if not state.recording then return end
        state.recording = false
        state.status = "Запись остановлена. Сохрани PilotTelemetry.log до следующего запуска игры."
    end
    state.recordingOwner = nil
    state.previous, state.lastInput = nil, nil
    bridge.update("recording", "0")
end

local function start(owner)
    if state.recording then return end
    if not state.pilotReady then
        state.status = "Ожидание запуска province_pilot."
        return
    end
    if not flush(true) then return end
    state.started = getTickCount()
    local wall = read("getRealTime")
    state.run = tostring(wall and wall.timestamp or 0) .. "_" .. tostring(state.started)
    state.samples, state.sequence, state.phase = 0, 0, "manual"
    state.notificationCount, state.lastNotification = 0, nil
    state.observerErrors, state.observerErrorCount, state.lastObserverError = {}, 0, nil
    state.collectorError = nil
    state.notificationSeen = setmetatable({}, {__mode = "k"})
    state.notificationGroups = nil
    state.previous, state.lastSample, state.lastScan, state.lastEnvironment = nil, nil, nil, nil
    state.lastLogged, state.waypointSnapshot, state.waypointNumber = nil, nil, 0
    state.lastCollisionLog, state.collisionSuppressed, state.collisionPeak = nil, 0, 0
    state.target, state.targetInfo, state.lastInput = nil, nil, nil
    state.candidates, state.candidateByElement = {}, {}
    state.handlingJson, state.surface = nil, nil
    state.contact, state.contactSince = nil, nil
    state.gearState, state.gearSince, state.controlSince = nil, nil, {}
    state.perf = {frames = 0, total_ms = 0, max_ms = 0, frame_max_ms = 0, began = state.started}
    state.hudFrames, state.hudTotalCost, state.hudMaxCost = 0, 0, 0
    state.maxFrameCost, state.maxScanCost, state.lastScanCost = 0, 0, 0
    state.lastNotificationPoll = nil
    state.loggedProbeTick = nil
    state.recording = true
    state.recordingOwner = owner or "manual"
    state.status = (autopilot.enabled or owner == "autopilot") and "Идёт запись полёта с автопилотом."
        or "Идёт запись ручного полёта. Управление у тебя."
    installNotificationObservers()
    local bindings = {}
    for _, name in ipairs(controls) do bindings[name] = read("getBoundKeys", name) or NULL end
    emit("recording_start", {version = VERSION, schema = 2, owner = state.recordingOwner,
        mode = (autopilot.enabled or owner == "autopilot") and "autopilot" or "manual_observer",
        controller_version = PilotController.version, autopilot_telemetry = autopilot.telemetry,
        wall_time = wall or NULL, mta_version = read("getVersion"), requested_interval_ms = state.interval,
        controls = controls, bindings = bindings, registered_hooks = state.hookStatus,
        units = {position = "MTA world units (metres)", velocity_raw_to_mps = 50,
            heading = "0 north +Y; 90 east +X; clockwise", pitch = "nose up positive",
            roll = "right wing down positive; atan2(-right.z, up.z)", body_axes = "right, forward, up",
            heading_error = "marker bearing minus nose heading in [-180,180); positive right, negative left; 180 either way",
            body_marker_angles = "yaw positive right; pitch positive above aircraft; null when direction is undefined",
            taxi_speed_change = "horizontal speed magnitude derivative; negative means slowing, also when reversing",
            taxi_yaw_radius = "speed / abs(nose yaw rate); estimate only, not obstacle or wing clearance",
            control_held_ms = "time since pressed was first observed this recording; not inferred before recording",
            landing_gear = "observed API boolean only; no extension animation progress inferred",
            angular_velocity_raw = "engine native; no assumed conversion",
            acceleration = "dv/dt; no gravity compensation", agl = "terrain query; may be unavailable outside streamed world"},
        navigation = {coordinates = "live elements", selected_target_is_heuristic = true,
            scope = "province_pilot", scan_ms = 250, radius_m = 5000, max_candidates = 96},
        unavailable_or_errors = state.failures}, true)
    emit("job_context", {destination = state.destination or NULL, notification = state.lastJobNotification or NULL,
        observed_tick_ms = state.jobNotificationTick or NULL, instruction = autopilot.instruction,
        notification_count = state.jobNotificationCount or 0, cached_before_recording = true}, true)
    pollNotifications()
    bridge.update("recording", state.recording and "1" or "0")
end

local function acceptJob()
    local pilot = read("getResourceFromName", "province_pilot")
    if not pilot or read("getResourceState", pilot) ~= "running" then
        state.status = "Трудоустройство недоступно: province_pilot не запущен."
        return
    end
    local pilotRoot = read("getResourceRootElement", pilot)
    if not pilotRoot then
        state.status = "Трудоустройство не отправлено: корень province_pilot не найден."
        return
    end
    if read("triggerServerEvent", "pilot:onJobAccepted", pilotRoot) then
        state.status = "Запрос на трудоустройство отправлен."
        return true
    else
        state.status = "Не удалось отправить запрос на трудоустройство."
    end
end

local ownedAnalog = {"accelerate", "brake_reverse", "vehicle_left", "vehicle_right", "steer_forward", "steer_back"}
local ownedDigital = {"vehicle_look_left", "vehicle_look_right", "handbrake", "sub_mission"}

local function autopilotSnapshot(compactProbe)
    return {enabled = autopilot.enabled, phase = autopilot.phase, instruction = autopilot.instruction,
        status = autopilot.status, controller_version = PilotController.version, telemetry = autopilot.telemetry,
        recording_owner = state.recordingOwner or NULL,
        hud_enabled = state.hudEnabled, hud_error = state.hudError or NULL,
        autonomy = state.autonomy, next_job = state.nextJob or NULL,
        requested = autopilot.output, decision = autopilot.detail or NULL, applied = state.applied or NULL,
        obstacle_probe = (compactProbe and state.probeSummary or state.probe) or NULL, gear_attempts = state.gearAttempts or 0}
end

local function playAutopilotSound(enabled)
    local name = enabled and "AutoPilotON.mp3" or "AirbusOff.mp3"
    local ok = bridge.update("play_sound", enabled and "on" or "off") == true
    if not ok then bridge.update("autopilot_sound_error", "Не удалось запустить звук: " .. name) end
    emit("autopilot_sound", {name = name, enabled = enabled, queued = ok, looped = false}, false)
end

local function releaseRmb(reason)
    if not state.rmbDownTick then return true end
    state.rmbReleaseTick = getTickCount()
    local ok = read("dfEmulateKey", "rmb", false) == true
    emit("autopilot_rmb", {pressed = false, ok = ok, reason = reason,
        held_ms = elapsed(getTickCount(), state.rmbDownTick)})
    if ok then state.rmbDownTick = nil end
    return ok
end

local function updateRmb(now)
    if not autopilot.enabled and not state.rmbDownTick then return end
    local busy = read("dfMenuOpen") == true or read("isCursorShowing") == true
        or read("isChatBoxInputActive") == true or read("isConsoleActive") == true
    if state.rmbDownTick then
        if (not autopilot.enabled or busy or elapsed(now, state.rmbDownTick) >= 80)
            and (not state.rmbReleaseTick or elapsed(now, state.rmbReleaseTick) >= 250) then releaseRmb("pulse_end") end
        return
    end
    if not autopilot.enabled or busy or elapsed(now, state.rmbLastTick) < 2000 or read("getKeyState", "mouse2") == true then return end
    state.rmbLastTick, state.rmbDownTick = now, now
    state.rmbReleaseTick = nil
    local ok = read("dfEmulateKey", "rmb", true) == true
    emit("autopilot_rmb", {pressed = true, ok = ok, interval_ms = 2000})
    if not ok then releaseRmb("press_failed") end
end

local function releaseAutopilotControls()
    local failed = {}
    if state.controlsOwned then
        for _, name in ipairs(ownedAnalog) do
            if read("setAnalogControlState", name) ~= true then failed[#failed + 1] = name end
        end
        for _, name in ipairs(ownedDigital) do
            if read("setPedControlState", localPlayer, name, false) ~= true then failed[#failed + 1] = name end
        end
    end
    if #failed == 0 then state.controlsOwned = false end
    state.gearPulse = nil
    return failed
end

stopAutopilot = function(reason)
    local wasEnabled = state.apSessionActive or autopilot.enabled or state.controlsOwned
    state.completionGrace = wasEnabled and state.autonomy and reason == "Потеря самолёта / места пилота"
        and getTickCount() or nil
    if state.nextJob then emit("autonomy_cancel", {reason = reason, stage = state.nextJob.stage}, true) end
    state.nextJob = nil
    local previous = wasEnabled and autopilotSnapshot() or nil
    autopilot:stop(reason)
    local failed = releaseAutopilotControls()
    if not releaseRmb("autopilot_stopped") then failed[#failed + 1] = "rmb" end
    state.controlsOwned, state.applied, state.gearPulse = false, nil, nil
    state.controlsSuspended = false
    state.apSessionActive = false
    state.gearAttempts, state.gearDesired, state.gearAttemptTick = 0, nil, nil
    if #failed > 0 then autopilot.status = reason .. "; не удалось отпустить: " .. table.concat(failed, ", ") end
    if wasEnabled then
        emit("autopilot_stop", {reason = reason, previous = previous, release_failures = failed}, true)
        playAutopilotSound(false)
    end
    if wasEnabled and state.recording then
        if state.recordingOwner == "autopilot" then stop("autopilot_stopped")
        else state.status = "Ручная запись продолжается. Управление у тебя." end
    end
    bridge.update("autopilot", "0")
    bridge.update("autopilot_waiting", "0")
    bridge.update("autopilot_status", autopilot.status)
    bridge.update("autopilot_phase", autopilot.phase)
    bridge.update("status", state.status)
end

local function groundProbe(item, now)
    if item.frozen == true and item.on_ground == true then return nil end
    local intent = autopilot:groundIntent(item, now)
    local forward = item.velocity_body_rfu_mps and number(item.velocity_body_rfu_mps[2]) or 0
    local motion = forward < -0.25 and -1 or forward > 0.25 and 1 or intent.direction
    local nav = item.navigation or {}
    local key = table.concat({intent.direction, motion, tostring(intent.runway), tostring(intent.runway_align),
        tostring(nav.id), tostring(nav.waypoint_generation), tostring(item.dimension), tostring(item.interior)}, ":")
    if item.on_ground ~= true then
        if not autopilot.airborne and elapsed(now, state.lastProbe) < 600 then return state.pathClear end
        return nil
    end
    local began = getTickCount()
    local basis, position = item.basis_world_rfu, item.position_m
    if not basis or not position or position == NULL then state.pathClear = nil; return nil end
    local speed = item.speed_kmh / 3.6
    local function compatible(work)
        if not work or work.vehicle ~= state.vehicle or work.key ~= key or elapsed(now, work.started) > 250 then return false end
        local dx, dy = position[1] - work.position[1], position[2] - work.position[2]
        return dx*dx + dy*dy <= math.max(1.5, speed * 0.3)^2
            and math.abs(intent.yaw - work.intentYaw) <= 0.5
            and math.abs(position[3] - work.position[3]) < 0.6
            and math.abs(angle(item.heading_deg - work.heading)) < 8 and math.abs(speed - work.speed) < 2
            and math.abs(angle(item.pitch_deg - work.pitch)) < 3 and math.abs(angle(item.roll_deg - work.roll)) < 3
    end
    local work = state.probeWork
    if compatible(work) then
        if work.done and elapsed(now, work.started) < 150 then return state.pathClear end
        if work.done then work = nil end
    else work = nil end
    if not work then
        if state.boundsVehicle ~= state.vehicle then
            local x1, y1, z1, x2, y2, z2 = read("getElementBoundingBox", state.vehicle)
            state.bounds = finite(x1) and finite(y1) and finite(z1) and finite(x2) and finite(y2) and finite(z2)
                and x1 < x2 and y1 < y2 and z1 < z2 and {x1, y1, z1, x2, y2, z2} or nil
            state.boundsVehicle = state.bounds and state.vehicle or nil
        end
        local box = state.bounds
        if not box then
            state.pathClear, state.probe = nil, {tick_ms = now, status = "unavailable", reason = "bounding_box_unavailable"}
            state.probeSummary = state.probe
            return nil
        end
        local direction = intent.direction
        local horizon = math.min(100, math.max(5, speed * speed / 6 + speed * 0.4 + 3))
        local reaction = math.min(horizon, speed * 0.35 + 0.1)
        if motion ~= direction then reaction = math.min(horizon, reaction + speed * speed / 6) end
        local travel = math.max(2, horizon - reaction)
        local yaw = math.rad(math.max(-15, math.min(15, intent.yaw * travel / math.max(2, speed))))
        local remainingTurn = math.rad(math.abs(intent.steering))
        yaw = math.max(-remainingTurn, math.min(remainingTurn, yaw))
        local heading = math.rad(item.heading_deg)
        local right, ahead = {math.cos(heading), -math.sin(heading)}, {math.sin(heading), math.cos(heading)}
        local contactHeight = math.max(1.2, box[3] * 0.65)
        local wingHeight = math.max(contactHeight, box[3])
        local upperHeight = math.max(3, box[3] + (box[6] - box[3]) * 0.35)
        local offsets = {}
        for _, z in ipairs({0.6, contactHeight, upperHeight}) do
            offsets[#offsets + 1] = {0, box[5] + 0.25, z, "nose"}
            offsets[#offsets + 1] = {0, box[2] - 0.25, z, "tail"}
        end
        for _, z in ipairs({wingHeight, upperHeight}) do
            offsets[#offsets + 1] = {box[1] - 0.25, 0, z, "left_wing"}
            offsets[#offsets + 1] = {box[4] + 0.25, 0, z, "right_wing"}
        end
        offsets[#offsets + 1] = {0, box[2], math.max(3, box[6] - 0.5), "tail_top"}
        local function body(x, y, z)
            return {basis[1][1]*x + basis[2][1]*y + basis[3][1]*z,
                basis[1][2]*x + basis[2][2]*y + basis[3][2]*z,
                basis[1][3]*x + basis[2][3]*y + basis[3][3]*z}
        end
        local function point(offset, dx, dy, turn)
            local c, s = math.cos(turn), math.sin(turn)
            -- Body attitude locates the part; travel and heading rotation stay in the ground plane.
            return {position[1] + right[1]*dx + ahead[1]*dy + c*offset[1] + s*offset[2],
                position[2] + right[2]*dx + ahead[2]*dy - s*offset[1] + c*offset[2], position[3] + offset[3]}
        end
        work = {started = now, vehicle = state.vehicle, key = key, position = position, heading = item.heading_deg,
            speed = speed, pitch = item.pitch_deg, roll = item.roll_deg, intentYaw = intent.yaw,
            next = 1, rays = {}, checked = {}, clear = true, cost = 0, passes = 0,
            horizon = horizon, reaction = reaction, yaw = math.deg(yaw), direction = direction, motion = motion,
            bounds = box, heights = {body_low = 0.6, body_contact = contactHeight, wing = wingHeight, upper = upperHeight}}
        local function add(a, b, part, kind)
            work.rays[#work.rays + 1] = {from = a, to = b, part = part, kind = kind}
        end
        for _, offset in ipairs(offsets) do
            local x, y, z, part = unpack(offset)
            local relative = body(x, y, z)
            local a = point(body(x * 0.98, y * 0.98, z), 0, 0, 0)
            local b = point(relative, 0, reaction * motion, 0)
            add(a, b, part, "reaction")
            a = b
            local steps = math.abs(yaw) > 0.01 and 2 or 1
            for step = 1, steps do
                local fraction = step / steps
                local turn, distance = yaw * fraction, travel * fraction * direction
                local dx = math.abs(turn) > 0.0001 and distance * (1 - math.cos(turn)) / turn or 0
                local dy = math.abs(turn) > 0.0001 and distance * math.sin(turn) / turn or distance
                b = point(relative, dx, reaction * motion + dy, turn)
                add(a, b, part, steps == 2 and "turn_sweep" or "planned")
                a = b
            end
        end
        state.probeWork = work
    end
    work.passes = work.passes + 1
    while work.next <= #work.rays and elapsed(getTickCount(), began) < 6 do
        local ray = work.rays[work.next]
        local a, b = ray.from, ray.to
        local result = read("isLineOfSightClear", a[1], a[2], a[3], b[1], b[2], b[3], true, true, true, true, true, false, false, state.vehicle)
        ray.clear = result == nil and NULL or result
        work.checked[#work.checked + 1] = ray
        work.next = work.next + 1
        if result ~= true then
            work.clear = result
            if result == false then work.blockedRay = #work.checked end
            work.done = true
            break
        end
    end
    if work.next > #work.rays then work.done = true end
    local cost = elapsed(getTickCount(), began)
    work.cost = work.cost + cost
    local status = work.clear == false and "blocked" or work.done and (work.clear == true and "clear" or "unavailable") or "pending"
    if status == "blocked" then state.lastClearProbe = nil end
    if status == "clear" then state.lastClearProbe = work end
    local clear = work.done and work.clear or nil
    if work.clear == false then clear = false end
    local cached = status == "pending" and compatible(state.lastClearProbe)
    if cached then clear = true end
    state.probe = {tick_ms = now, probe_id = work.started, status = status, lookahead_m = work.horizon,
        reaction_distance_m = work.reaction, reverse = work.direction < 0, motion_direction = work.motion,
        requested_direction = work.direction, predicted_yaw_deg = work.yaw, heights_local = work.heights,
        rays = work.checked, ray_count_total = #work.rays, blocked_ray = work.blockedRay,
        complete = work.next > #work.rays, bounds_local = work.bounds, cost_ms = cost, total_cost_ms = work.cost,
        passes = work.passes, cached_clear_tick_ms = cached and state.lastClearProbe.started or NULL}
    state.probeSummary = {tick_ms = now, probe_id = work.started, status = status, lookahead_m = work.horizon,
        reverse = work.direction < 0, motion_direction = work.motion, requested_direction = work.direction,
        predicted_yaw_deg = work.yaw, complete = state.probe.complete, clear = clear, ray_count = #work.checked,
        ray_count_total = #work.rays, cost_ms = cost, passes = work.passes, cached_clear_tick_ms = state.probe.cached_clear_tick_ms,
        rays_event = "autopilot_obstacle_probe"}
    state.lastProbe, state.pathClear = now, clear
    return clear
end


applyAutopilot = function(now)
    if not autopilot.enabled then return end
    if autopilot.detail and autopilot.detail.controls_suspended then
        if not state.controlsSuspended then
            local failed = releaseAutopilotControls()
            if #failed > 0 then stopAutopilot("Не удалось отпустить управление на стоянке"); return end
            state.controlsSuspended = true
            state.applied = {tick_ms = now, suspended = true, reason = "vehicle_frozen"}
            emit("autopilot_controls_suspended", {suspended = true, reason = "vehicle_frozen"})
        end
        return
    end
    if state.controlsSuspended then
        state.controlsSuspended = false
        emit("autopilot_controls_suspended", {suspended = false, reason = "vehicle_unfrozen"})
    end
    local out = autopilot.output
    local values = {out.throttle, out.brake, math.max(0, -out.aileron), math.max(0, out.aileron),
        math.max(0, -out.elevator), math.max(0, out.elevator)}
    state.controlsOwned = true
    -- MTA clears the opposite direction even when setting zero. Write the active side last.
    local order = {1, 2, out.aileron < 0 and 4 or 3, out.aileron < 0 and 3 or 4,
        out.elevator < 0 and 6 or 5, out.elevator < 0 and 5 or 6}
    for _, i in ipairs(order) do
        local name = ownedAnalog[i]
        if read("setAnalogControlState", name, values[i], true) ~= true then
            stopAutopilot("Ошибка команды управления: " .. name)
            return
        end
    end
    local readback = {}
    for i, name in ipairs(ownedAnalog) do readback[i] = number(read("getAnalogControlState", name)) or NULL end
    local gearDown = read("getVehicleLandingGearDown", state.vehicle)
    if out.gear_down ~= nil then
        if type(gearDown) ~= "boolean" then stopAutopilot("Неизвестно положение шасси"); return end
        if state.gearDesired ~= out.gear_down then
            state.gearDesired, state.gearAttempts, state.gearAttemptTick = out.gear_down, 0, nil
        end
        if gearDown == out.gear_down then state.gearAttempts = 0
        elseif elapsed(now, state.gearAttemptTick) >= 2000 then
            if (state.gearAttempts or 0) >= 3 then stopAutopilot("Шасси не отвечают на управление"); return end
            state.gearAttempts = (state.gearAttempts or 0) + 1
            state.gearAttemptTick, state.gearPulse = now, now
            emit("autopilot_gear_request", {down = out.gear_down, observed_down = gearDown, attempt = state.gearAttempts}, true)
        end
    end
    local holding = autopilot.detail and autopilot.detail.rudder_control == "hold"
    local pulse = holding or elapsed(now, autopilot.started) % 180 / 180 < math.abs(out.rudder)
    local digital = {pulse and out.rudder < 0, pulse and out.rudder > 0, out.handbrake,
        state.gearPulse ~= nil and elapsed(now, state.gearPulse) < 100}
    for i, name in ipairs(ownedDigital) do
        if read("setPedControlState", localPlayer, name, digital[i]) ~= true then
            stopAutopilot("Ошибка команды управления: " .. name)
            return
        end
    end
    state.applied = {tick_ms = now, analog = values, analog_names = ownedAnalog, analog_write_order = order,
        analog_readback = readback, readback_stage = "after_write_before_game_frame",
        digital = digital, digital_names = ownedDigital, rudder_control = holding and "hold" or "pwm",
        rudder_pwm_period_ms = holding and 0 or 180}
end

updateAutopilot = function(item, now)
    if not autopilot.enabled then return end
    local oldPhase, oldWaypoint = autopilot.phase, autopilot.waypoint
    local clear
    if item.vehicle ~= NULL then clear = groundProbe(item, now) end
    if state.recording and state.probe and state.loggedProbeTick ~= state.probe.tick_ms then
        emit("autopilot_obstacle_probe", state.probe)
        state.loggedProbeTick = state.probe.tick_ms
    end
    autopilot:update(item, now, clear, state.probe and state.probe.status)
    state.windowRestored = false
    if autopilot.detail and autopilot.detail.timing_resynced then
        emit("autopilot_timing_resync", {frame_gap_ms = autopilot.detail.frame_gap_ms,
            minimized = item.window_minimized, restored = item.window_restored}, true)
    end
    if not autopilot.enabled then stopAutopilot(autopilot.status)
    elseif oldPhase ~= autopilot.phase or oldWaypoint ~= autopilot.waypoint then
        emit("autopilot_transition", {previous_phase = oldPhase, current = autopilotSnapshot()}, true)
    end
end

local function startAutopilot()
    if autopilot.enabled or state.nextJob then return end
    if not state.pilotReady then autopilot.status = "Ожидание province_pilot"; return end
    if type(_G.setAnalogControlState) ~= "function" or type(_G.setPedControlState) ~= "function" then
        autopilot.status = "Клиентское API управления недоступно"
        return
    end
    local beganRecording = autopilot.telemetry and not state.recording
    if beganRecording then start("autopilot") end
    if autopilot.telemetry and not state.recording then autopilot.status = "Не удалось включить выбранную запись телеметрии"; return end
    local now = getTickCount()
    local item = sample(now, 0, inputSnapshot())
    state.windowRestored = false
    local ok, reason = autopilot:start(item, now)
    if not ok then
        autopilot.status = reason
        emit("autopilot_start_rejected", {reason = reason}, true)
        if beganRecording and state.recording then stop("autopilot_start_rejected") end
        return
    end
    state.apSessionActive = true
    state.completionGrace = nil
    state.rmbLastTick = now
    state.controlsSuspended = false
    if state.recording then state.status = "Идёт запись полёта с автопилотом." end
    state.lastSample, state.lastProbe, state.probe, state.pathClear = nil, nil, nil, nil
    state.probeSummary, state.loggedProbeTick = nil, nil
    state.probeWork, state.lastClearProbe = nil, nil
    state.lastAppliedPhase, state.gearDesired, state.gearAttempts = nil, nil, 0
    for _, name in ipairs(controls) do
        for key in pairs(read("getBoundKeys", name) or {}) do keySet[key] = true end
    end
    emit("autopilot_start", {controller_version = PilotController.version, initial = item,
        instruction = autopilot.instruction, telemetry = autopilot.telemetry}, true)
    playAutopilotSound(true)
    bridge.update("autopilot", "1")
    bridge.update("autopilot_status", autopilot.status)
    return true
end

local function updateAutonomy(now)
    local pending = state.nextJob
    if not pending then return end
    if not state.autonomy or not state.pilotReady then stopAutopilot("Автономный цикл отменён"); return end
    if elapsed(now, pending.began) > 60000 then stopAutopilot("Новый рейс не начался: ожидание истекло"); return end
    local vehicle = read("getPedOccupiedVehicle", localPlayer)
    if pending.stage == "wait_exit" then
        if vehicle or elapsed(now, pending.began) < 750 then return end
        pending.stage, pending.requested = "wait_plane", now
        state.destination = nil
        autopilot.terminal, autopilot.instruction = false, "unknown"
        emit("autonomy_job_request", {completed_routes = state.completedRoutes}, true)
        if not acceptJob() then stopAutopilot("Не удалось повторно трудоустроиться"); return end
        autopilot.status = "Автономно: ожидание нового самолёта"
    elseif pending.stage == "wait_plane" then
        local ready = vehicle and read("getElementModel", vehicle) == 519
            and read("getPedOccupiedVehicleSeat", localPlayer) == 0
            and read("getVehicleController", vehicle) == localPlayer
        if not ready then pending.readySince = nil; return end
        pending.readySince = pending.readySince or now
        if elapsed(now, pending.readySince) < 300 then return end
        -- Clear the wait before starting: a synchronous failure must not rearm it.
        state.nextJob = nil
        bridge.update("autopilot_waiting", "0")
        if startAutopilot() then emit("autonomy_restart", {completed_routes = state.completedRoutes}, true)
        else stopAutopilot(autopilot.status) end
    end
end

local function command(value)
    if value == "accept_job" then
        if not state.nextJob then acceptJob() end
    elseif value == "start" then start()
    elseif value == "stop" then stop("user")
    elseif value == "autopilot_start" then startAutopilot()
    elseif value == "autopilot_stop" then stopAutopilot("Остановлен кнопкой")
    elseif value == "autopilot_autonomy:1" or value == "autopilot_autonomy:0" then
        state.autonomy = value == "autopilot_autonomy:1"
        if not state.autonomy then
            state.completionGrace = nil
            if state.nextJob then stopAutopilot("Автономность выключена") end
        end
        emit("autonomy_setting", {enabled = state.autonomy})
    elseif value == "autopilot_hud:1" or value == "autopilot_hud:0" then
        state.hudEnabled = value == "autopilot_hud:1"
        state.hudSmooth, state.hudLastFrame, state.hudError = nil, nil, nil
        emit("autopilot_hud", {enabled = state.hudEnabled})
    elseif value == "autopilot_telemetry:1" or value == "autopilot_telemetry:0" then
        autopilot.telemetry = value == "autopilot_telemetry:1"
        if autopilot.enabled then
            if autopilot.telemetry then start("autopilot")
            elseif state.recordingOwner == "autopilot" then stop("autopilot_telemetry_unchecked") end
        end
    elseif value:sub(1, 9) == "interval:" then
        local interval = tonumber(value:sub(10))
        if interval == 20 or interval == 50 or interval == 100 or interval == 200 then
            state.interval = interval
            emit("sample_interval", {ms = interval}, true)
        end
    elseif value:sub(1, 6) == "phase:" then
        state.phase = value:sub(7, 40)
        emit("phase", {name = state.phase}, true)
    elseif value:sub(1, 5) == "note:" then emit("note", {text = value:sub(6, 512)}, true) end
end

local function onFrame(frameMs, background)
    state.backgroundTick = background == true
    local frameBegan = getTickCount()
    local now = frameBegan
    state.frame, state.frames = state.frame + 1, state.frames + 1
    if not state.lastFps then state.lastFps = now end
    if elapsed(now, state.lastFps) >= 1000 then
        state.fps = state.frames * 1000 / elapsed(now, state.lastFps)
        state.frames, state.lastFps = 0, now
    end
    if elapsed(now, state.lastUi) >= 250 then
        local pilot = read("getResourceFromName", "province_pilot")
        local ready = pilot and read("getResourceState", pilot) == "running" or false
        if state.pilotReady and not ready then stopAutopilot("Ресурс пилота остановлен"); stop("pilot_resource_stopped") end
        if not state.pilotReady and ready then state.status = "province_pilot запущен. Готов к ручной записи." end
        state.pilotReady = ready
        state.pilotRoot = ready and read("getResourceRootElement", pilot) or nil
        if not ready then state.status = "Ожидание запуска province_pilot." end
        if state.retryNotifications then
            state.retryNotifications = false
            installNotificationObservers()
        end
        if not bridge.update("heartbeat", "1") then cleanup(); return end
        for _ = 1, 8 do
            local value = bridge.command()
            if not value then break end
            command(value)
        end
        updateUi(state.latest)
        state.lastUi = now
        if (state.recording or autopilot.enabled) and elapsed(now, state.lastNotificationPoll) >= 1000 then
            pollNotifications()
            state.lastNotificationPoll = now
        end
    end
    -- Startup/commands can consume milliseconds; never feed a pre-start tick to the controller.
    now = getTickCount()
    updateAutonomy(now)
    now = getTickCount()
    syncSafetyMonitor()
    if safetyActive() and elapsed(now, state.lastSafetyScan) >= 2000 then scanSafetyPlayers(now) end
    local input
    if state.recording then
        input = inputSnapshot()
        if not sameInput(input, state.lastInput) then
            for _, name in ipairs(controls) do
                if not state.lastInput or input.digital[name] ~= state.lastInput.digital[name] then
                    state.controlSince[name] = input.digital[name] == true and now or nil
                end
            end
            emit("input", {frame = state.frame, input = input})
            state.lastInput = input
        end
    end
    local interval = state.recording and state.interval or 250
    if autopilot.enabled then interval = math.min(50, interval) end
    if elapsed(now, state.lastSample) >= interval then
        state.latest = sample(now, frameMs, input or inputSnapshot())
        updateAutopilot(state.latest, now)
        state.latest.autopilot = autopilotSnapshot(true)
        if state.recording and elapsed(now, state.lastLogged) >= state.interval then
            state.samples = state.samples + 1
            state.latest.index = state.samples
            state.latest.logged_sample_dt_ms = state.lastLogged and elapsed(now, state.lastLogged) or NULL
            emit("sample", state.latest)
            state.lastLogged = now
        end
        state.lastSample = now
    end
    applyAutopilot(now)
    updateRmb(now)
    if state.recording and elapsed(now, state.lastCollisionLog) >= 100 then flushCollisionBurst() end
    if state.recording then flush(false) end
    if state.recording then
        local perf = state.perf
        local cost = elapsed(getTickCount(), frameBegan)
        state.maxFrameCost = math.max(state.maxFrameCost or 0, cost)
        perf.frames, perf.total_ms = perf.frames + 1, perf.total_ms + cost
        perf.max_ms, perf.frame_max_ms = math.max(perf.max_ms, cost), math.max(perf.frame_max_ms, frameMs or 0)
        if elapsed(now, perf.began) >= 1000 then
            emit("collector_performance", {frames = perf.frames, window_ms = elapsed(now, perf.began),
                collector_mean_ms = perf.total_ms / perf.frames, collector_max_ms = perf.max_ms,
                frame_max_ms = perf.frame_max_ms, scan_last_ms = state.lastScanCost or 0,
                scan_max_ms = state.maxScanCost or 0, scan_elements = state.inspected or 0,
                hud_frames = state.hudFrames or 0, hud_max_ms = state.hudMaxCost or 0,
                hud_mean_ms = (state.hudTotalCost or 0) / math.max(1, state.hudFrames or 0),
                buffered_bytes = state.bufferBytes, clock_resolution = "getTickCount milliseconds",
                coverage = "collector: preRender; hud: render; other asynchronous event handlers excluded"})
            state.perf = {frames = 0, total_ms = 0, max_ms = 0, frame_max_ms = 0, began = now}
            state.hudFrames, state.hudTotalCost, state.hudMaxCost = 0, 0, 0
        end
    end
end

local function drawAutopilotHud(item, now)
    local width, height = guiGetScreenSize()
    if not finite(width) or not finite(height) or width < 1 or height < 1 then return end
    local scale = math.min(width / 1280, height / 800, 1.3)
    local cx, cy = width * 0.5, height * 0.46
    local green, dim, amber, shadow = 0xED70FF92, 0xAF64D885, 0xFFFFC36A, 0x9006160C
    local h = state.hudSmooth
    local dt = elapsed(now, state.hudLastFrame)
    if not h or dt > 300 then h = {}; state.hudSmooth = h end
    state.hudLastFrame = now
    local blend = 1 - math.exp(-math.min(dt, 300) / 85)
    local function smooth(key, value, angular)
        if not finite(value) then h[key] = nil; return nil end
        local old = h[key]
        local delta = old and (angular and (value - old + 180) % 360 - 180 or value - old)
        h[key] = old and old + delta * blend or value
        if angular then h[key] = (h[key] + 180) % 360 - 180 end
        return h[key]
    end
    local heading = smooth("heading", item.heading_deg, true)
    local pitch, roll = smooth("pitch", item.pitch_deg), smooth("roll", item.roll_deg, true)
    local speed = smooth("speed", item.speed_kmh)
    local altitude = smooth("altitude", item.position_m and item.position_m[3])
    local climb, agl = smooth("climb", item.climb_mps), smooth("agl", item.agl_terrain_m)
    local function numberText(value, pattern)
        return finite(value) and string.format(pattern or "%.0f", value) or "--"
    end
    local function line(x1, y1, x2, y2, color, thickness)
        x1, y1, x2, y2 = cx + x1 * scale, cy + y1 * scale, cx + x2 * scale, cy + y2 * scale
        local stroke = (thickness or 1) * scale
        if thickness then dxDrawLine(x1, y1, x2, y2, shadow, stroke + 2 * scale, false) end
        dxDrawLine(x1, y1, x2, y2, color or green, stroke, false)
    end
    local function text(value, x, y, align, color, size, span)
        align, span = align or "left", (span or 160) * scale
        local x1, y1 = cx + x * scale, cy + y * scale
        if align == "right" then x1 = x1 - span elseif align == "center" then x1 = x1 - span * 0.5 end
        local textScale = (size or 0.88) * scale
        dxDrawText(value, x1 + scale, y1 + scale, x1 + span + scale, y1 + 20 * scale,
            shadow, textScale, "default-bold", align, "top", false, false, false, false)
        dxDrawText(value, x1, y1, x1 + span, y1 + 19 * scale,
            color or green, textScale, "default-bold", align, "top", false, false, false, false)
    end
    local function box(x, y, w, height, color)
        line(x, y, x + w, y, color); line(x + w, y, x + w, y + height, color)
        line(x + w, y + height, x, y + height, color); line(x, y + height, x, y, color)
    end
    local nav, detail, output = item.navigation or {}, autopilot.detail or {}, autopilot.output or {}
    local warning = autopilot.phase == "obstacle_hold" or autopilot.phase == "probe_wait" or autopilot.phase == "waiting_marker"
    text("AP  /  " .. string.upper(autopilot.phase), -322, -211, "left", warning and amber or green, 0.94, 370)
    text(state.recording and "REC  /  TELEMETRY" or "REC OFF", 322, -211, "right", state.recording and green or dim)

    -- This ladder is aircraft attitude, independent of the third-person camera.
    if finite(pitch) and finite(roll) then
        local c, s = math.cos(math.rad(-roll)), math.sin(math.rad(-roll))
        local function rotate(x, y) return x * c - y * s, x * s + y * c end
        local function ladderLine(x1, y1, x2, y2, color)
            x1, y1 = rotate(x1, y1); x2, y2 = rotate(x2, y2)
            -- Clip rotated lines to the attitude window (Liang-Barsky).
            local dx, dy, lo, hi = x2 - x1, y2 - y1, 0, 1
            local function clip(p, q)
                if p == 0 then return q >= 0 end
                local t = q / p
                if p < 0 then lo = math.max(lo, t) else hi = math.min(hi, t) end
                return lo <= hi
            end
            if clip(-dx, x1 + 174) and clip(dx, 174 - x1)
                and clip(-dy, y1 + 105) and clip(dy, 105 - y1) then
                line(x1 + lo * dx, y1 + lo * dy, x1 + hi * dx, y1 + hi * dy, color)
            end
        end
        local first = math.max(-90, math.floor((pitch - 32) / 5) * 5)
        local last = math.min(90, math.ceil((pitch + 32) / 5) * 5)
        for tick = first, last, 5 do
            local y, reach = (pitch - tick) * 6, tick == 0 and 165 or 73
            local color = tick == 0 and green or dim
            if tick < 0 then
                for x = 28, reach - 4, 14 do
                    ladderLine(x, y, math.min(x + 8, reach), y, color)
                    ladderLine(-x, y, -math.min(x + 8, reach), y, color)
                end
            else
                ladderLine(28, y, reach, y, color); ladderLine(-reach, y, -28, y, color)
            end
            if tick ~= 0 then
                ladderLine(reach, y, reach, y + (tick > 0 and 5 or -5), color)
                ladderLine(-reach, y, -reach, y + (tick > 0 and 5 or -5), color)
                for _, side in ipairs({-1, 1}) do
                    local lx, ly = rotate(side * (reach + 17), y)
                    if math.abs(lx) < 152 and math.abs(ly) < 93 then
                        text(tostring(tick), lx, ly - 7, "center", dim, 0.73, 38)
                    end
                end
            end
        end
    else text("ATTITUDE --", 0, -55, "center", amber) end
    line(-25, 0, -9, 0, green, 1.5); line(-9, 0, 0, 5, green, 1.5)
    line(0, 5, 9, 0, green, 1.5); line(9, 0, 25, 0, green, 1.5)
    line(0, -8, 0, -3)

    if finite(heading) then
        local course = heading % 360
        for tick = math.floor((course - 35) / 5) * 5, math.ceil((course + 35) / 5) * 5, 5 do
            local x = (tick - course) * 5
            if math.abs(x) <= 170 then
                line(x, -158, x, tick % 10 == 0 and -145 or -151, dim)
                if tick % 10 == 0 then text(string.format("%03d", tick % 360), x, -181, "center", dim, 0.78, 45) end
            end
        end
        line(-5, -137, 0, -143); line(0, -143, 5, -137)
        text(string.format("HDG %03d", math.floor(course + 0.5) % 360), 0, -132, "center")
    else text("HDG --", 0, -132, "center", amber) end

    text("SPEED  KM/H", -216, -70, "right", dim)
    box(-314, -45, 98, 35); text(numberText(speed), -227, -42, "right", green, 1.4)
    text("ALT  M", 216, -70, "left", dim)
    box(216, -45, 98, 35); text(numberText(altitude), 303, -42, "right", green, 1.4)
    text("AGL  " .. numberText(agl) .. " M", 216, 5)
    text("V/S  " .. numberText(climb, "%+.1f") .. " M/S", 216, 29)
    text("PITCH  " .. numberText(pitch, "%+.1f"), -216, 5, "right")
    text("BANK  " .. numberText(roll, "%+.1f"), -216, 29, "right")
    text(item.on_ground == true and "GROUND" or item.on_ground == false and "AIR" or "GROUND --", -216, 67, "right")
    local gear = item.landing_gear_state == "down" and "DOWN" or item.landing_gear_state == "up" and "UP" or "--"
    text("GEAR  " .. gear, 216, 67)

    local turn = nav.heading_error_deg
    text("TURN  " .. numberText(turn, "%+.1f") .. " DEG", 0, 122, "center", nav.ambiguous and amber or green)
    text("DIST  " .. numberText(nav.distance_3d_m) .. " M     DZ  " .. numberText(nav.altitude_error_m, "%+.0f") .. " M",
        0, 147, "center", green, 0.88, 410)
    local marker = type(nav.marker_type) == "string" and string.upper(nav.marker_type) or "NONE"
    text("TARGET  " .. marker .. "  /  #" .. numberText(nav.waypoint_generation), -322, 104, "left", dim, 0.78, 225)
    if type(nav.color_rgba) == "table" then
        local r, g, b, a = unpack(nav.color_rgba)
        if finite(r) and finite(g) and finite(b) and finite(a) then
            local markerColor = 0xFF000000 + math.floor(math.max(0, math.min(255, r))) * 65536
                + math.floor(math.max(0, math.min(255, g))) * 256 + math.floor(math.max(0, math.min(255, b)))
            box(-322, 129, 10, 10, markerColor)
            text(string.format("RGBA %d %d %d %d", r, g, b, a), -304, 125, "left", dim, 0.71, 210)
        end
    end
    local pos = nav.position
    if pos and finite(pos[1]) and finite(pos[2]) and finite(pos[3]) then
        local x, y = getScreenFromWorldPosition(pos[1], pos[2], pos[3], 0, false)
        local pad = 30 * scale
        if finite(x) and finite(y) and x > pad and x < width - pad and y > pad and y < height - pad then
            x, y = (x - cx) / scale, (y - cy) / scale
            local color = nav.ambiguous and amber or green
            line(x, y - 11, x + 11, y, color); line(x + 11, y, x, y + 11, color)
            line(x, y + 11, x - 11, y, color); line(x - 11, y, x, y - 11, color)
        else text("TARGET OFFSCREEN", 322, 104, "right", amber, 0.78, 210) end
    else text("NO TARGET", 322, 104, "right", amber) end

    line(-322, 177, 322, 177, dim)
    text("CMD  SPD " .. numberText(detail.goal_speed_kmh) .. "   PITCH " .. numberText(detail.goal_pitch_deg, "%+.1f")
        .. "   BANK " .. numberText(detail.goal_roll_deg, "%+.1f"), -322, 187, "left", dim, 0.81, 490)
    text("THR " .. numberText(finite(output.throttle) and output.throttle * 100, "%.0f")
        .. "%  BRK " .. numberText(finite(output.brake) and output.brake * 100, "%.0f") .. "%", 322, 187, "right", dim, 0.81, 190)
    local function controlBar(label, x, value)
        text(label, x, 216, "left", dim, 0.75, 75)
        local center = x + 120
        line(center - 38, 224, center + 38, 224, dim)
        line(center, 220, center, 228, dim)
        if finite(value) then
            local cursor = center + math.max(-1, math.min(1, value)) * 38
            line(cursor, 218, cursor, 230, green, 2)
        end
        text(numberText(value, "%+.2f"), x + 206, 216, "right", green, 0.75, 48)
    end
    controlBar("RUDDER", -322, output.rudder)
    controlBar("AILERON", -104, output.aileron)
    controlBar("ELEVATOR", 114, output.elevator)
end

local function onHudRender()
    local now, item = getTickCount(), state.latest
    if not state.hudEnabled or state.hudError or state.minimized or not autopilot.enabled or not item
        or read("dfMenuOpen") == true or elapsed(now, state.lastSample) > 300 or not valid(state.vehicle) then
        state.hudSmooth, state.hudLastFrame = nil, nil
        return
    end
    local ok, message = pcall(drawAutopilotHud, item, now)
    local cost = elapsed(getTickCount(), now)
    if state.recording then
        state.hudFrames = (state.hudFrames or 0) + 1
        state.hudTotalCost = (state.hudTotalCost or 0) + cost
        state.hudMaxCost = math.max(state.hudMaxCost or 0, cost)
    end
    if not ok then
        state.hudError = tostring(message):sub(1, 512)
        bridge.update("autopilot_hud_error", state.hudError)
        emit("autopilot_hud_error", {message = state.hudError, autopilot_continues = true}, true)
    end
end

local function guard(fn, name, recoverable)
    return function(...)
        if state.closed then return end
        local ok, message = pcall(fn, ...)
        if not ok then
            message = tostring(message):sub(1, 2048)
            if recoverable then
                state.observerErrorCount = state.observerErrorCount + 1
                state.lastObserverError = tostring(name) .. ": " .. message
                local previous = state.observerErrors[name]
                local now = getTickCount()
                if not previous or elapsed(now, previous) >= 1000 then
                    state.observerErrors[name] = now
                    emit("observer_error", {observer = name, message = message,
                        total_errors = state.observerErrorCount, recording_continues = true}, true)
                end
                return
            end
            emit("collector_error", {observer = name, message = message}, true)
            stopAutopilot("Ошибка обработчика: " .. tostring(name))
            state.recording = false
            state.collectorError = message
            state.status = "Ошибка телеметрии: " .. tostring(message)
            updateUi(nil)
        end
    end
end

state.hookStatus = {}
local function hook(name, element, fn, recoverable)
    local callback = guard(fn, name, recoverable)
    local ok = read("addEventHandler", name, element, callback)
    state.hookStatus[name] = ok == true
    if ok then state.hooks[#state.hooks + 1] = {name, element, callback} end
end

local function notificationContext()
    local vehicle = read("getPedOccupiedVehicle", localPlayer)
    return {sample_index = state.samples, phase = state.phase,
        vehicle = valid(vehicle) and elementId(vehicle) or NULL,
        position = valid(vehicle) and vector("getElementPosition", vehicle) or NULL,
        velocity_raw = valid(vehicle) and vector("getElementVelocity", vehicle) or NULL,
        landing_gear_down = valid(vehicle) and read("getVehicleLandingGearDown", vehicle),
        target = state.target and elementId(state.target) or NULL}
end

local function notificationText(value, depth, output)
    depth, output = depth or 0, output or {}
    if depth > 6 or #output >= 40 then return output end
    if type(value) == "string" then output[#output + 1] = value
    elseif type(value) == "table" then
        for _, nested in pairs(value) do notificationText(nested, depth + 1, output) end
    end
    return output
end

local function notification(channel, payload, origin, text)
    if type(text) ~= "string" then text = table.concat(notificationText(payload), "\n") end
    local plain = text:gsub("#%x%x%x%x%x%x", "")
    local trusted = origin == "province_pilot" and channel == "province:sendNotification"
    if trusted then
        state.jobNotificationCount = (state.jobNotificationCount or 0) + 1
        state.lastJobNotification = channel .. " | " .. origin .. "\n" .. plain
        state.jobNotificationTick = getTickCount()
        local destination = plain:match("Ваш пункт назначения%s*%-%s*([^\r\n]+)")
        if destination then
            destination = destination:gsub("%s+$", ""):gsub("%.$", "")
            if destination ~= state.destination then
                emit("job_destination", {previous = state.destination or NULL, destination = destination,
                    text = plain, channel = channel, origin = origin, context = notificationContext()}, true)
                state.destination = destination
            end
        end
        bridge.update("destination", state.destination or "ещё не назначен")
        bridge.update("notification", state.lastJobNotification)
        bridge.update("notification_count", tostring(state.jobNotificationCount))
    end
    if state.recording then
        state.notificationCount = state.notificationCount + 1
        if plain ~= "" then state.lastNotification = channel .. " | " .. tostring(origin or "unknown") .. "\n" .. plain end
        emit("job_notification", {channel = channel, origin = origin or "unknown", payload = payload,
            text_raw = text, text_plain = plain, index = state.notificationCount,
            destination = state.destination or NULL,
            context = notificationContext(), attribution = origin == "province_pilot" and "pilot_resource" or "notification_channel"}, true)
    end
    if trusted then
        local now = getTickCount()
        local working = state.apSessionActive or autopilot.enabled
            or state.completionGrace and elapsed(now, state.completionGrace) < 5000
        -- Duplicate completion events must not cancel or repeat an employment request.
        if state.nextJob and plain:find("Вы выполнили рейс", 1, true) then return end
        local mode = autopilot:notify(plain, now)
        if mode == "completed" or mode == "job_ended" then
            local repeatJob = mode == "completed" and working and state.autonomy
            if repeatJob then emit("autonomy_queued", {reason = "route_completed", wait_for_exit = true}, true) end
            stopAutopilot(autopilot.status)
            if repeatJob then
                state.completedRoutes = (state.completedRoutes or 0) + 1
                state.nextJob = {stage = "wait_exit", began = now}
                autopilot.status = "Рейс выполнен: ожидание выхода и нового трудоустройства"
                bridge.update("autopilot_waiting", "1")
                bridge.update("autopilot_status", autopilot.status)
            end
        end
    end
end

local function arguments(...)
    local args = {count = select("#", ...), values = {}}
    for i = 1, args.count do
        local value = select(i, ...)
        args.values[i] = value == nil and NULL or value
    end
    return args
end

local function notificationElement(element, channel)
    local payload = read("getElementData", element, "data")
    local encoded = json(payload)
    if encoded == state.notificationSeen[element] then return end
    state.notificationSeen[element] = encoded
    local owner = ownership(element)
    local text = type(payload) == "table" and table.concat(notificationText({payload.header or "", payload.text or ""}), "\n") or ""
    notification(channel, {id = elementId(element), data = payload or NULL}, owner, text)
end

pollNotifications = guard(function()
    if not state.recording and not autopilot.enabled then return end
    for _, element in ipairs(read("getElementsByType", "notifications:Static", root) or {}) do
        notificationElement(element, "static_snapshot")
    end
    local groups = read("getElementData", root, "notifications:groups")
    local encoded = json(groups)
    if encoded ~= state.notificationGroups then
        state.notificationGroups = encoded
        if type(groups) == "table" then notification("groups_snapshot", groups, "province_notifications") end
    end
end, "notification_poll", true)

local notificationEvents = {
    "province:sendNotification", "notifications.createStatus.client", "notifications.clearStatus.client",
    "notifications.createQuickButtons.client", "notifications.addQuickButtons.client",
    "notifications.removeQuickButtons.client", "notifications.destroyQuickButtons.client",
    "notifications:setQuickButtonActive.client", "notifications.setVisibleButtonHelp.client",
    "notifications.hideAllButtonHelp.client", "plrTimer:init", "plrTimer:secs", "plrTimer:updateText",
    "pilot:OpenWorkGui", "pilot:PlaySound", "Pilot:CloseGUIWork",
}
installNotificationObservers = function()
    for _, event in ipairs(notificationEvents) do
        if not state.hookStatus[event] then
            local name = event
            hook(name, root, function(...)
                local owner = ownership(source)
                if not state.recording and not autopilot.enabled and owner ~= "province_pilot" then return end
                notification(name, {source = elementId(source), arguments = arguments(...)}, owner,
                    table.concat(notificationText({...}), "\n"))
            end, true)
        end
    end
end

hook("onClientElementDataChange", root, function(key)
    if not state.recording then return end
    if key == "data" and read("getElementType", source) == "notifications:Static" then
        notificationElement(source, "static_change")
    elseif source == root and key == "notifications:groups" then pollNotifications() end
end, true)
hook("onClientChatMessage", root, function(text, r, g, b, messageType)
    safetyChat(text, r, g, b, messageType)
    if not state.recording then return end
    local plain = tostring(text):gsub("#%x%x%x%x%x%x", "")
    local relevant = false
    for _, word in ipairs({"илот", "амол", "шасси", "Шасси", "маркер", "Маркер", "Взл", "взл",
        "осадк", "рейс", "Рейс", "аэропорт", "Аэропорт", "ысот", "курса", "Курса"}) do
        if plain:find(word, 1, true) then relevant = true; break end
    end
    if relevant then notification("chat", {color = {r, g, b}, message_type = messageType}, "chat_keyword_match", text) end
end, true)
hook("onClientResourceStart", root, function()
    state.retryNotifications = true
end)
hook("onClientResourceStop", root, function(stoppedResource)
    if read("getResourceName", stoppedResource) == "province_pilot" then
        stopAutopilot("Ресурс пилота остановлен")
        stop("pilot_resource_stop")
        state.pilotReady = false
        updateUi(nil)
    end
end)

installNotificationObservers()

hook("onClientPreRender", root, function(frameMs)
    state.lastRenderTick = getTickCount()
    onFrame(frameMs)
end)
hook("onClientRender", root, onHudRender, true)
hook("onClientKey", root, function(key, pressed)
    local ownRmb = key == "mouse2" and state.rmbDownTick ~= nil
    if autopilot.enabled and pressed and keySet[key] and not ownRmb and not read("isChatBoxInputActive")
        and not read("isConsoleActive") and not read("dfMenuOpen") then stopAutopilot("Ручной перехват: " .. key) end
    if state.recording and keySet[key] and not read("isChatBoxInputActive") and not read("isConsoleActive") then
        emit("key", {key = key, pressed = pressed, frame = state.frame})
    end
end)
for _, event in ipairs({"onClientMarkerHit", "onClientMarkerLeave"}) do
    local name = event
    hook(name, root, function(element, matchingDimension)
        if (state.recording or autopilot.enabled) and (element == localPlayer or element == state.vehicle)
            and (state.candidateByElement[source] or select(2, ownership(source))) then
            emit(name, {marker = elementId(source), position = vector("getElementPosition", source),
                matching_dimension = matchingDimension, element = elementId(element)}, true)
            state.lastScan = nil
        end
    end)
end
hook("onClientElementDestroy", root, function()
    if state.notificationSeen[source] then
        local owner = ownership(source)
        local previous = state.notificationSeen[source]
        state.notificationSeen[source] = nil
        notification("static_destroy", {id = elementId(source), previous_json = previous}, owner)
    end
    local item = state.candidateByElement[source]
    if item then
        emit("navigation_destroy", {candidate = item, selected = source == state.target}, true)
        state.lastScan = nil
        state.candidateByElement[source] = nil
    end
end, true)
local function impactContext()
    local vehicle = source
    local raw = vector("getElementVelocity", vehicle)
    local latest = state.latest or {}
    return {vehicle = elementId(vehicle), position_m = vector("getElementPosition", vehicle) or NULL,
        velocity_world_mps = raw and scale(raw, 50) or NULL, speed_kmh = raw and norm(raw)*180 or NULL,
        health_at_callback = read("getElementHealth", vehicle), on_ground = read("isVehicleOnGround", vehicle),
        landing_gear_down = read("getVehicleLandingGearDown", vehicle), controls = inputSnapshot(),
        autopilot = autopilotSnapshot(), previous_sample_index = state.samples,
        previous_sample_age_ms = state.lastSample and elapsed(getTickCount(), state.lastSample) or NULL,
        heading_deg = latest.heading_deg, pitch_deg = latest.pitch_deg, roll_deg = latest.roll_deg,
        navigation = latest.navigation or NULL}
end
hook("onClientVehicleCollision", root, function(hit, force, part, x, y, z, nx, ny, nz, otherForce, model)
    if source ~= read("getPedOccupiedVehicle", localPlayer) then return end
    safetyCollision(hit)
    if state.recording then
        local now = getTickCount()
        if elapsed(now, state.lastCollisionLog) >= 100 then
            flushCollisionBurst()
            emit("collision", {hit = valid(hit) and elementId(hit) or NULL,
                hit_kind = valid(hit) and read("getElementType", hit) or "world",
                hit_model = model or (valid(hit) and read("getElementModel", hit)) or NULL,
                force_raw = force, other_force_raw = otherForce, bodypart = part,
                position = {x or NULL, y or NULL, z or NULL}, normal = {nx or NULL, ny or NULL, nz or NULL},
                context = impactContext(), suppressed_since_previous = state.collisionSuppressed or 0,
                suppressed_peak_force_raw = state.collisionPeak or 0,
                health_timing = "pre_reaction; see vehicle_damage and subsequent samples"}, true)
            state.lastCollisionLog, state.collisionSuppressed, state.collisionPeak = now, 0, 0
        else
            state.collisionSuppressed = (state.collisionSuppressed or 0) + 1
            state.collisionLast = {hit = valid(hit) and elementId(hit) or NULL, hit_model = model,
                force_raw = force, other_force_raw = otherForce, bodypart = part,
                position = {x, y, z}, normal = {nx, ny, nz}, tick_ms = now,
                sample_index = state.samples, autopilot_phase = autopilot.phase, requested = autopilot.output}
            if (number(force) or 0) >= (state.collisionPeak or 0) then state.collisionStrongest = state.collisionLast end
            state.collisionPeak = math.max(state.collisionPeak or 0, number(force) or 0)
        end
    end
    if autopilot.enabled and finite(nz) and math.abs(nz) < 0.65 and finite(force) and force > 0 then
        stopAutopilot("Столкновение с препятствием: ручной перехват")
    end
end, true)
hook("onClientVehicleDamage", root, function(attacker, weapon, loss, x, y, z, tyre)
    if source ~= read("getPedOccupiedVehicle", localPlayer) then return end
    if state.recording then emit("vehicle_damage", {attacker = valid(attacker) and elementId(attacker) or NULL,
        weapon = weapon, loss = loss, tyre = tyre, position = {x or NULL, y or NULL, z or NULL}, context = impactContext()}, true) end
    if autopilot.enabled and finite(loss) and loss >= 10 then stopAutopilot("Самолёт получил повреждение") end
end, true)
hook("onClientPlayerWasted", localPlayer, function() stopAutopilot("Пилот погиб"); stop("player_wasted") end)
local function stopBackgroundTimer()
    if state.backgroundTimer then read("killTimer", state.backgroundTimer) end
    state.backgroundTimer = nil
end
hook("onClientMinimize", root, function()
    state.minimized = true
    if not state.backgroundTimer then
        state.backgroundTimer = read("setTimer", guard(function()
            if state.closed or not state.minimized or not (autopilot.enabled or state.recording or state.nextJob or state.rmbDownTick) then return end
            local now = getTickCount()
            if elapsed(now, state.lastRenderTick) < 100 or elapsed(now, state.lastSample) < 50 then return end
            onFrame(elapsed(now, state.lastSample), true)
        end, "background_tick"), 50, 0)
    end
    emit("window_minimize", {autopilot_continues = autopilot.enabled,
        background_timer = state.backgroundTimer ~= nil and state.backgroundTimer ~= false}, true)
end)
hook("onClientRestore", root, function()
    state.minimized, state.windowRestored, state.previous = false, true, nil
    stopBackgroundTimer()
    state.lastSample, state.lastScan, state.surface = nil, nil, nil
    state.probeWork, state.lastClearProbe, state.lastProbe = nil, nil, nil
    emit("window_restore", {autopilot_continues = autopilot.enabled}, true)
end)

cleanup = function()
    if state.closed then return end
    stopBackgroundTimer()
    stopAutopilot("Выгрузка скрипта")
    native.alertMonitor(false)
    state.safetyMonitorActive = false
    stop("script_cleanup")
    flush(true)
    state.closed = true
    for _, entry in ipairs(state.hooks) do read("removeEventHandler", entry[1], entry[2], entry[3]) end
    bridge.update("loaded", "0")
    _G.__DarkFlamePilotCleanup = nil
end
hook("onClientResourceStop", resourceRoot, cleanup)
_G.__DarkFlamePilotCleanup = cleanup
bridge.update("loaded", "1")
if not state.hookStatus.onClientPreRender then
    state.status = "Ошибка: обработчик onClientPreRender не зарегистрирован."
    bridge.update("loaded", "0")
end
updateUi(nil)

-- BEGIN EMBEDDED PILOT CONTROLLER
local PilotController = (function()
-- Pure controller: no game APIs, resource loading or server events.
local Controller = {}
Controller.__index = Controller
Controller.version = "0.1.94"

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
            if self.enabled and self.airborne then
                -- Fired in the air: keep flying with the alarm on; the ground logic stops after touchdown.
                self.jobEnded = "Работа завершена / увольнение"
            else
                self:stop("Работа завершена / увольнение")
            end
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
    elseif text:find("Выпустите шасси", 1, true) then
        self.landing, self.gearJobRequest, self.gearDeployReason = true, true, "job_notice"
    end
    if mode == "takeoff" then self.takeoffPermit = now or self.lastTick or 0 end
    if mode and mode ~= self.instruction then
        self.pushbackPending = self.enabled and self.instruction == "reverse" and mode == "taxi" or false
        self.pushbackNoticeTick, self.pushback = now or self.lastTick, false
        if mode == "reverse" then self.departWaypoint = self.waypoint end
        self.parkingHold = nil
        self.taxiTarget, self.taxiReentry = nil, nil
        self.instruction = mode
        if mode == "takeoff" or mode == "reverse" or mode == "boarding_move" then
            self.finalLanding, self.landingHeading = false, nil
            self.flightSeen, self.landing = false, false
            self.gearJobRequest, self.gearDeployReason = false, nil
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
    self.controlReady = false
    self.groundProgress = nil
    self.jobEnded, self.holdSince, self.turnback, self.aerobrake, self.routeKnown, self.aggressiveNow = nil, nil, nil, nil, nil, false
    self.groundSince, self.airSince, self.missingSince = nil, nil, nil
    self.airborne = data.on_ground == false and finite(data.agl_terrain_m) and data.agl_terrain_m > 3
    self.flightSeen = self.airborne or false
    self.waypoint, self.output, self.clearSince = 0, neutral(), nil
    self.closest, self.passedSince, self.blocked = nil, nil, false
    self.captureObservation, self.pendingCapture = nil, nil
    self.airChainPlan = nil
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
    self.gearDeployReason = self.gearJobRequest and "job_notice"
        or self.airborne and self.landing and data.landing_gear_down == true and "already_down_on_approach" or nil
    self.phase, self.status = "armed", "Включён"
    return true
end

local function stopMotion(out, data)
    local forward = data.velocity_body_rfu_mps and data.velocity_body_rfu_mps[2]
    if finite(forward) and forward < -0.3 then out.throttle = 0.4
    elseif data.speed_kmh > 1 then out.brake = (data.speed_kmh > 30 or out.hard_stop) and 1 or 0.55 end
    out.handbrake = data.speed_kmh < 3
end

function Controller:taxiReentryIntent(data, now)
    -- Ground recovery of a red checkpoint, reverse first (the pilot's sketch): a marker left beside or
    -- behind is backed into with the tail, not circled forward; a marker that appeared around a
    -- standing aircraft is left backwards and re-entered, because the server counts entries only.
    local nav = data.navigation
    local color = type(nav) == "table" and nav.color_rgba or {}
    local red = finite(color[1]) and finite(color[2]) and finite(color[3]) and color[1] > 200 and color[2] < 80 and color[3] < 80
    if data.on_ground ~= true or self.airborne or type(nav) ~= "table" or nav.marker_type ~= "checkpoint" or not red
        or self.pushback or self.pushbackPending or self.instruction == "reverse" or self.escape
        or not finite(nav.marker_size_m) or nav.marker_size_m <= 0 or not vector(nav.position) or not vector(data.position_m)
        or not finite(nav.heading_error_deg) or not finite(nav.distance_2d_m) then
        self.taxiReentry = nil
        return
    end
    local recovery = self.taxiReentry
    if recovery and recovery.waypoint ~= self.waypoint then recovery, self.taxiReentry = nil, nil end
    local size, error, dist = nav.marker_size_m, nav.heading_error_deg, nav.distance_2d_m
    if not recovery then
        if math.abs(error) > 95 and dist < 160 and data.speed_kmh < 55 and nav.marker_inside ~= true then
            recovery = {stage = "backin", since = now, waypoint = self.waypoint, reason = "marker_behind",
                timeout = math.max(15000, (dist - size + 10) * 1000 / 3 + 5000)}
        elseif nav.marker_inside == true and data.speed_kmh < 8 then
            self.insideSince = self.insideSince or now
            if elapsed(now, self.insideSince) >= 1500 then
                recovery = {stage = "exit", since = now, waypoint = self.waypoint, reason = "appeared_inside"}
            end
        else
            self.insideSince = nil
        end
        self.taxiReentry = recovery
        if not recovery then return end
    end
    recovery.inside = nav.marker_inside
    if recovery.stage == "backin" then
        -- Tail toward the centre; done once the circle registers, or the nose can take it forward.
        if nav.marker_inside == true then
            recovery.stage, recovery.since = "ack", now
        elseif elapsed(now, recovery.since) > recovery.timeout then
            -- A timeout is not evidence that a forward loop is possible. Hold for a checked retry.
            recovery.stage, recovery.since = "blocked", now
        elseif math.abs(error) < 45 and dist > size * 0.6 then
            self.taxiReentry = nil
            return
        else
            -- Brake first if still rolling forward from the overshoot, then reverse; the pace rises with
            -- distance so a 120 m back-up takes ~20 s, not 40, and stays under the 22 km/h reverse max.
            if finite(data.velocity_body_rfu_mps and data.velocity_body_rfu_mps[2]) and data.velocity_body_rfu_mps[2] > 2 then
                return {direction = -1, steering = angle(error - 180), speed = 0, brake = 1, recovery = recovery}
            end
            return {direction = -1, steering = angle(error - 180), speed = clamp(6 + dist * 0.4, 8, 22), recovery = recovery}
        end
    end
    if recovery.stage == "ack" then
        -- Entry was observed: allow the server to advance before moving out again.
        if elapsed(now, recovery.since) < 3000 then
            return {direction = -1, steering = 0, speed = 0, recovery = recovery}
        end
        recovery.stage, recovery.since = "exit", now
    end
    if recovery.stage == "blocked" then
        self.detail.recovery_alarm = "reverse_timeout"
        return {direction = -1, steering = 0, speed = 0, recovery = recovery}
    end
    if recovery.stage == "exit" then
        -- Straight back until the aircraft is clear of the circle by 4 m.
        if (nav.marker_inside ~= true and dist > size + 4) or elapsed(now, recovery.since) > 10000 then
            recovery.stage, recovery.since = "return", now
        else
            return {direction = -1, steering = 0, speed = 15, recovery = recovery}
        end
    end
    if recovery.stage == "return" then
        -- Forward into the centre; the waypoint change (recorded by the collector) ends it.
        if elapsed(now, recovery.since) > 15000 then self.taxiReentry = nil; return end
        return {direction = 1, steering = error, speed = math.abs(error) > 40 and 10 or 18, recovery = recovery}
    end
    self.taxiReentry = nil
end

function Controller:updateGroundProgress(data, now)
    local speed = data.horizontal_speed_kmh
    if data.on_ground ~= true or data.frozen == true or not vector(data.position_m)
        or not finite(speed) or speed < 5 then self.groundProgress = nil; return end
    local p = self.groundProgress
    local dt = p and elapsed(now, p.tick) / 1000 or 0
    if not p or dt > 1 then
        self.groundProgress = {tick = now, position = data.position_m, speed = speed,
            began = now, travel = 0, expected = 0, ratio = 1}
        return
    end
    if dt <= 0 then return end
    local dx, dy = data.position_m[1] - p.position[1], data.position_m[2] - p.position[2]
    p.travel = p.travel + math.sqrt(dx * dx + dy * dy)
    p.expected = p.expected + (speed + p.speed) / 7.2 * dt
    p.tick, p.position, p.speed = now, data.position_m, speed
    local span = elapsed(now, p.began)
    if span >= 1000 and p.expected > 1 then
        p.measuredRatio, p.wallSpeed = p.travel / p.expected, p.travel * 3600 / span
        local ratio = clamp(p.measuredRatio, 0.35, 1)
        p.ratio = p.ratio + (ratio - p.ratio) * (ratio < p.ratio and 0.7 or 0.25)
        p.windowMs, p.observed = span, now
        p.began, p.travel, p.expected = now, 0, 0
    end
end

function Controller:rolloutPlan(data)
    local nav, timer = data.navigation, data.waypoint_timer
    local forward = data.velocity_body_rfu_mps and data.velocity_body_rfu_mps[2]
    if not self.flightSeen or data.on_ground ~= true or data.frozen == true
        or self.instruction == "reverse" or self.instruction == "passengers" or self.instruction == "wait_clearance"
        or not finite(forward) or forward < 0 or type(nav) ~= "table" or nav.ambiguous
        or nav.marker_type ~= "ring" or nav.marker_inside == true
        or not vector(nav.position) or not vector(nav.next_position)
        or not finite(nav.distance_2d_m) or not finite(nav.marker_size_m) or nav.marker_size_m <= 0 or nav.marker_size_m > 60
        or not finite(nav.heading_error_deg) or math.abs(nav.heading_error_deg) > 5
        or not finite(nav.altitude_error_m) or math.abs(nav.altitude_error_m) > 5
        or not finite(data.agl_terrain_m) or data.agl_terrain_m > 3
        or math.abs(data.roll_deg or 0) > 5 or math.abs(data.heading_rate_dps or 0) > 3
        or not finite(nav.bearing_deg) or type(timer) ~= "table" or not finite(timer.remaining_s)
        or timer.remaining_s <= 0 or not finite(timer.age_ms) or timer.age_ms > 2500 then return end
    local dx, dy = nav.next_position[1] - nav.position[1], nav.next_position[2] - nav.position[2]
    if dx * dx + dy * dy < 25 then return end
    local nextTurn = math.abs(angle(math.deg(math.atan2(dx, dy)) - nav.bearing_deg))
    local entry = nextTurn > 35 and 12 or nextTurn > 15 and 18 or nextTurn > 5 and 26 or 35
    local remaining = math.max(0, nav.distance_2d_m - nav.marker_size_m + 2)
    local ratio = self.groundProgress and self.groundProgress.ratio or 1
    -- Five seconds for timing variation, plus time lost slowing from the maximum rollout speed.
    local reserve = 5 + (55 - entry)^2 / (2 * 1.8 * 55 * 3.6)
    local required = remaining * 3.6 / (math.max(1, timer.remaining_s - reserve) * ratio)
    local brakingDistance = math.max(0, nav.distance_2d_m - nav.marker_size_m - data.speed_kmh / 3.6 * 0.5 - 5)
    local cap = math.min(55, math.sqrt((entry / 3.6)^2 + 2 * 1.8 * brakingDistance) * 3.6)
    local speed = math.min(cap, math.max(35, required))
    return {speed = speed, cap = cap, required = required, entry = entry, remaining = remaining,
        seconds = timer.remaining_s, reserve = reserve, ratio = ratio, nextTurn = nextTurn,
        margin = timer.remaining_s - remaining * 3.6 / math.max(1, speed * ratio), limited = required > cap}
end

-- Aggressive taxi braking planned over the whole remembered chain: speed is spent only where a
-- corner or the stop demands it, never "just in case" at every marker. Entry speed by the turn at
-- each point (<= 5 deg free, then 45/32/22/16 km/h), braking planned at 3.0 m/s^2 (5.4 measured).
-- A permit point (where the server said "Взлёт разрешён" last time) ends the plan: the server
-- freezes the aircraft there itself, at any speed.
function Controller:taxiChainLimit(nav, data, free)
    if not vector(nav.position) or not vector(data.position_m) then return free end
    local points, sizes, flags = {nav.position}, {nav.marker_size_m}, {{}}
    if type(nav.chain_ahead) == "table" then
        for _, m in ipairs(nav.chain_ahead) do
            if not vector(m.position) then break end
            points[#points + 1], sizes[#sizes + 1] = m.position, m.size_m
            flags[#flags + 1] = {yellow = m.yellow == true, permit = m.permit == true, unknown = m.unknown == true}
        end
    end
    local limit, response = free, data.speed_kmh / 3.6 * 0.35
    local d = math.sqrt((points[1][1] - data.position_m[1])^2 + (points[1][2] - data.position_m[2])^2)
    for i = 1, #points do
        if i > 1 then d = d + math.sqrt((points[i][1] - points[i - 1][1])^2 + (points[i][2] - points[i - 1][2])^2) end
        if flags[i].permit then break end
        local size = finite(sizes[i]) and sizes[i] > 0 and sizes[i] or 30
        local remaining = math.max(0, d - size - response)
        local entry
        if flags[i].yellow or flags[i].unknown then entry = 16
        else
            local A, B, C = i == 1 and self.previousNavPosition or points[i - 1], points[i], points[i + 1]
            if A and C and vector(A) then
                local legIn, legOut = (B[1] - A[1])^2 + (B[2] - A[2])^2, (C[1] - B[1])^2 + (C[2] - B[2])^2
                if legIn > 1 and legOut > 1 then
                    local turn = math.abs(angle(math.deg(math.atan2(C[1] - B[1], C[2] - B[2]) - math.atan2(B[1] - A[1], B[2] - A[2]))))
                    entry = turn > 60 and 22 or turn > 35 and 30 or turn > 15 and 40 or turn > 5 and 55 or nil
                end
            elseif not C then entry = 26 end  -- the chain ends here with nothing known beyond
        end
        if entry then limit = math.min(limit, math.sqrt((entry / 3.6)^2 + 2 * 3.0 * remaining) * 3.6) end
    end
    return limit
end

-- A lesson for the aim memory: this marker's aim moves `offset` metres to the side `side`
-- (+1 right of the heading) of the aircraft's current heading; the previous marker is named so a
-- saturated aim can pass the shift back along the approach line.
function Controller:aimLesson(nav, data, side, offset, reason)
    if type(nav) ~= "table" or nav.marker_type ~= "checkpoint" or not vector(nav.position) or not finite(data.heading_deg) then return end
    local h = math.rad(data.heading_deg)
    local base = self.lastAim and self.lastAim.waypoint == self.waypoint and self.lastAim or nil
    local previous = vector(self.previousNavPosition) and {self.previousNavPosition[1], self.previousNavPosition[2], self.previousNavPosition[3]} or nil
    self.detail.aim_learn = {nav.position[1], nav.position[2], nav.position[3], size = nav.marker_size_m,
        dx = side * offset * math.cos(h), dy = -side * offset * math.sin(h),
        base = base and {base[1], base[2]} or nil, previous = previous, reason = reason,
        hit = vector(data.position_m) and {data.position_m[1], data.position_m[2]} or nil,
        distance = finite(nav.distance_2d_m) and nav.distance_2d_m or nil}
    -- Past the place of the hit the retry creeps (12 km/h within 40 m of it).
    if vector(data.position_m) then self.creepPoint = {data.position_m[1], data.position_m[2], waypoint = self.waypoint} end
    self.detail.no_cut_marker = {nav.position[1], nav.position[2], reason = reason}
end

-- Swept component geometry, kept pure so collision witnesses can be replayed outside MTA.
function Controller:probeGeometry(item, intent, box, motion, simple)
    local basis, position = item.basis_world_rfu, item.position_m
    local speed, direction = item.speed_kmh / 3.6, intent.direction
    local horizon = math.min(100, math.max(8, speed * speed / 6 + speed * 0.4 + 3))
    local reaction = math.min(horizon, speed * 0.35 + 0.1)
    if motion ~= direction then reaction = math.min(horizon, reaction + speed * speed / 6) end
    local travel = math.max(2, horizon - reaction)
    -- The old 15-degree total cap omitted the outside wing during sustained taxi turns.
    local yaw = math.rad(clamp(intent.yaw * travel / math.max(2, speed), -80, 80))
    local remainingTurn = math.rad(math.abs(intent.steering))
    yaw = clamp(yaw, -remainingTurn, remainingTurn)
    local turnTravel = math.abs(intent.yaw) > 0.01
        and math.min(travel, math.max(2, speed) * math.abs(math.deg(yaw) / intent.yaw)) or travel
    local heading = math.rad(item.heading_deg)
    local right, ahead = {math.cos(heading), -math.sin(heading)}, {math.sin(heading), math.cos(heading)}
    local contactHeight = math.max(1.2, box[3] * 0.65)
    local wingHeight = box[3]
    -- Evidence ceiling: recorded contacts span -0.55 .. 4.79 m. A ray above that only finds jet
    -- bridges and canopies the aircraft passes under (recorded false stop at 8.29 m on the stand).
    local upperHeight = math.max(3, math.min(box[3] + 1.3, box[3] + (box[6] - box[3]) * 0.35))
    local offsets = {}
    local function offset(x,y,z,part) offsets[#offsets+1] = {x,y,z,part} end
    for _, z in ipairs({0.6, contactHeight, upperHeight}) do
        offset(0, box[5] + 2.25, z, "nose")
        offset(0, box[2] + 1.75, z, "tail")
    end
    -- Recorded ramp contacts on model 519: local x=6.13..6.17, y=4.73..4.75, z=-0.38..-0.33.
    -- Bounding-box minZ is the wing, NOT the bottom of the nacelles.
    for _, side in ipairs({-1,1}) do
        local part = side < 0 and "left_engine" or "right_engine"
        for _, x in ipairs({5.4,6.15,6.9}) do
            for _, z in ipairs({-0.55,0.25,contactHeight}) do offset(side*x,5.4,z,part) end
        end
        part = side < 0 and "left_wing" or "right_wing"
        local span = math.abs(side < 0 and box[1] or box[4])
        -- Always retain both wings, including on familiar taxiways. Sample the leading edge;
        -- transverse rays below also cover posts between the old 0.33/0.66/1 span fractions.
        for i = 0, math.ceil(span-5.5) do
            local x = math.min(span,5.5+i)
            for _, z in ipairs({wingHeight-0.65,wingHeight+0.9}) do offset(side*x,2.6,z,part) end
        end
    end
    -- The 15.9 m fin ray is removed: it sees overhead structure at the stand and no contact above
    -- 4.79 m has ever been recorded. The tail is already swept at upperHeight by the loop above.
    local function body(x,y,z)
        return {basis[1][1]*x+basis[2][1]*y+basis[3][1]*z,
            basis[1][2]*x+basis[2][2]*y+basis[3][2]*z,
            basis[1][3]*x+basis[2][3]*y+basis[3][3]*z}
    end
    local function point(offset,dx,dy,turn)
        local c,s=math.cos(turn),math.sin(turn)
        return {position[1]+right[1]*dx+ahead[1]*dy+c*offset[1]+s*offset[2],
            position[2]+right[2]*dx+ahead[2]*dy-s*offset[1]+c*offset[2],position[3]+offset[3]}
    end
    local work={rays={},horizon=horizon,reaction=reaction,yaw=math.deg(yaw),direction=direction,motion=motion,
        bounds=box,heights={body_low=0.6,engine_low=-0.55,body_contact=contactHeight,wing=wingHeight,upper=upperHeight}}
    local function add(a,b,part,kind) work.rays[#work.rays+1]={from=a,to=b,part=part,kind=kind} end
    local function pose(f)
        local distance=travel*f
        local arc=math.min(distance,turnTravel)
        local turn=turnTravel>0.0001 and yaw*arc/turnTravel or 0
        local tail=math.max(0,distance-turnTravel)
        return direction*((math.abs(turn)>0.0001 and arc*(1-math.cos(turn))/turn or 0)+tail*math.sin(turn)),
            reaction*motion+direction*((math.abs(turn)>0.0001 and arc*math.sin(turn)/turn or arc)+tail*math.cos(turn)),turn
    end
    -- 7 steps meant 672 rays and a sweep that could never finish in its lifetime (recorded).
    -- 3 steps is 336 rays, the same count as the recorded terminal witness at yaw -14.6.
    local steps = simple and 1 or math.max(1, math.min(3, math.ceil(math.abs(work.yaw)/5)))
    for _,o in ipairs(offsets) do
        local rel=body(o[1],o[2],o[3])
        local a,b=point(rel,0,0,0),point(rel,0,reaction*motion,0)
        add(a,b,o[4],"reaction"); a=b
        for step=1,steps do
            local dx,dy,turn=pose(step/steps)
            b=point(rel,dx,dy,turn)
            add(a,b,o[4],steps>1 and "turn_sweep" or "planned");a=b
        end
    end
    -- Cross-span checks cover the surface already beside the aircraft and the future swept width.
    -- Both edges are checked so a post does not fall behind a leading-edge ray at a slow start.
    for step=0,steps do
        local dx,dy,turn=0,0,0
        if step>0 then dx,dy,turn=pose(step/steps) end
        for _,side in ipairs({-1,1}) do
            local span=math.abs(side<0 and box[1] or box[4])
            for _,y in ipairs({-1.2,2.6}) do
                for _,z in ipairs({wingHeight-0.65,wingHeight+0.9}) do
                    add(point(body(side*5.5,y,z),dx,dy,turn),point(body(side*span,y,z),dx,dy,turn),
                        side<0 and "left_wing" or "right_wing","wing_span")
                end
            end
        end
    end
    return work
end

function Controller:groundIntent(data, now)
    -- Called by the obstacle probe too, before the first update of a session: detail may not exist yet.
    self.detail = self.detail or {}
    local nav = data.navigation
    local error = type(nav) == "table" and finite(nav.heading_error_deg) and nav.heading_error_deg or 0
    local c = type(nav) == "table" and nav.color_rgba or {}
    if data.on_ground == true and self.instruction ~= "reverse" and nav and nav.marker_type == "checkpoint"
        and nav.marker_inside == true and finite(c[1]) and finite(c[2]) and finite(c[3])
        and c[1] > 200 and c[2] > 160 and c[3] < 80 then
        self.escape = nil
        return {direction = 1, steering = 0, speed = 0, yaw = 0}
    end
    local rollout = self:rolloutPlan(data)
    -- Obstacle escape: a wall does not move, so after a hold at standstill the aircraft backs straight
    -- out a few metres and takes the turn again from farther away (recorded: a wing sweep 3 m short of
    -- the terminal held the taxi for 20 s until the pilot took over).
    if not self.escape and data.on_ground == true and finite(data.ground_bump_ms) and data.ground_bump_ms < 600
        and data.speed_kmh < 25 and (self.escapeCount or 0) < 5 and vector(data.position_m)
        and (not self.lastBumpEscape or elapsed(now, self.lastBumpEscape) > 4000) then
        self.escapeCount, self.lastBumpEscape = (self.escapeCount or 0) + 1, now
        if data.ground_bump_side == 1 or data.ground_bump_side == -1 then
            self.avoidSide, self.avoidOffset, self.avoidWaypoint = -data.ground_bump_side, 10, self.waypoint
        end
        self.escape = {since = now, start = {data.position_m[1], data.position_m[2]},
            distance = (self.avoidOffset or 10) >= 10 and 16 or 12}
        self.detail.bump_escape = true
        self:aimLesson(nav, data, self.avoidSide or -1, self.avoidOffset or 10, "bump")
    end
    if self.escape then
        local e = self.escape
        local moved = vector(data.position_m) and math.sqrt((data.position_m[1] - e.start[1])^2 + (data.position_m[2] - e.start[2])^2) or 0
        -- Back out with the nose turning toward the marker (pushback-style), so the next attempt starts
        -- already pointed away from the obstacle; done after 10 m, or 5 m once the nose is within 25 deg.
        -- The full distance is the run-up the sidestep needs (a 10 m step at taxi turning radius
        -- takes ~16 m). But the marker clock outranks it: backing 16 m away from a marker that
        -- expires in 8 s is exactly how the recorded firing happened.
        local etimer = data.waypoint_timer
        local rush = type(etimer) == "table" and finite(etimer.remaining_s) and finite(etimer.age_ms)
            and etimer.age_ms < 3000 and etimer.remaining_s < 15 or false
        local need = rush and math.min(e.distance, 6) or e.distance
        self.detail.escape_rush = rush
        if (moved >= need and not self.blocked) or moved >= need + 8 or elapsed(now, e.since) > 10000 then
            self.escape, self.blocked, self.clearSince = nil, false, nil
        else
            return {direction = -1, steering = error, speed = clamp(10 + (need - moved) * 0.6, rush and 18 or 10, 22),
                yaw = clamp(error * 0.7, -8, 8), align = true, escape = true, escape_moved_m = moved}
        end
    end
    -- After a touchdown short of the yellow marker the brakes stay in full down to 35 km/h, whatever
    -- the mode; the fast roll-through applies to the red chain only.
    local mc = type(nav) == "table" and nav.color_rgba or {}
    local yellowNav = finite(mc[1]) and finite(mc[2]) and finite(mc[3]) and mc[1] > 200 and mc[2] > 160 and mc[3] < 80
    local sharpTurnoff = type(nav) == "table" and nav.marker_type == "checkpoint" and not yellowNav
        and finite(nav.heading_error_deg) and math.abs(nav.heading_error_deg) > 70
        and finite(nav.distance_2d_m) and nav.distance_2d_m < 110
    local rolloutFloor = rollout and rollout.speed + 1 or sharpTurnoff and 35 or not yellowNav and (self.routeKnown and 60 or 46) or 35
    if self.finalLanding and self.flightSeen and data.on_ground == true
        and data.speed_kmh > rolloutFloor and self.landingHeading then
        local steering = angle(self.landingHeading - data.heading_deg)
        local rate = self.filteredRates and self.filteredRates.yaw or 0
        local rudder = clamp((clamp(steering * 0.8, -3, 3) - rate * 0.3) / 8.8, -0.3, 0.3)
        return {direction = 1, steering = steering, speed = rollout and rollout.speed or 0, yaw = 8.8 * rudder,
            rudder = rudder, landing_rollout = true, rollout = rollout}
    end
    -- A red checkpoint is a 30 m circle, not a ring: on the ground the aim point sits inside it,
    -- shifted 0.6 R toward the next marker, so corners are cut and the taxi line stays straight.
    -- The yellow marker (parking) and the server's stop keep the exact centre.
    local mc = type(nav) == "table" and nav.color_rgba or {}
    local yellowAim = finite(mc[1]) and finite(mc[2]) and finite(mc[3]) and mc[1] > 200 and mc[2] > 160 and mc[3] < 80
    if type(nav) == "table" and nav.marker_type == "checkpoint" and not yellowAim and vector(nav.position) and vector(nav.next_position)
        and vector(data.position_m) and finite(nav.marker_size_m) and finite(nav.distance_2d_m) and nav.distance_2d_m > nav.marker_size_m
        and finite(data.heading_deg) and self.instruction ~= "reverse" and not self.pushback and not self.pushbackPending then
        local dx, dy = nav.next_position[1] - nav.position[1], nav.next_position[2] - nav.position[2]
        local len = math.sqrt(dx * dx + dy * dy)
        local ax, ay
        local learnedAim = type(nav.aim_offset) == "table" and finite(nav.aim_offset[1]) and finite(nav.aim_offset[2])
        local gplan = type(nav.chain_ahead) == "table" and #nav.chain_ahead > 0 and (learnedAim or self.avoidWaypoint ~= self.waypoint)
            and self:chainPlan(nav, data, 0.35) or nil
        if gplan and gplan.points and vector(gplan.points[1]) then
            local p1, p2 = gplan.points[1], gplan.points[2]
            ax, ay = p1[1], p1[2]
            local d1 = math.max(1, math.sqrt((p1[1] - data.position_m[1])^2 + (p1[2] - data.position_m[2])^2))
            local lookahead = clamp(data.speed_kmh / 3.6 * 1.2, 20, 60)
            if vector(p2) and d1 < lookahead then
                local ex, ey = p2[1] - p1[1], p2[2] - p1[2]
                local leg = math.max(1, math.sqrt(ex * ex + ey * ey))
                local t = clamp((lookahead - d1) / leg, 0, 1)
                local sx, sy = p1[1] + ex * t, p1[2] + ey * t
                local offCentre = nav.distance_2d_m * math.abs(math.sin(math.rad(angle(
                    math.deg(math.atan2(sx - data.position_m[1], sy - data.position_m[2])) - nav.bearing_deg))))
                if offCentre <= nav.marker_size_m * 0.7 then ax, ay = sx, sy end
            end
            self.detail.plan_points = gplan.points
        elseif learnedAim then
            ax, ay = nav.position[1] + nav.aim_offset[1], nav.position[2] + nav.aim_offset[2]
            -- VIA detours disabled: see chainPlan. The terrain map replaces this guess.
        elseif len > 1 then
            local shift = nav.marker_size_m * 0.6
            ax, ay = nav.position[1] + dx / len * shift, nav.position[2] + dy / len * shift
        end
        if ax then
            error = angle(math.deg(math.atan2(ax - data.position_m[1], ay - data.position_m[2])) - data.heading_deg)
            self.detail.aim_point = {ax, ay}
            self.lastAim = {ax, ay, waypoint = self.waypoint}
        end
    end
    if type(nav) == "table" and nav.marker_type == "checkpoint" and yellowAim and vector(nav.position) and vector(data.position_m)
        and finite(nav.marker_size_m) and finite(nav.bearing_deg) and finite(data.heading_deg) and self.instruction ~= "reverse" then
        if not self.parkingTarget or self.parkingTarget.waypoint ~= self.waypoint then
            local b, size = math.rad(nav.bearing_deg), nav.marker_size_m
            self.parkingTarget = {waypoint = self.waypoint,
                x = nav.position[1], y = nav.position[2]}
        end
        local t = self.parkingTarget
        if type(nav.aim_offset) == "table" and finite(nav.aim_offset[1]) and finite(nav.aim_offset[2]) then
            t.x, t.y, t.learned = nav.position[1] + nav.aim_offset[1], nav.position[2] + nav.aim_offset[2], true
        end
        local dist = math.sqrt((t.x - data.position_m[1])^2 + (t.y - data.position_m[2])^2)
        if dist > 4 then error = angle(math.deg(math.atan2(t.x - data.position_m[1], t.y - data.position_m[2])) - data.heading_deg) end
        self.detail.parking_target_m, self.detail.aim_point = dist, {t.x, t.y}
        self.lastAim = {t.x, t.y, waypoint = self.waypoint}
    end
    local reentry = self:taxiReentryIntent(data, now)
    local pending = not reentry and self.pushbackPending and elapsed(now, self.pushbackNoticeTick) < 250
    local align = not reentry and self.instruction == "taxi" and not pending
        -- Align in reverse until the nose is nearly on the marker (recorded by hand: the pilot backs
        -- out turning until straight, then drives; at 50 deg the bot drove off at an angle past the gate).
        and (self.pushback and math.abs(error) > 28 or self.pushbackPending and math.abs(error) > 32)
    local reverse = self.instruction == "reverse" or align or pending
    -- On the new taxi leg, align the nose while still rolling backwards.
    if self.avoidSide and self.avoidWaypoint == self.waypoint and type(nav) == "table" and finite(nav.distance_2d_m) then
        -- Sidestep: aim 8 m beside the marker centre, away from the obstacle, until this marker is passed
        -- (only while the collector's aim memory has not taken the marker over).
        if not (type(nav.aim_offset) == "table" and finite(nav.aim_offset[1])) then
            error = angle(error + self.avoidSide * math.deg(math.atan2(self.avoidOffset or 8, math.max(15, nav.distance_2d_m))))
        end
    elseif self.avoidSide and self.avoidWaypoint ~= self.waypoint then
        self.avoidSide = nil
    end
    local steering = reverse and not align and angle(error - 180) or error
    -- Corners are rolled through at 9-17 km/h: recorded at 4-7 km/h a 90-degree taxi corner took
    -- 19 s of the marker's 30-s timer.
    -- Reverse 22 here is 16 on the server speedometer (x127/180), as the pilot backs out.
    local speed = pending and 0 or align and 20 or reverse and 22 or math.abs(steering) > 60 and 9
        or math.abs(steering) > 35 and 13 or math.abs(steering) > 15 and 17 or math.abs(steering) > 5 and 22 or 26
    -- Aggressive taxi: the recorded pilot rolls between ground markers at 50-60 km/h; a per-marker
    -- jitter keeps the pace from looking machine-made.
    -- Ground pace is the same in both modes (the pilot's call): the aggression switch decides the
    -- air only. Corners, the stop and the gate zone are still planned by the chain law.
    local brisk = not reverse and not align and not pending
    local jitter = (self.waypoint * 7919) % 11 / 10
    if brisk then
        -- Aggressive taxi: 95-105 here is 67-74 on the server speedometer (x127/180), wheels on the
        -- ground; small heading offsets between markers are steered at pace, corners come from the plan.
        local gateZone = self.departWaypoint and (self.waypoint - self.departWaypoint) < 3
        speed = math.abs(steering) > 60 and 14 or math.abs(steering) > 35 and 22 or math.abs(steering) > 20 and 34
            or math.abs(steering) > 12 and 48 or not self.routeKnown and 46 or gateZone and 70 + jitter * 5 or 95 + jitter * 10
        -- A block the bot has proven it can take faster carries its own multiplier. Physical
        -- limits below still clamp it.
        if finite(nav.study_pace) and nav.study_pace > 1 then speed = speed * nav.study_pace end
        -- Time budget: the server allows 30 s per marker and its clock is in the samples. When the
        -- cautious pace cannot make the marker in the time left (a long first-visit leg, a gate-zone
        -- crawl), the pace rises to what the clock needs, four seconds in hand; corners and the stop
        -- are still planned by the chain law below, and a real turn keeps its own pace.
        local cp = self.creepPoint
        local creeping = cp and cp.waypoint == self.waypoint and vector(data.position_m)
            and (cp[1] - data.position_m[1])^2 + (cp[2] - data.position_m[2])^2 < 40 * 40 or false
        if creeping then
            speed = math.min(speed, 12)
            self.detail.creep_after_hit = true
        end
        local timer = data.waypoint_timer
        if math.abs(steering) <= 35 and type(timer) == "table" and finite(timer.remaining_s) and finite(timer.age_ms)
            and timer.age_ms < 3000 and type(nav) == "table" and nav.marker_type == "checkpoint"
            and finite(nav.distance_2d_m) and finite(nav.marker_size_m) then
            local required = math.max(0, nav.distance_2d_m - nav.marker_size_m * 0.5) / math.max(1, timer.remaining_s - 4) * 3.6
            self.detail.time_budget_required_kmh = required
            if required > speed then
                speed = math.min(105, required * 1.15)
                -- Being careful past an obstacle is worth nothing if the marker expires meanwhile.
                if creeping then self.detail.creep_overridden_by_clock = true end
            end
        end
    end
    if reentry then reverse, steering, speed = reentry.direction < 0, reentry.steering, reentry.speed end
    local entryLimit
    if not reentry and not reverse and math.abs(steering) <= (brisk and 12 or 5) then
        if not brisk then speed = 50 end  -- 35 on the server speedometer (x127/180)
        if rollout then speed = brisk and math.max(rollout.speed, speed) or rollout.speed end
        if type(nav) == "table" and nav.marker_type == "checkpoint" and finite(nav.marker_size_m)
            and nav.marker_size_m > 0 and finite(nav.distance_2d_m) then
            if brisk then
                entryLimit = self:taxiChainLimit(nav, data, speed)
            else
                local remaining = math.max(0, nav.distance_2d_m - nav.marker_size_m - data.speed_kmh / 3.6 * 0.35)
                entryLimit = math.sqrt((18 / 3.6)^2 + 2 * 1.8 * remaining) * 3.6
            end
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
        -- Only a gross misalignment stops the roll; up to 10 deg is steered out with the rudder while
        -- accelerating (recorded: the pilot rolls at once, the bot spent 7 s aligning to 2.5 deg).
        runwayAlign = not self.takeoffRolling and data.speed_kmh < 70 and (math.abs(error) > 10 or math.abs(rate) > 5)
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
        marker_entry_speed_limit = entryLimit, held_rudder = heldRudder, predicted_steering_error = predicted,
        rollout = rollout}
end

-- Synthetic target for flight without a marker. The first two seconds keep the current heading and
-- height (markers blink while the server swaps them); after that the aircraft circles near the route
-- at a safe height. On final it stays on the landing heading so the touchdown still completes.
function Controller:holdNavigation(data, hold, seconds)
    if not vector(data.position_m) or not finite(data.heading_deg) then return nil end
    local heading, altitude = data.heading_deg, data.position_m[3]
    local ground = finite(data.surface and data.surface.ground_z_m) and data.surface.ground_z_m or nil
    local bearing, target, kind = heading, altitude, "hold"
    if hold == "final_no_marker" then
        bearing, target, kind = self.landingHeading or heading, ground or altitude, "checkpoint"
    elseif seconds > 2 then
        bearing, target = (heading + 30) % 360, math.max(altitude, ground and ground + 150 or altitude)
    end
    local rad, reach = math.rad(bearing), 600
    return {virtual = true, id = "hold", kind = "virtual", marker_type = kind, bearing_deg = bearing,
        position = {data.position_m[1] + math.sin(rad) * reach, data.position_m[2] + math.cos(rad) * reach, target},
        distance_2d_m = reach, distance_3d_m = math.sqrt(reach * reach + (target - altitude)^2),
        altitude_error_m = target - altitude, heading_error_deg = angle(bearing - heading)}
end

-- Airborne, the controller never hands the aircraft to nobody: a blind tick repeats the last command,
-- raises the alarm through detail.airborne_hold and leaves the decision to the pilot (any flight key).
function Controller:airborneHold(reason, text)
    self.detail = self.detail or {}
    self.detail.airborne_hold, self.detail.airborne_hold_text = reason, text
    self.holdSince = self.holdSince or self.lastTick
    self.phase, self.status = "hold_" .. reason, text
    return self.output
end

-- A delayed marker update is not a missed ring. Prove capture from a short observed motion
-- segment (a 13.5 m sphere is wholly inside the ring corridor), then use the advertised next
-- position until the live marker catches up. Never invent another step beyond that arrow.
function Controller:latencyNavigation(data, nav, now, resync)
    local function same(a, b)
        return vector(a) and vector(b) and math.abs(a[1] - b[1]) < 0.25
            and math.abs(a[2] - b[2]) < 0.25 and math.abs(a[3] - b[3]) < 0.25
    end
    local function hit(a, b, centre, radius, flat)
        if not vector(a) or not vector(b) then return false end
        local dx, dy, dz = b[1] - a[1], b[2] - a[2], b[3] - a[3]
        if flat then dz = 0 end
        local length2 = dx * dx + dy * dy + dz * dz
        local u = length2 > 0 and clamp(((centre[1] - a[1]) * dx + (centre[2] - a[2]) * dy
            + (centre[3] - a[3]) * dz) / length2, 0, 1) or 0
        return (a[1] + u * dx - centre[1])^2 + (a[2] + u * dy - centre[2])^2
            + (flat and 0 or (a[3] + u * dz - centre[3])^2) <= radius^2
    end
    local p, obs = self.pendingCapture, self.captureObservation
    if not self.airborne or self.detail.timing_resync_reason == "position_jump" then
        self.captureObservation = nil
        self.pendingCapture = nil
        return nav
    end
    if resync then self.captureObservation, obs = nil, nil end
    if p and nav and not same(nav.position, p.source) then
        self.detail.marker_ack_ms = elapsed(now, p.since)
        -- The server is authoritative, including a route change or a different target type.
        self.pendingCapture, p = nil, nil
    end
    local color = nav and nav.color_rgba or {}
    local red = nav and nav.marker_type == "checkpoint" and self.aggressiveNow and self.landing
        and finite(color[2]) and color[2] < 80
    if not p and nav and (nav.marker_type == "ring" or red) and not nav.ambiguous and vector(nav.position)
        and vector(data.position_m) and vector(nav.next_position) and not same(nav.position, nav.next_position)
        and obs and same(obs.target, nav.position) and elapsed(now, obs.tick) <= 300
        and hit(obs.position, data.position_m, nav.position, (finite(nav.marker_size_m) and nav.marker_size_m or 30) * (red and 0.9 or 0.45), red) then
        local ahead = type(nav.chain_ahead) == "table" and nav.chain_ahead or {}
        local first = ahead[1]
        if not first or not same(first.position, nav.next_position) then first = nil end
        local rest = {}
        if first then for i = 2, #ahead do rest[#rest + 1] = ahead[i] end end
        p = {source = nav.position, since = now, position = nav.next_position,
            marker_type = (red or first and first.marker_type == "checkpoint") and "checkpoint" or "ring",
            size = first and finite(first.size_m) and first.size_m or nav.marker_size_m or 30,
            yellow = first and first.yellow == true, ahead = rest, chain_source = nav.chain_source,
            id = nav.id, lastPosition = data.position_m, lastTick = now}
        self.pendingCapture, self.ringPass, self.turnback = p, nil, nil
        if nav.marker_type == "ring" then self.finalLanding, self.landingHeading = false, nil end
        self.detail.local_ring_capture = nav.marker_type == "ring"
        self.detail.local_checkpoint_capture = red or false
    end
    if nav and vector(nav.position) and vector(data.position_m) then
        self.captureObservation = {target = nav.position, position = data.position_m, tick = now}
    end
    if not p then return nav end
    self.detail.marker_ack_pending_ms = elapsed(now, p.since)
    self.detail.predicted_target_position = p.position
    if not resync and elapsed(now, p.lastTick) <= 300 and hit(p.lastPosition, data.position_m, p.position,
        p.size * (p.marker_type == "checkpoint" and 0.9 or 0.45), p.marker_type == "checkpoint") then
        p.reached = p.reached or now
    end
    p.lastPosition, p.lastTick = data.position_m, now
    if p.reached then
        -- The one known step is exhausted: retain flight control and wait for live navigation.
        self.detail.airborne_hold = "marker_ack_wait"
        local held = self:holdNavigation(data, "no_marker", elapsed(now, p.reached) / 1000)
        if held then held.awaiting_marker = true end
        return held
    end
    local dx, dy, dz = p.position[1] - data.position_m[1], p.position[2] - data.position_m[2], p.position[3] - data.position_m[3]
    local bearing, distance = math.deg(math.atan2(dx, dy)), math.sqrt(dx * dx + dy * dy)
    return {id = p.id, awaiting_marker = true, position = p.position, marker_type = p.marker_type,
        marker_size_m = p.size, color_rgba = p.yellow and {255, 255, 0, 255} or {255, 0, 0, 255},
        chain_source = p.chain_source, chain_ahead = p.ahead, next_position = p.ahead[1] and p.ahead[1].position,
        bearing_deg = bearing, heading_error_deg = angle(bearing - data.heading_deg),
        track_error_deg = angle(bearing - (finite(data.track_deg) and data.track_deg or data.heading_deg)),
        altitude_error_m = dz, distance_2d_m = distance, distance_3d_m = math.sqrt(distance * distance + dz * dz)}
end

-- Airborne plan over the checkpoint chain ahead. Every corner is a fillet arc between its legs:
-- with the measured turn radius r(v) = v / omega(v), omega = 24 deg/s * (v / 75 m/s)^0.6 at full
-- bank, the arc passes the vertex at r * (1 / cos(turn / 2) - 1), which must stay inside the marker
-- radius, and its tangent points r * tan(turn / 2) must fit both legs. The largest speed that fits is
-- the corner speed; below 100 km/h the corner is flown on the ground, so the wheels go down before it.
function Controller:chainPlan(nav, data, margin)
    local function learned(m)
        local o = m.aim_offset
        if type(o) == "table" and finite(o[1]) and finite(o[2]) and vector(m.position) then
            return {m.position[1] + o[1], m.position[2] + o[2], m.position[3]}
        end
    end
    -- Disabled: a VIA learned from one old contact kept steering around a clear hangar. The
    -- terrain map replaces this guess; the plain aim offset from a real contact still applies.
    local function via()
        return nil
    end
    local function viaUnused(m, current)
        local o = m.aim_offset
        local v = type(o) == "table" and o.via
        if type(v) ~= "table" or not finite(v[1]) or not finite(v[2]) or not vector(m.position) then return nil end
        if current and vector(data.position_m) then
            -- Dropped once the aircraft is past it (closer to the marker than the via is).
            local dv = (v[1] - data.position_m[1])^2 + (v[2] - data.position_m[2])^2
            local dm = (m.position[1] - data.position_m[1])^2 + (m.position[2] - data.position_m[2])^2
            local vm = (m.position[1] - v[1])^2 + (m.position[2] - v[2])^2
            if dv < 64 or dm <= vm then return nil end
        end
        return {v[1], v[2], m.position[3]}
    end
    local points, sizes, yellows, unknownIndex = {}, {}, {}, nil
    margin = margin or 0.75
    local rings, fixedPoints = {}, {}
    local function push(p, size, yellow, fixed, ring)
        points[#points + 1], sizes[#sizes + 1], yellows[#yellows + 1] = p, size, yellow
        fixedPoints[#points], rings[#points] = fixed == true, ring == true
    end
    local v0 = via(nav, true)
    if v0 then push(v0, 10, false, true) end
    push(learned(nav) or nav.position, nav.marker_size_m, false, nav.marker_type == "ring" or nav.direct == true or learned(nav) ~= nil, nav.marker_type == "ring")
    if type(nav.chain_ahead) == "table" then
        for _, m in ipairs(nav.chain_ahead) do
            if not vector(m.position) then break end
            local vm = via(m, false)
            if vm then push(vm, 10, false, true) end
            points[#points + 1], sizes[#sizes + 1], yellows[#yellows + 1] = learned(m) or m.position, m.size_m, m.yellow == true
            fixedPoints[#points] = m.marker_type == "ring" or m.direct == true or learned(m) ~= nil
            rings[#points] = m.marker_type == "ring"
            -- An arrow target of unknown type may be the yellow marker: planned as a possible stop.
            if m.unknown then unknownIndex = #points end
        end
    end
    local entryBefore = {}
    local entryCount = 0
    -- Disabled until the terrain map can vouch for the corner: a guessed point aimed the nose
    -- into a hangar (recorded). Crash memory still applies, it reacts to a real contact.
    if false and margin < 0.5 and #points >= 2 then
        local i = 1
        while i < #points do
            local A = i == 1 and self.previousNavPosition or points[i - 1]
            local B, C = points[i], points[i + 1]
            if vector(A) and vector(B) and vector(C) and not yellows[i] then
                local legIn = math.sqrt((B[1] - A[1])^2 + (B[2] - A[2])^2)
                local legOut = math.sqrt((C[1] - B[1])^2 + (C[2] - B[2])^2)
                if legIn > 12 and legOut > 12 then
                    local turn = math.abs(angle(math.deg(math.atan2(C[1] - B[1], C[2] - B[2])
                        - math.atan2(B[1] - A[1], B[2] - A[2]))))
                    if turn >= 60 and entryCount == 0 then
                        -- On the outgoing line, back from the corner: the aircraft reaches it
                        -- already pointing down the next leg (the pilot's yellow arc).
                        local d = math.min(25, legIn * 0.4, legOut * 0.4)
                        -- A hand never stops on exactly the same line twice: the entry point carries
                        -- a small sideways offset, fixed per marker so it cannot jitter frame to frame.
                        local wob = (((self.waypoint or 0) * 7919 + i * 31) % 13) / 6 - 1
                        local ux, uy = (C[1] - B[1]) / legOut, (C[2] - B[2]) / legOut
                        local ex = B[1] - ux * d - uy * wob * 2.5
                        local ey = B[2] - uy * d + ux * wob * 2.5
                        table.insert(points, i, {ex, ey, B[3]})
                        table.insert(sizes, i, 10)
                        table.insert(yellows, i, false)
                        table.insert(fixedPoints, i, true)
                        table.insert(rings, i, false)
                        if unknownIndex and unknownIndex >= i then unknownIndex = unknownIndex + 1 end
                        entryBefore[i + 1] = true
                        entryCount = entryCount + 1
                        if self.detail then self.detail.entry_points = (self.detail.entry_points or 0) + 1 end
                        i = i + 1
                    end
                end
            end
            i = i + 1
        end
    end
    -- Corridor smoothing: every vertex slides toward the midpoint of its neighbours, staying within
    -- 0.7 of its own marker radius; the last point (the stop) and unknown steps stay put.
    local centres = points
    local previous = self.previousNavPosition
    local cache, reuse = self.airChainPlan, nil
    -- Keep one corridor through successive marker acknowledgements. Re-smoothing a suffix with
    -- the previous *raw centre* moves the remaining aims sideways just as the aircraft enters a
    -- short bend. Reuse only an exactly matching suffix of the same advertised chain.
    if cache and cache.source == nav.chain_source and cache.margin == margin then
        for first = 1, #cache.centres do
            if #cache.centres - first + 1 == #centres then
                local matches = true
                for i, c in ipairs(centres) do
                    local old = cache.centres[first + i - 1]
                    if math.abs(c[1] - old[1]) > 0.25 or math.abs(c[2] - old[2]) > 0.25
                        or math.abs(c[3] - old[3]) > 0.25 or sizes[i] ~= cache.sizes[first + i - 1]
                        or yellows[i] ~= cache.yellows[first + i - 1]
                        or rings[i] ~= cache.rings[first + i - 1]
                        or fixedPoints[i] ~= cache.fixedPoints[first + i - 1] then matches = false; break end
                end
                if matches then reuse = first; break end
            end
        end
    end
    points = {}
    for i, p in ipairs(centres) do
        local q = reuse and cache.points[reuse + i - 1] or p
        points[i] = {q[1], q[2], q[3]}
    end
    if reuse then previous = reuse > 1 and cache.points[reuse - 1] or cache.previous end
    for _ = 1, reuse and 0 or 25 do
        for i = 1, #points - 1 do
            local prev, nxt = i == 1 and previous or points[i - 1], points[i + 1]
            if prev and vector(prev) and not yellows[i] and i ~= unknownIndex and not fixedPoints[i] then
                local m = margin or 0.75
                if m < 0.5 then
                    -- Ground: a taxiway bend keeps the narrow 0.35 R, a reversal onto the runway is
                    -- cut to 0.8 R so the turn is flown early on the inside and the aircraft arrives
                    -- aligned with the next leg instead of swinging its nose past the marker.
                    local turn = math.abs(angle(math.deg(math.atan2(nxt[1] - centres[i][1], nxt[2] - centres[i][2])
                        - math.atan2(centres[i][1] - prev[1], centres[i][2] - prev[2]))))
                    m = entryBefore[i] and 0.35 or clamp(0.35 + (turn - 45) / 75 * 0.45, 0.35, 0.8)
                end
                local r = (finite(sizes[i]) and sizes[i] > 0 and sizes[i] or 30) * m
                local mx, my = (prev[1] + nxt[1]) / 2, (prev[2] + nxt[2]) / 2
                local dx, dy = mx - centres[i][1], my - centres[i][2]
                local dist = math.sqrt(dx * dx + dy * dy)
                if dist > r then dx, dy = dx / dist * r, dy / dist * r end
                points[i][1], points[i][2] = centres[i][1] + dx, centres[i][2] + dy
            end
        end
    end
    if not reuse then
        self.airChainPlan = {source = nav.chain_source, centres = centres, sizes = sizes, yellows = yellows,
            points = points, previous = previous, margin = margin, rings = rings, fixedPoints = fixedPoints}
    end
    local result = {corners = {}, distances = {}, unknownIndex = unknownIndex, points = points}
    local d = math.sqrt((points[1][1] - data.position_m[1])^2 + (points[1][2] - data.position_m[2])^2)
    result.distances[1] = d
    for i = 2, #points do
        d = d + math.sqrt((points[i][1] - points[i - 1][1])^2 + (points[i][2] - points[i - 1][2])^2)
        result.distances[i] = d
    end
    for i = 1, #points do
        if yellows[i] then result.stopIndex = i; break end
        local A, B, C = i == 1 and previous or points[i - 1], points[i], points[i + 1]
        if A and C and vector(A) then
            local legIn = math.sqrt((B[1] - A[1])^2 + (B[2] - A[2])^2)
            local legOut = math.sqrt((C[1] - B[1])^2 + (C[2] - B[2])^2)
            if legIn > 1 and legOut > 1 then
                local turn = math.abs(angle(math.deg(math.atan2(C[1] - B[1], C[2] - B[2]) - math.atan2(B[1] - A[1], B[2] - A[2]))))
                local size = finite(sizes[i]) and sizes[i] or 30
                local speed, start = 240, nil
                if turn >= 8 then
                    local tanHalf = math.tan(math.rad(turn / 2))
                    -- The fillet must fit inside the legs: its tangent length r*tan(turn/2) cannot
                    -- exceed half of the shorter leg, or two arcs overlap and no path tracks them.
                    local rMax = math.min(size / (1 / math.cos(math.rad(turn / 2)) - 1), math.min(legIn, legOut) / (2 * tanHalf))
                    -- Speed that arc allows with the bank the wing clears at 10-15 m (40 deg) plus
                    -- 8 deg/s of rudder: v = (w r + sqrt((w r)^2 + 4 g tan(bank) r)) / 2, w = rad(8).
                    -- (Measured on the Ugryumoch arrival: 41-46 m legs with 76 deg turns are flyable at
                    -- 100-116 km/h through the 30 m circles, and the pilot flies them at 105-126.)
                    local w = math.rad(8)
                    speed = math.min(240, (w * rMax + math.sqrt((w * rMax) ^ 2 + 4 * 9.81 * math.tan(math.rad(40)) * rMax)) / 2 * 3.6)
                    start = rMax * tanHalf
                end
                result.corners[i] = {turn = turn, speed = speed, start = start, distance = result.distances[i], leg_in = legIn, leg_out = legOut, ring = rings[i] == true}
                if speed < 85 and not result.landIndex and not rings[i] then result.landIndex = i end
            end
        end
    end
    return result
end

-- Test the actual curved intercept to the next checkpoint, including roll-in time. A straight
-- line to that point can miss the current cylinder even when the turning arc captures it (and
-- vice versa). Require capture with both slower and faster measured yaw response.
function Controller:checkpointCut(nav, data, aim, bankLimit, track)
    local speed = math.max(20, data.horizontal_speed_kmh and data.horizontal_speed_kmh / 3.6 or data.speed_kmh / 3.6)
    local radius = (finite(nav.marker_size_m) and nav.marker_size_m or 30) - 4
    local worst = 0
    for _, response in ipairs({0.85, 1.15}) do
        local x, y, heading, bank = data.position_m[1], data.position_m[2], track, data.roll_deg
        local closest = math.huge
        for _ = 1, 60 do
            local dx, dy = aim[1] - x, aim[2] - y
            local distance = math.sqrt(dx * dx + dy * dy)
            local err = angle(math.deg(math.atan2(dx, dy)) - heading)
            local rate = 2 * speed / math.max(40, distance, speed * 1.2) * math.sin(math.rad(clamp(err, -75, 75)))
            local goalBank = clamp(math.deg(math.atan2(speed * rate, 9.81)), -bankLimit, bankLimit)
            bank = bank + clamp(goalBank - bank, -4, 4)
            -- Manual log measurements: about 20 deg/s at 39 deg bank and 28 at 65.
            local yaw = response * 28 * math.sin(math.rad(bank)) / math.sin(math.rad(65))
            heading = heading + yaw * 0.1
            local nx, ny = x + math.sin(math.rad(heading)) * speed * 0.1, y + math.cos(math.rad(heading)) * speed * 0.1
            local sx, sy = nx - x, ny - y
            local u = clamp(((nav.position[1] - x) * sx + (nav.position[2] - y) * sy) / (sx * sx + sy * sy), 0, 1)
            closest = math.min(closest, math.sqrt((x + sx * u - nav.position[1])^2 + (y + sy * u - nav.position[2])^2))
            x, y = nx, ny
            if closest < radius then break end
        end
        worst = math.max(worst, closest)
        if closest >= radius then return false, worst end
    end
    return true, worst
end

function Controller:update(data, now, pathClear, probeStatus)
    if not self.enabled then return self.output end
    local dt = elapsed(now, self.lastTick) / 1000
    self.lastTick = now
    if dt <= 0 then return self.output end
    local frameGap = dt
    local resyncReason
    if dt > 0.3 and data then
        if data.window_minimized == true then resyncReason = "background"
        elseif data.window_restored == true then resyncReason = "restore"
        elseif self.controlReady and dt <= 5 then resyncReason = "short_stall"
        -- Airborne, a pause of any length is resynced: nobody is there to take the aircraft.
        elseif self.airborne then resyncReason = "airborne_stall" end
    end
    local resync = resyncReason ~= nil
    self.detail = self.detail or {}
    -- The aggressive flag acts only on a route learned this session (a chain flown before, by hand or
    -- by the bot); anywhere else the whole flight is the cautious one. Recorded: the flag alone gave
    -- a 255 km/h approach with the cautious landing law, wheels 93 m before a 90-degree corner.
    local navNow = type(data) == "table" and data.navigation or nil
    local source = type(navNow) == "table" and navNow.chain_source or nil
    local learnedSource = type(source) == "string" and source:find("learned_", 1, true) == 1
    if learnedSource then self.routeKnown = true
    -- A ring is a new leg: the departure taxi chain must not carry the aggression into an arrival
    -- whose chain is unknown (recorded at Vice City: 255 km/h through the rings without any plan,
    -- wheels at 144, marker missed). The arrival chain re-arms it through the rings' look-ahead.
    elseif type(navNow) == "table" and navNow.marker_type == "ring" then self.routeKnown = false end
    self.aggressiveNow = (self.aggressive and self.routeKnown == true) or false
    self.detail.frame_gap_ms, self.detail.timing_resynced = frameGap * 1000, false
    self.detail.timing_resync_reason = nil
    if dt > 0.3 and not resync then
        return self:stop(self.controlReady and "Пауза кадров больше 5 с" or "Пауза кадров больше 300 мс при запуске")
    end
    if not data or data.vehicle ~= self.vehicle or data.driver ~= true then return self:stop("Потеря самолёта / места пилота") end
    if tostring(data.dimension) .. ":" .. tostring(data.interior) ~= self.world then return self:stop("Смена мира") end
    if data.blown == true or data.in_water == true or (finite(data.health) and data.health < 300) then return self:stop("Повреждение самолёта / вода") end
    local gap
    for _, name in ipairs({"heading_deg", "pitch_deg", "roll_deg", "speed_kmh", "climb_mps"}) do
        if not finite(data[name]) then gap = "Нет данных: " .. name; break end
    end
    if not gap and type(data.on_ground) ~= "boolean" then gap = "Неизвестен контакт с землёй" end
    if gap then
        -- Airborne, a blind tick keeps the last command; handing the aircraft to nobody is never better.
        if not self.airborne then return self:stop(gap) end
        return self:airborneHold("data_gap", gap)
    end
    if resync then
        dt = 0.05
        self.filteredRates, self.output = {yaw = 0, pitch = 0, roll = 0}, neutral()
    end
    if vector(data.position_m) and vector(self.lastPosition) then
        local d = 0
        for i = 1, 3 do d = d + (data.position_m[i] - self.lastPosition[i])^2 end
        if math.sqrt(d) > math.max(30, data.speed_kmh / 3.6 * frameGap * 4) then
            if not self.airborne then return self:stop("Скачок позиции самолёта") end
            -- A teleport in flight: forget the motion history and re-acquire from here.
            resync, resyncReason = true, "position_jump"
            dt, self.filteredRates, self.output = 0.05, {yaw = 0, pitch = 0, roll = 0}, neutral()
            self.closest, self.passedSince, self.ringPass = nil, nil, nil
        end
    end
    self.lastPosition = data.position_m
    self.controlReady = true
    self:updateGroundProgress(data, now)
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
    self.detail = {waypoint = self.waypoint, instruction = self.instruction, path_clear = pathClear, probe_status = probeStatus,
        aggressive_now = self.aggressiveNow, route_known = self.routeKnown == true}
    local clock = data.waypoint_timer
    self.detail.marker_timer_s = type(clock) == "table" and finite(clock.remaining_s) and clock.remaining_s or nil
    self.detail.frame_gap_ms, self.detail.timing_resynced = frameGap * 1000, resync or false
    self.detail.timing_resync_reason = resyncReason
    nav = self:latencyNavigation(data, nav, now, resync)
    if resyncReason == "airborne_stall" and frameGap > 5 then self.detail.airborne_hold = "long_stall" end
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
    local hold
    if not nav then
        if self.parkingHold and not self.airborne then
            self.phase, self.status = "boarding_hold", "Стоянка: ожидание задания после входа в маркер"
            self.detail.parking_hold_reason = self.parkingHold
            stopMotion(out, data)
            out.gear_down, self.output = true, out
            return out
        end
        self.missingSince = self.missingSince or now
        if self.airborne then
            -- No marker in the air: keep flying (straight, then a holding circle) instead of quitting.
            hold = self.finalLanding and "final_no_marker" or "no_marker"
            nav = self:holdNavigation(data, hold, elapsed(now, self.missingSince) / 1000)
        end
        if not nav then
            if elapsed(now, self.missingSince) > 1000 then return self:stop("Нет текущего маркера") end
            self.phase, self.status = "waiting_marker", "Ожидание следующего маркера"
            if not self.airborne then stopMotion(out, data) end
            self.output = out
            return out
        end
    end
    if not hold then self.missingSince = nil end
    if nav.ambiguous then
        if not self.airborne then return self:stop("Неоднозначный выбор маркера") end
        -- Airborne, the scanner's sticky pick is followed; a wrong marker beats no pilot.
        hold = hold or "ambiguous_marker"
    end
    if self.jobEnded and self.airborne then hold = hold or "job_ended" end
    if hold then
        self.detail.airborne_hold = hold
        if hold == "no_marker" and elapsed(now, self.missingSince) > 2000 then self.landing, self.gearDeployReason = false, nil end
    end
    local changed = false
    if not nav.virtual then
        changed = nav.id ~= self.navId or nav.marker_type ~= self.navType
        if not changed and self.navPosition then
            for i = 1, 3 do if math.abs(nav.position[i] - self.navPosition[i]) > 0.25 then changed = true end end
        end
    end
    if changed then
        if self.navPosition then
            local heightStep = nav.position[3] - self.navPosition[3]
            if heightStep < -3 then self.descendingWaypoints = self.descendingWaypoints + 1
            elseif heightStep > 3 then self.descendingWaypoints = 0 end
        end
        self.waypoint = self.waypoint + 1
        self.escapeCount, self.blockedStill = nil, nil
        self.navId, self.navType = nav.id, nav.marker_type
        self.previousNavPosition = self.navPosition
        self.navPosition = {nav.position[1], nav.position[2], nav.position[3]}
        self.closest, self.passedSince, self.ringPass = nil, nil, nil
        self.parkingHold, self.turnback = nil, nil
        -- The aerobrake survives a checkpoint-to-checkpoint switch: the arrival chain has several.
        if not (self.aerobrake and nav.marker_type == "checkpoint") then self.aerobrake = nil end
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
        local rollout, progress = intent.rollout, self.groundProgress
        if progress then
            self.detail.ground_progress_ratio, self.detail.ground_wall_speed_kmh = progress.ratio, progress.wallSpeed
            self.detail.ground_progress_measured_ratio, self.detail.ground_progress_window_ms = progress.measuredRatio, progress.windowMs
        end
        if rollout then
            self.detail.rollout_remaining_s, self.detail.rollout_remaining_m = rollout.seconds, rollout.remaining
            self.detail.rollout_time_reserve_s = rollout.reserve
            self.detail.rollout_required_speed_kmh, self.detail.rollout_speed_cap_kmh = rollout.required, rollout.cap
            self.detail.rollout_entry_speed_kmh, self.detail.rollout_next_turn_deg = rollout.entry, rollout.nextTurn
            self.detail.rollout_linear_margin_s, self.detail.rollout_deadline_limited = rollout.margin, rollout.limited
        end
        if intent.reentry then
            local recovery = intent.reentry
            self.detail.taxi_reentry_stage, self.detail.taxi_reentry_reason = recovery.stage, recovery.reason
            self.detail.taxi_reentry_stage_ms, self.detail.taxi_reentry_heading_deg = elapsed(now, recovery.since), recovery.heading
            self.detail.taxi_reentry_exit_distance_m, self.detail.taxi_reentry_progress_m = recovery.exitDistance, recovery.progress
            self.detail.taxi_reentry_remaining_m, self.detail.taxi_reentry_cross_track_m = recovery.remaining, recovery.cross
            self.detail.taxi_reentry_inside, self.detail.taxi_reentry_failure = nav.marker_inside, recovery.failure
            if recovery.failure then return self:stop(recovery.failure .. ": ручной перехват") end
        end
        local uncertainLandingPath = self.finalLanding and self.flightSeen and data.on_ground == true
            and data.speed_kmh > 60 and pathClear ~= true
        if intent.landing_rollout or uncertainLandingPath then
            self.phase, self.status = "landing_rollout", "Касание: торможение по направлению полосы"
            out.brake, out.rudder, out.gear_down = 1, intent.rudder or 0, true
            self.detail.landing_stage, self.detail.landing_heading_deg = "rollout", self.landingHeading
            self.detail.goal_speed_kmh, self.detail.steering_error_deg = uncertainLandingPath and 0 or intent.speed, intent.steering
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
            self.detail.pushback_remaining_deg = math.max(0, math.abs(steeringError) - 28)
            self.detail.pushback_monitoring = monitoring
            -- The full alignment in reverse takes ~20 m (recorded by hand: 4.5 s at 13-21 km/h).
            if self.pushbackTravel > 45 or pushbackDistance > 45 then
                self.detail.pushback_stop_reason = "distance_limit"
                return self:stop("Превышен путь разворота задним ходом (45 м): ручной перехват")
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
                local remaining = math.max(0, nav.distance_2d_m - stopRadius)
                if nav.marker_inside == true then self.parkingHold = self.parkingHold or "marker_entry" end
                -- v*t + v^2/(2*a) <= remaining; include response time before braking.
                local deceleration, response = 3.5, 0.35
                local speedLimit = (math.sqrt((deceleration * response)^2 + 2 * deceleration * remaining)
                    - deceleration * response) * 3.6
                -- Aggressive: 50 to the stop like the pilot's taxi, the braking law (3.5 m/s^2) ends it.
                goalSpeed = math.min(goalSpeed, 50, speedLimit, remaining < 25 and 16 or 50,
                    self.parkingTarget and self.parkingTarget.crawl and 10 or 50)
                self.detail.parking_stop_radius_m, self.detail.parking_remaining_m = stopRadius, remaining
                self.detail.parking_boundary_distance_m = nav.distance_2d_m - size
                self.detail.parking_speed_limit_kmh = math.min(16, speedLimit)
                if remaining <= 0.15 then self.parkingHold = self.parkingHold or (nav.marker_inside == true and "parking_target" or "inner_boundary")
                elseif nav.marker_inside == true and not (self.parkingTarget and self.parkingTarget.waypoint == self.waypoint) then
                    self.parkingHold = self.parkingHold or "marker_entry" end
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
            if self.escape then
                -- The tail path is blocked too: stop backing, hold.
                if pathClear == false and elapsed(now, self.escape.since) > 1500 then self.escape = nil end
            elseif data.speed_kmh < 1 then
                self.blockedStill = self.blockedStill or now
                if elapsed(now, self.blockedStill) >= 1500 and (self.escapeCount or 0) < 5 and vector(data.position_m) then
                    -- Which side the obstacle is on decides the sidestep of the next attempt: a right
                    -- wing hit aims the next run 8 m left of the marker centre, and so on (recorded:
                    -- a post 19.5 m right of the taxi line, three straight retries into it).
                    local part = data.obstacle_part
                    self.avoidOffset = (part == "nose" or part == "left_engine" or part == "right_engine") and 10 or 8
                    if part == "right_wing" or part == "right_engine" then self.avoidSide = -1
                    elseif part == "left_wing" or part == "left_engine" then self.avoidSide = 1
                    elseif self.avoidSide then self.avoidSide = -self.avoidSide else self.avoidSide = -1 end
                    self.avoidWaypoint = self.waypoint
                    self.escapeCount = (self.escapeCount or 0) + 1
                    self.escape = {since = now, start = {data.position_m[1], data.position_m[2]}, distance = 14}
                    self:aimLesson(nav, data, self.avoidSide, self.avoidOffset or 8, "blocked")
                    self.blockedStill = nil
                end
            else self.blockedStill = nil end
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
        if self.escape and intent.escape then
            self.phase = "obstacle_escape"
            self.status = "Препятствие: откат назад " .. string.format("%.0f", intent.escape_moved_m or 0) .. " м, новая попытка (" .. (self.escapeCount or 1) .. ")"
        elseif self.blocked then
            goalSpeed = 0
            self.phase, self.status = "obstacle_hold", "Препятствие на траектории: торможение"
            if self.parkingTarget and self.parkingTarget.waypoint == self.waypoint then self.parkingTarget.crawl = true end
            out.hard_stop = true
        elseif pathClear == nil then
            goalSpeed = 0
            self.phase, self.status = "probe_wait", "Ожидание проверки пути"
        elseif switching or intent.pending then
            goalSpeed = 0
            self.phase, self.status = "direction_change", "Остановка перед сменой направления"
        elseif intent.reentry then
            self.phase = "taxi_reentry_" .. intent.reentry.stage
            self.status = intent.reentry.stage == "backin" and "Повторный заход: задним ходом к метке"
                or intent.reentry.stage == "exit" and "Повторный заход: выход за границу метки"
                or intent.reentry.stage == "return" and "Повторный заход: возврат в метку"
                or intent.reentry.stage == "blocked" and "Нет прогресса задним ходом: остановка, нужен ручной перехват"
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
            if speedError > 0.5 then out.brake = clamp(speedError * 0.14, 0, 1)
            elseif speedError < -1 then out.throttle = clamp(-speedError * 0.04, 0, 0.35) end
        else
            local speedError = goalSpeed - forward * 3.6
            -- Random bands per pulse, in our km/h: release at goal + (-1..+3 spdmtr), press again at
            -- goal - (4..8 spdmtr). A machine holds 30.0; a hand lands on 33, coasts to 26, presses.
            local up = self.pulseUp or 0
            local down = math.min(self.pulseDown or 8.5, goalSpeed * 0.5)
            if speedError < -8 - math.max(0, up) then
                -- A real slowdown (corner, stop): brake in full proportion, throttle off.
                self.groundThrottleOn = false
                out.brake = clamp(-speedError * 0.08, 0, 1)
            elseif goalSpeed < 30 then
                -- Corners and the parking crawl: a hand feathers the key here, no pulses.
                self.groundThrottleOn = false
                if speedError > 0.5 then out.throttle = clamp(speedError * 0.09, 0, math.abs(steeringError) > 60 and 0.2 or 0.5)
                elseif speedError < -3 then out.brake = clamp(-speedError * 0.08, 0, 0.6) end
            else
                if speedError > down then
                    if not self.groundThrottleOn then self.pulseUp = (math.random() * 4 - 1) / 0.702 end
                    self.groundThrottleOn = true
                elseif speedError <= -up then
                    if self.groundThrottleOn then self.pulseDown = (4 + math.random() * 4) / 0.702 end
                    self.groundThrottleOn = false
                end
                if self.groundThrottleOn then out.throttle = math.abs(steeringError) > 60 and 0.12 or 1 end
            end
            self.detail.ground_throttle_on, self.detail.pulse_up_kmh, self.detail.pulse_down_kmh = self.groundThrottleOn == true, up, down
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
            -- Rotation at 130 (recorded by hand: airborne at 104-119 with 11 deg of pitch); the wheels
            -- leave the runway as early as the aircraft allows, the climb follows the first ring.
            local pitchGoal = data.speed_kmh >= 130 and 8 or -1
            out.elevator = data.speed_kmh >= 125 and clamp((pitchGoal - data.pitch_deg) * 0.06 - pitchRate * 0.025, -0.25, 0.5) or 0
            out.aileron = clamp(-data.roll_deg * 0.04 - rollRate * 0.02, -0.3, 0.3)
            self.detail.goal_pitch_deg = pitchGoal
        end
        self.detail.runway_align, self.detail.runway_rolling = intent.runway_align, self.takeoffRolling
        self.detail.goal_yaw_dps, self.detail.steering_error_deg = intent.yaw, error
        -- Aggressive: gear up the moment the wheels leave the runway (recorded by hand: it accelerates faster).
        -- Takeoff is always the aggressive one: the gear comes up as soon as the wheels leave the ground.
        out.gear_down = not (data.on_ground == false and data.speed_kmh > 115)
    else
        if nav.virtual then self.closest = nil else self.closest = math.min(self.closest or math.huge, nav.distance_3d_m) end
        local mc = nav.color_rgba or {}
        local yellowTarget = finite(mc[1]) and finite(mc[2]) and finite(mc[3]) and mc[1] > 200 and mc[2] > 160 and mc[3] < 80
        -- Aggressive arrival over the red chain (checkpoints register when overflown; recorded at 13 m).
        -- The markers ahead are known through their arrows, so the chain is planned as a whole: the
        -- first corner that cannot be flown at >= 100 km/h fixes the touchdown point, the yellow marker
        -- fixes the aerobrake point, and the along-chain distances decide whether to skim on or land now.
        -- Aggressive only on a chain the bot has flown before: the first visit to an airport is flown
        -- the cautious way while the chain is learned; the next one gets the whole plan.
        -- Only a chain learned in this session counts: the first visit to any airport is flown the
        -- cautious way even when the airport is in the built-in memory (it serves the HUD and the plan
        -- preview, not the aggression).
        local routeKnown = type(nav.chain_source) == "string" and nav.chain_source:find("learned_", 1, true) == 1
        local plan = self.aggressiveNow and self.landing and nav.marker_type == "checkpoint" and not yellowTarget and routeKnown
            and vector(nav.position) and vector(data.position_m) and self:chainPlan(nav, data) or nil
        if plan then self.routeKnown = true; self.detail.plan_points = plan.points end
        local mustLandNow = false
        if plan then
            local v = data.speed_kmh / 3.6
            -- ~150 m to put the wheels down from 10 m, then braking to a ground corner at 3.5 m/s^2.
            if plan.landIndex then mustLandNow = plan.distances[plan.landIndex] < 150 + math.max(0, v * v - 36) / 7 end
        end
        local cornerNow = plan and plan.corners[1] or nil
        -- No plan (chain not learned this session) means no skim at all: the cautious landing law.
        -- A checkpoint missed in the air is taken by the turn-back below, never by landing straight
        -- ahead (recorded: that pointed the aircraft at a wall).
        local skim = plan ~= nil and self.aggressiveNow and self.landing and not self.finalLanding and not yellowTarget
            and nav.marker_type == "checkpoint" and data.on_ground == false and not mustLandNow and not self.turnback
            and (not cornerNow or cornerNow.speed >= 85) and math.abs(error) < 110
        local descendingTarget = finite(nav.altitude_error_m) and nav.altitude_error_m < -2
        if self.flightSeen and self.descendingWaypoints >= 3 and agl and agl < 170 and descendingTarget
            and finite(data.surface and data.surface.ground_z_m)
            and nav.position[3] - data.surface.ground_z_m < 90 then self.landing = true end
        self.detail.descending_waypoints = self.descendingWaypoints
        local groundCheckpoint = nav.marker_type == "checkpoint" and finite(data.surface and data.surface.ground_z_m)
            and math.abs(nav.position[3] - data.surface.ground_z_m) < 15
        -- Aggressive: the low rings are flown through, not landed on (recorded by hand: rings at 5-8 m,
        -- then the whole checkpoint chain in the air); the wheels wait for the yellow marker.
        -- A missed red checkpoint on an aggressive route is taken by the turn-back, never by landing
        -- straight ahead (recorded: wheels down on the grass, 120 km/h, marker 5 m behind the wing).
        local missedBehind = self.aggressiveNow and nav.marker_type == "checkpoint" and not yellowTarget and math.abs(error) > 90
        -- Never a touchdown while turning back or holding for a lost marker (recorded: 6 m over water).
        if self.landing and agl and not self.finalLanding and not skim and not missedBehind and not self.turnback and not self.detail.airborne_hold
            and nav.marker_type ~= "ring" and not nav.awaiting_marker
            and (agl < 7 or groundCheckpoint and agl < 15) then
            self.finalLanding = true
            self.landingHeading = math.abs(error) < 25 and nav.bearing_deg or data.heading_deg
        end
        self.phase, self.status = self.landing and "approach" or "flight", self.landing and "Заход на посадку" or "Полёт к центру кольца"
        if hold == "no_marker" then
            self.phase = elapsed(now, self.missingSince) > 2000 and "hold_pattern" or "hold_straight"
            self.status = "Маркер потерян: управление удержано, круг ожидания"
        elseif hold == "final_no_marker" then self.phase = "hold_final"
        elseif hold == "ambiguous_marker" then self.phase, self.status = "hold_ambiguous", "Неоднозначный маркер: следую за выбранным" end
        -- The ring is 30 m across: the aim point sits inside it, shifted up to 7.5 m toward the inside
        -- of the turn that follows (its arrow gives the next leg), so the bend after the ring is
        -- smaller and the aircraft never chases the exact centre (recorded: banks at the ring, misses).
        local aimBearing = nav.bearing_deg
        if nav.marker_type == "ring" and not self.finalLanding and vector(nav.next_position) and vector(nav.position)
            and vector(data.position_m) and finite(nav.bearing_deg) and finite(nav.marker_size_m) and nav.distance_2d_m > 15 then
            local ndx, ndy = nav.next_position[1] - nav.position[1], nav.next_position[2] - nav.position[2]
            if ndx * ndx + ndy * ndy > 100 then
                local nextTurn = angle(math.deg(math.atan2(ndx, ndy)) - nav.bearing_deg)
                -- Swept against the strict ring sim: 12 m of shift (gain /25 x 0.40) keeps 52/52 at
                -- every start offset; 13.5 m starts losing rings. The old 7.5 m cap aimed at the centre.
                local shift = clamp(nextTurn / 25, -1, 1) * nav.marker_size_m * 0.40
                local b = math.rad(nav.bearing_deg)
                local ax, ay = nav.position[1] + math.cos(b) * shift, nav.position[2] - math.sin(b) * shift
                local centreBearing = math.deg(math.atan2(nav.position[1] - data.position_m[1], nav.position[2] - data.position_m[2]))
                -- Only with consistent geometry: the bearing computed from positions must agree with the scanner's.
                if math.abs(angle(centreBearing - nav.bearing_deg)) < 5 then
                    aimBearing = math.deg(math.atan2(ax - data.position_m[1], ay - data.position_m[2]))
                    error = angle(aimBearing - data.heading_deg)
                    self.detail.ring_aim_shift_m, self.detail.aim_point = shift, {ax, ay}
                end
            end
        end
        local courseError = error
        if finite(nav.track_error_deg) and data.speed_kmh > 100 then
            local trackNow = finite(data.track_deg) and data.track_deg or angle(nav.bearing_deg - nav.track_error_deg)
            courseError = angle(error + angle(angle(aimBearing - trackNow) - error) * 0.7)
        end
        if self.finalLanding then
            if self.aerobrake and nav.marker_type == "checkpoint" and finite(nav.bearing_deg) then
                -- The last red marker and the yellow form a short landing tail. Keep following
                -- that tail when the marker advances; never retain the previous marker's heading.
                self.landingHeading = nav.bearing_deg
            end
            courseError = angle(self.landingHeading - (finite(data.track_deg) and data.track_deg or data.heading_deg))
        end
        local horizontalSpeed = finite(data.horizontal_speed_kmh) and data.horizontal_speed_kmh / 3.6 or data.speed_kmh / 3.6
        -- Bank the wing clears at this height, needed before the full limit is known.
        local bankLimitEarly = agl and agl < 16 and math.deg(math.asin(clamp((agl - 2) / 12, 0, 1))) or 45
        local trim = data.speed_kmh > 220 and -1.4 or -0.7
        local captureMargin = finite(nav.marker_size_m) and math.max(0, nav.marker_size_m * 0.45) or 0
        local lastRingBeforeChain = nav.marker_type == "ring" and type(nav.chain_ahead) == "table" and nav.chain_ahead[1]
            and nav.chain_ahead[1].marker_type == "checkpoint" and not nav.chain_ahead[1].unknown
        local ringLift = (self.aggressiveNow and self.landing and lastRingBeforeChain and captureMargin > 0) and math.min(6, captureMargin * 0.45) or 0
        local ringAltitudeError = (finite(nav.altitude_error_m) and nav.altitude_error_m or 0) + ringLift
        self.detail.ring_lift_m = ringLift
        local track = finite(data.track_deg) and data.track_deg
            or finite(nav.track_error_deg) and angle(nav.bearing_deg - nav.track_error_deg) or data.heading_deg
        local trackError = angle(nav.bearing_deg - track)
        local alongTrack = nav.distance_2d_m * math.cos(math.rad(trackError))
        local crossTrack = nav.distance_2d_m * math.sin(math.rad(trackError))
        local verticalMiss = ringAltitudeError - data.climb_mps * alongTrack / math.max(1, horizontalSpeed)
        local miss3d = math.sqrt(crossTrack^2 + verticalMiss^2)
        if not self.ringPass and nav.marker_type == "ring" and not self.finalLanding and vector(data.position_m)
            and captureMargin > 0 and data.speed_kmh > 100 and alongTrack > 0
            -- The straight pass starts up to 250 m out once the track already threads the ring: no
            -- more bank chasing the centre in the last seconds (recorded: 47 deg of roll at the ring).
            and nav.distance_2d_m < math.max(250, horizontalSpeed * 2.5)
            and math.abs(trackError) < 10 and math.abs(data.roll_deg) < 25 and math.abs(yawRate) < 6
            and miss3d < captureMargin * 0.8 then
            -- Keep the incoming course through the capture corridor until the server moves the ring.
            self.ringPass = {heading = track, pitch = clamp(math.deg(math.atan2(ringAltitudeError,
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
                    -- The server did not move the ring: leave the corridor and come around for another pass.
                    self.ringPass, pass = nil, nil
                    self.detail.airborne_hold = "ring_not_advanced"
                end
            end
        end
        self.detail.ring_flythrough, self.detail.predicted_miss_3d_m = pass ~= nil, miss3d
        if not pass and not (self.landing and nav.marker_type == "checkpoint") and self.closest
            and nav.distance_3d_m > self.closest + 15 and math.abs(error) > 95 then
            self.passedSince = self.passedSince or now
            -- Behind the marker: the intercept law turns back to it; only the alarm goes off.
            if elapsed(now, self.passedSince) > 350 then self.detail.airborne_hold = self.detail.airborne_hold or "marker_behind" end
        else self.passedSince = nil end
        local interceptDistance = math.max(60, nav.distance_2d_m, horizontalSpeed * 2.2)
        local turnRate = 2 * horizontalSpeed / interceptDistance * math.sin(math.rad(clamp(courseError, -75, 75)))
        local wantedYaw = math.deg(turnRate)
        if changed or not self.turnDirection then self.turnDirection = courseError < 0 and -1 or 1 end
        local ringCapture = nav.marker_type == "ring" and captureMargin > 0 and math.abs(courseError) < 15
            and nav.distance_2d_m < horizontalSpeed * 1.2 and math.abs(crossTrack) < captureMargin
            and courseError * self.turnDirection < 0
        -- Inside the capture corridor, roll out instead of reversing bank to chase centimetres.
        if ringCapture then turnRate, wantedYaw = 0, 0 end
        -- Anticipation: inside the corridor the marker is already made, so the aircraft rolls into the
        -- turn that follows instead of levelling and arriving at the next one with no bank left
        -- (video: it enters banked, straightens in the corridor, then misses the next marker).
        -- The roll-in is allowed only while the sideways drift it costs, d^2 / 2r, still fits the
        -- room left in this marker's corridor.
        local anticipate, nextBank = false, nil
        if pass and vector(nav.next_position) and vector(data.position_m) and finite(nav.distance_2d_m) then
            local bx, by = nav.next_position[1] - data.position_m[1], nav.next_position[2] - data.position_m[2]
            local distNext = math.max(1, math.sqrt(bx * bx + by * by))
            local nextCourse = angle(math.deg(math.atan2(bx, by)) - track)
            local turnBank = math.max(5, math.min(bankLimitEarly or 45, 45))
            local radius = horizontalSpeed * horizontalSpeed / math.max(1, 9.81 * math.tan(math.rad(turnBank)))
            local drift = nav.distance_2d_m * nav.distance_2d_m / (2 * radius)
            if math.abs(nextCourse) > 3 then
                local nextRate = 2 * horizontalSpeed / math.max(60, distNext, horizontalSpeed * 1.2) * math.sin(math.rad(clamp(nextCourse, -75, 75)))
                nextBank = clamp(math.deg(math.atan2(horizontalSpeed * nextRate, 9.81)), -12, 12)
            end
            if math.abs(nextCourse) > 4 and drift <= math.max(0, captureMargin - miss3d) then
                anticipate = true
                courseError = nextCourse
                turnRate = 2 * horizontalSpeed / math.max(60, distNext, horizontalSpeed * 1.2)
                    * math.sin(math.rad(clamp(courseError, -75, 75)))
                wantedYaw, ringCapture = math.deg(turnRate), false
                self.detail.ring_anticipate_deg, self.detail.ring_anticipate_drift_m = nextCourse, drift
            end
        end
        if pass and not anticipate then
            -- Through the corridor the rudder holds the line to the ring (cross-track), not just the
            -- heading it had when the pass began (simulated: heading-hold passed 12-15 m off centre).
            local drift = angle(pass.heading - track)
            local lineWeight = clamp((math.abs(crossTrack) - captureMargin * 0.4) / (captureMargin * 0.3), 0, 1)
            local lineError = finite(trackError) and clamp(trackError, -6, 6) * lineWeight or 0
            wantedYaw = clamp((drift < 0 and -1 or 1) * math.max(0, math.abs(drift) - 0.75) * 1.0 + lineError * 1.2, -4, 4)
            turnRate, ringCapture = math.rad(wantedYaw), true
            if pass.crossed then self.status = "Пролёт кольца: ожидание новой цели" end
        end
        -- Aggressive approach banks to 45 deg only with height to spare; near the ground the wing tip
        -- clearance rules (half span ~7.5 m), so the limit falls with AGL: 20 deg at 10 m, 12 deg on the deck.
        -- Cautious landing: 45 deg with height, 30 low (recorded: a 42 deg ring against a 25 deg limit, missed).
        -- Wing clearance rules near the ground: at bank b the tip sits about 12 * sin(b) below the
-- fuselage, so the bank that keeps 2 m of it is asin((agl - 2) / 12) (recorded: 33 deg at 6 m AGL
-- scraped Ugryumoch and destroyed the aircraft; the pilot banks 38 deg only at 9 m and above).
        local wingBank = agl and agl < 16 and math.deg(math.asin(clamp((agl - 2) / 12, 0, 1))) or 90
        self.detail.wing_bank_limit_deg = wingBank
        local bankLimit = self.landing and (self.aggressiveNow and (agl and agl > 25 and 45 or clamp(15 + (agl or 0) * 1.2, 15, 45)) or (agl and agl > 40 and 45 or 30))
            or agl and clamp(12 + math.max(0, agl - 5) * 0.8, 12, 70) or 25
        if nav.virtual then bankLimit = math.min(bankLimit, 25) end
        bankLimit = math.min(bankLimit, wingBank)
        -- Right after a turn-back the intercept stays gentle: 45 deg of bank for 6 s, so the ring is
        -- met on a straight line instead of another hard bank (simulated: 65-70 deg at 236 m, +50 m).
        if self.turnbackEnded and elapsed(now, self.turnbackEnded) < 6000 then bankLimit = math.min(bankLimit, 45) end
        local requiredBank = math.deg(math.atan2(horizontalSpeed * turnRate, 9.81))
        local goalRoll = clamp(requiredBank, -bankLimit, bankLimit)
        -- Rudder first (recorded by hand: 45% of all turning done wings level, the rudder alone gives
        -- 5-9 deg/s at any speed): a turn the rudder can make is yawed with level wings; the bank
        -- comes only when the needed rate is beyond it, or for a turn-back.
        -- Rudder only for small, far corrections, with hysteresis so the wings do not flap between
        -- level and banked at the ring (recorded: bank kicked in 138 m out, roll 47 deg at the ring).
        local active = self.rudderTurnActive
        local rudderTurn = not self.turnback and not nav.virtual
            and math.abs(math.deg(turnRate)) <= (active and 9 or 7)
            and math.abs(courseError) <= (active and 20 or 15)
        self.rudderTurnActive = rudderTurn
        -- A little bank in the direction of the turn, the rudder does the rest (the pilot's way).
        if rudderTurn then goalRoll = clamp(requiredBank, -12, 12) end
        -- Through the ring the bank is held toward the leg that follows instead of being wiped to
        -- zero: 12 deg at 270 km/h drifts 2 m over the last 100 m, the corridor is 21 m.
        if pass and not anticipate then goalRoll = nextBank or 0 end
        -- Checkpoints are flat circles of unlimited height (MTA collides a checkpoint marker as a 2D
        -- circle; recorded hits at 13 m), so an aggressive arrival never dives at one: it keeps at least
        -- 10 m and skims. Rings stay precise targets.
        local altitudeError = ringAltitudeError
        if nav.awaiting_marker and agl then altitudeError = math.max(altitudeError, 12 - agl) end
        if self.aggressiveNow and nav.marker_type == "checkpoint" and not yellowTarget and agl then
            altitudeError = math.max(altitudeError, 10 - agl)
        end
        local lookahead = 0.35
        local elevation = math.deg(math.atan2(altitudeError - data.climb_mps * lookahead,
            math.max(20, nav.distance_2d_m - horizontalSpeed * math.cos(math.rad(courseError)) * lookahead)))
        local goalPitch = pass and pass.pitch or clamp(elevation + trim, -23, 22)
        local goalSpeed = self.landing and not self.aggressiveNow and agl and clamp(125 + math.max(0, agl - 8) * 1.25, 125, 245)
            or 255  -- no bank penalty: in this game the turn tightens with speed, it does not loosen
        local cruiseBlend = not self.landing and not self.finalLanding
            and clamp((15 - math.max(math.abs(goalRoll), math.abs(data.roll_deg))) / 10, 0, 1)
                * clamp((12 - math.abs(courseError)) / 6, 0, 1) or 0
        goalSpeed = goalSpeed + 15 * cruiseBlend
        -- A sharp turn at the next ring is prepared: its arrow gives the leg after it, and from 600 m
        -- out the speed comes down to 200 (170 past 60 deg) so the bank can make it (recorded: 43 deg
        -- at 265 km/h asked for 67-85 deg of bank against a 50 deg limit; the ring passed 49 m off).
        if nav.marker_type == "ring" and vector(nav.next_position) and vector(nav.position) and finite(nav.bearing_deg) then
            local ndx, ndy = nav.next_position[1] - nav.position[1], nav.next_position[2] - nav.position[2]
            if ndx * ndx + ndy * ndy > 100 then
                local nextTurn = math.abs(angle(math.deg(math.atan2(ndx, ndy)) - nav.bearing_deg))
                self.detail.next_ring_turn_deg = nextTurn
                -- Speed for the turn after the ring: the arc from this ring to the next one has radius
                -- leg / (2 sin(turn)); the bank available at this height sets the speed that radius
                -- allows (v^2 = g tan(bank) r). Never below 140. (Simulated: 20-27 deg turns after
                -- takeoff at 257 km/h with a 16-30 deg bank limit passed the rings 45 m off.)
                if nextTurn > 8 and nav.distance_2d_m < 700 and not self.finalLanding then
                    local leg = math.sqrt(ndx * ndx + ndy * ndy)
                    local radiusNeeded = leg / (2 * math.sin(math.rad(math.min(90, nextTurn))))
                    local bankAhead = math.min(bankLimit, 60)
                    local allowed = math.sqrt(9.81 * math.tan(math.rad(bankAhead)) * radiusNeeded) * 3.6
                    goalSpeed = math.min(goalSpeed, math.max(140, allowed))
                    self.detail.next_ring_speed_kmh = math.max(140, allowed)
                end
            end
        end
        if self.aggressiveNow and self.landing and nav.marker_type == "ring" and not self.finalLanding
            and routeKnown and vector(nav.position) and vector(data.position_m) and type(nav.chain_ahead) == "table"
            and #nav.chain_ahead >= 2 then
            local ahead = self:chainPlan(nav, data)
            local chainMin, first = 240, nil
            for index, corner in pairs(ahead.corners) do
                if not corner.ring and corner.speed < 200 then
                    chainMin = math.min(chainMin, corner.speed)
                    if not first or corner.distance < first then first = corner.distance end
                end
            end
            if chainMin < 240 and first then
                -- Reserve one second of response and budget 2 m/s^2 over the remaining path.
                -- Do not credit an extra shed on the same metres: skim may be cancelled for landing.
                local reaction = horizontalSpeed
                local room = math.max(0, first - reaction)
                local ceiling = math.sqrt((chainMin / 3.6) ^ 2 + 2 * 2 * room) * 3.6
                goalSpeed = math.min(goalSpeed, math.max(chainMin, ceiling))
                self.detail.ring_chain_min_kmh, self.detail.ring_chain_ceiling_kmh = chainMin, ceiling
            end
        end
        local arrivalLimit
        if plan and not self.turnback and not missedBehind then
            arrivalLimit = 240
            for _, corner in pairs(plan.corners) do
                if not corner.ring then
                    local target = corner.speed * (corner.turn >= 20 and 0.9 or 1)
                    if plan.landIndex and corner.turn >= 20 then target = math.min(target, 40) end
                    local room = math.max(0, corner.distance - (corner.start or 0) - horizontalSpeed)
                    arrivalLimit = math.min(arrivalLimit, math.sqrt((target / 3.6)^2 + 4 * room) * 3.6)
                end
            end
            self.detail.arrival_speed_limit_kmh = arrivalLimit
            self.detail.arrival_must_land = mustLandNow
            goalSpeed = math.min(goalSpeed, math.max(95, arrivalLimit))
        end
        if skim and agl and agl < 40 then
            -- Height: 10 m on straights, 22 m before a corner so the bank has wing clearance
            -- (bank limit 15 + 2.2*AGL, capped at 55; the plan's radius assumes full bank).
            local cornerSoon = cornerNow and cornerNow.turn >= 20 and nav.distance_2d_m < 200
            local hold = cornerSoon and 22 or 10
            goalPitch = clamp((hold - agl) * 0.8 - data.climb_mps * 0.6 + trim, -4, 10)
            bankLimit = math.min(clamp(15 + agl * 2.2, 15, 55), wingBank)
            -- Speed: every corner ahead bounds it through the braking law (air, ~3 m/s^2), and the
            -- yellow marker through the aerobrake distance.
            local limit = 240
            if plan then
                for _, corner in pairs(plan.corners) do
                    -- Reach the corner speed BEFORE the arc starts, with one second for attitude
                    -- response. Distance to the vertex itself brakes too late on the short taxi legs.
                    local remaining = math.max(0, corner.distance - (corner.start or 0) - horizontalSpeed)
                    -- A speed reserve on close bends covers yaw response and tracking error;
                    -- the geometric maximum alone leaves no room to finish rolling into the turn.
                    local cornerSpeed = corner.speed * (corner.turn >= 20 and 0.9 or 1)
                    limit = math.min(limit, math.sqrt((cornerSpeed / 3.6)^2 + 2 * 2 * remaining) * 3.6)
                end
                -- The stop: wheels down ~70 m before the yellow marker's centre at <= 70 km/h (recorded
                -- by hand: 62-71 km/h, 58-69 m), air braking measured at 2 m/s^2 (recorded: 202 -> 118
                -- over 12 s). 3 m/s^2 with a 60 m stop put the wheels down at 103 km/h and the terminal
                -- was 8 m past the marker.
                if plan.stopIndex then limit = math.min(limit, math.sqrt(19 ^ 2 + 2 * 2 * math.max(0, plan.distances[plan.stopIndex] - 70)) * 3.6) end
                -- Without memory the next marker may be the yellow one: keep enough room to aerobrake into it.
                if plan.unknownIndex then limit = math.min(limit, math.sqrt(19 ^ 2 + 2 * 2 * math.max(0, plan.distances[plan.unknownIndex] - 70)) * 3.6) end
            end
            local chainMin = 240
            if plan then for _, corner in pairs(plan.corners) do chainMin = math.min(chainMin, corner.speed) end end
            goalSpeed = math.max(95, math.min(limit, chainMin))
            self.detail.skim_chain_min_kmh = chainMin
            -- Shed while faster than the plan by 10, keep shedding until within 3 (recorded: a 30 km/h
            -- threshold fired for one frame, the nose never got up and the engine never went off).
            local shed = agl < 35 and (data.speed_kmh > goalSpeed + 10 or self.skimShed and data.speed_kmh > goalSpeed + 3)
            if shed then
                self.skimShed = self.skimShed or {since = now}
                goalPitch = clamp(18 - math.max(0, agl - 15) * 0.8, 6, 18)
                local age = elapsed(now, self.skimShed.since)
                out.engine_off = data.speed_kmh > 110 and data.pitch_deg > 0 and data.climb_mps > -6
                    and age >= 150 and (age % 900) < 500 or false
            else
                self.skimShed = nil
            end
            self.detail.skim_shed, self.detail.skim_engine_cut = shed, out.engine_off == true
            -- Corner cut: the fillet arc starts before the marker; from there the aircraft steers for
            -- the next one, so the arc clips this marker's circle instead of overflying its centre.
            -- The aircraft flies the corridor point the plan was checked against, not the marker's
            -- centre (recorded: plan on the smoothed path, aircraft on the centres, corner missed by
            -- 10 m at 40 m legs, then a full-throttle 'approach' into the terminal).
            self.pursuitAim = false
            if plan and plan.points and vector(plan.points[1]) and nav.distance_2d_m > 12 then
                local p1, p2 = plan.points[1], plan.points[2]
                local px, py = p1[1] - data.position_m[1], p1[2] - data.position_m[2]
                local d1 = math.max(1, math.sqrt(px * px + py * py))
                local lookahead = clamp(horizontalSpeed * 1.6, 40, 120)
                if vector(p2) and d1 < lookahead then
                    local ex, ey = p2[1] - p1[1], p2[2] - p1[2]
                    local leg = math.max(1, math.sqrt(ex * ex + ey * ey))
                    local t = clamp((lookahead - d1) / leg, 0, 1)
                    local ax, ay = p1[1] + ex * t - data.position_m[1], p1[2] + ey * t - data.position_m[2]
                    local size = finite(nav.marker_size_m) and nav.marker_size_m or 30
                    local cx, cy = nav.position[1] - data.position_m[1], nav.position[2] - data.position_m[2]
                    local offCentre = nav.distance_2d_m * math.abs(math.sin(math.rad(angle(math.deg(math.atan2(ax, ay)) - math.deg(math.atan2(cx, cy))))))
                    -- Only while the line to the slid aim still crosses this marker's circle.
                    if offCentre <= size * 0.7 then px, py, self.pursuitAim = ax, ay, true end
                    self.detail.skim_pursuit_t, self.detail.skim_pursuit_off_m = t, offCentre
                end
                local distAim = math.max(1, math.sqrt(px * px + py * py))
                courseError = angle(math.deg(math.atan2(px, py)) - track)
                turnRate = 2 * horizontalSpeed / math.max(40, distAim, horizontalSpeed * 1.2) * math.sin(math.rad(clamp(courseError, -75, 75)))
                wantedYaw = math.deg(turnRate)
                requiredBank = math.deg(math.atan2(horizontalSpeed * turnRate, 9.81))
                self.detail.skim_aim_offset_m = math.sqrt((plan.points[1][1] - nav.position[1])^2 + (plan.points[1][2] - nav.position[2])^2)
                local size = finite(nav.marker_size_m) and nav.marker_size_m or 30
                local straightMiss = math.abs(nav.distance_2d_m * math.sin(math.rad(angle(nav.bearing_deg - track))))
                self.detail.skim_straight_miss_m, self.detail.skim_centre_bias = straightMiss, false
                if nav.distance_2d_m < 70 and straightMiss > size * 0.8 then
                    courseError = angle(nav.bearing_deg - track)
                    turnRate = 2 * horizontalSpeed / math.max(40, nav.distance_2d_m, horizontalSpeed * 1.2) * math.sin(math.rad(clamp(courseError, -75, 75)))
                    wantedYaw = math.deg(turnRate)
                    requiredBank = math.deg(math.atan2(horizontalSpeed * turnRate, 9.81))
                    self.detail.skim_centre_bias = true
                end
            end
            local cut = false
            if not self.pursuitAim and nav.distance_2d_m < (cornerNow and cornerNow.start or 0) + 30 + horizontalSpeed * 0.7
                and vector(nav.next_position) and math.abs(error) < 60 then
                -- The cut aims at the next marker's corridor point when the plan has one, and only
                -- while that line still passes within 0.7 R of the current centre; otherwise the cut
                -- would leave the circle (recorded: cut from 38 m, passed 35 m off a 30 m marker).
                local aim = plan and plan.points and plan.points[2] or nav.next_position
                local bx, by = aim[1] - data.position_m[1], aim[2] - data.position_m[2]
                local cx, cy = nav.position[1] - data.position_m[1], nav.position[2] - data.position_m[2]
                local aimBearingNext = math.deg(math.atan2(bx, by))
                local offCentre = nav.distance_2d_m * math.abs(math.sin(math.rad(angle(aimBearingNext - math.deg(math.atan2(cx, cy))))))
                -- The straight-line offset is telemetry only. Validate the curved intercept;
                -- neither a point behind the aircraft nor an unmade turn proves capture.
                local forward = cx * math.sin(math.rad(track)) + cy * math.cos(math.rad(track))
                local toward = cx * bx + cy * by
                if toward > 0 and forward > 0 then
                    cut, self.detail.low_pass_cut_predicted_m = self:checkpointCut(nav, data, aim, bankLimit, track)
                end
                self.detail.low_pass_cut_off_centre_m = offCentre
            end
            if cut then
                local aim = plan and plan.points and plan.points[2] or nav.next_position
                local bx, by = aim[1] - data.position_m[1], aim[2] - data.position_m[2]
                local distNext = math.max(1, math.sqrt(bx * bx + by * by))
                courseError = angle(math.deg(math.atan2(bx, by)) - track)
                turnRate = 2 * horizontalSpeed / math.max(40, distNext, horizontalSpeed * 1.2) * math.sin(math.rad(clamp(courseError, -75, 75)))
                wantedYaw = math.deg(turnRate)
                requiredBank = math.deg(math.atan2(horizontalSpeed * turnRate, 9.81))
            end
            goalRoll = clamp(requiredBank, -bankLimit, bankLimit)
            self.phase = "low_pass"
            self.status = shed and "Агрессив: гашу скорость, нос вверх, двигатель" or cut and "Агрессив: срез угла на следующий чекпоинт" or "Агрессив: пролёт чекпоинтов" .. (cornerSoon and ", поворот впереди" or " на 10 м")
            self.detail.low_pass_speed_limit_kmh, self.detail.low_pass_cut, self.detail.low_pass_hold_m = limit, cut, hold
            self.detail.low_pass_corner_turn_deg = cornerNow and cornerNow.turn or nil
            self.detail.low_pass_corner_speed_kmh = cornerNow and cornerNow.speed or nil
            self.detail.low_pass_corner_start_m = cornerNow and cornerNow.start or nil
            self.detail.low_pass_land_index, self.detail.low_pass_stop_index = plan and plan.landIndex or nil, plan and plan.stopIndex or nil
        end
        -- Missed marker: a maximum-rate turn toward it beats the gentle intercept two to one
        -- (recorded by hand: 20-23 s at 35 deg mean bank against ~13 s at 65 deg, 270 km/h, throttle
        -- full). Bank is capped by height above ground; the intercept law takes over once aligned.
        local behind = self.detail.airborne_hold == "marker_behind" or self.detail.airborne_hold == "ring_not_advanced"
        -- Rings always; red checkpoints in the aggressive mode too (they register from any height, so
        -- coming back over one is enough; the climb in the turn keeps it above the terminal).
        local turnbackTarget = nav.marker_type == "ring" or (self.aggressiveNow and nav.marker_type == "checkpoint" and not yellowTarget)
        -- A checkpoint lost during the skim (beside or behind, low) turns back at once: no waiting for
        -- the hold detector and never the plain approach law with full throttle at 7 m (recorded).
        local skimMiss = self.aggressiveNow and nav.marker_type == "checkpoint" and not yellowTarget and not nav.awaiting_marker
            and math.abs(error) > 95 and finite(nav.distance_2d_m)
            and nav.distance_2d_m > (finite(nav.marker_size_m) and nav.marker_size_m or 30)
        if skimMiss then self.detail.airborne_hold = self.detail.airborne_hold or "skim_miss" end
        if not self.turnback and (behind or skimMiss) and not self.finalLanding and not pass and turnbackTarget then
            self.turnback = {dir = courseError < 0 and -1 or 1, since = now, reason = self.detail.airborne_hold}
        end
        local turnback = self.turnback
        local tbDist = nav.distance_2d_m
        if turnback then
            -- Re-enter along the route: aim 250 m before the ring on the line from the previous
            -- marker, so the ring is met already heading for the next one (simulated: re-entry from
            -- any direction missed the following ring and chained one turn-back into the next).
            local prev = self.previousNavPosition
            if not turnback.entryDone and vector(prev) and vector(nav.position) and vector(data.position_m) then
                local ddx, ddy = nav.position[1] - prev[1], nav.position[2] - prev[2]
                local len = math.sqrt(ddx * ddx + ddy * ddy)
                if len > 50 then
                    local ex, ey = nav.position[1] - ddx / len * 250, nav.position[2] - ddy / len * 250
                    local de = math.sqrt((ex - data.position_m[1])^2 + (ey - data.position_m[2])^2)
                    if de > 60 then
                        local be = math.deg(math.atan2(ex - data.position_m[1], ey - data.position_m[2]))
                        courseError, error, tbDist = angle(be - track), angle(be - data.heading_deg), de
                        self.detail.turnback_entry_m = de
                    else turnback.entryDone = true end
                end
            end
        end
        if turnback and (math.abs(courseError) < 12 and tbDist > 40
            or elapsed(now, turnback.since) > 25000 or not turnbackTarget) then
            self.turnback, turnback, self.turnbackEnded = nil, nil, now
        end

        if turnback then
            rudderTurn, self.rudderTurnActive = false, false
            -- No engine-cut or airbrake command from the earlier skim may survive a recovery.
            out.engine_off, self.skimShed, self.turnbackPower = false, nil, true
            local bankMax = math.min(not agl and 45 or agl < 25 and 45 or agl < 150 and 60 or 70, wingBank)
            -- The server allows 30 s per marker and the timer is visible: with less than 12 s left the
            -- teardrop is a firing, so the recovery is the tightest turn the wing clears, at once.
            local timer = data.waypoint_timer
            local urgent = type(timer) == "table" and finite(timer.remaining_s) and finite(timer.age_ms) and timer.age_ms < 3000
                and timer.remaining_s < 12
            if urgent then
                turnback.extend = false
                bankMax = math.min(70, wingBank)
            end
            self.detail.turnback_urgent, self.detail.marker_timer_s = urgent or false, type(timer) == "table" and timer.remaining_s or nil
            -- Teardrop: a circle whose radius exceeds the distance to the ring never lines up on it
            -- (recorded: three laps at 47-190 m, radius ~110 m at 170 km/h). When the ring sits inside
            -- the turning circle, fly straight away to 2.3 radii, then turn at full bank and slow
            -- (140 km/h shrinks the radius), and the ring ends up on a straight final.
            local omega = math.rad(math.max(20, 24 * (horizontalSpeed / 75) ^ 0.6))
            local radius = horizontalSpeed / omega
            if turnback.extend == nil and not urgent and tbDist < 1.6 * radius and math.abs(courseError) > 60 then turnback.extend = true end
            if turnback.extend and tbDist > 2.0 * radius then
                turnback.extend, turnback.dir = false, courseError < 0 and -1 or 1
            end
            if turnback.extend then
                -- The outbound leg climbs: height buys bank (the wing limit near the ground is what
                -- made the circle too wide in the first place), so the return turn is tighter and
                -- shorter (video: the aircraft flew straight away from the marker and the job timer ran out).
                goalRoll, wantedYaw, goalSpeed = 0, 0, 150
                local climbTo = nav.marker_type == "checkpoint" and 30 or 60
                goalPitch = clamp(math.max(goalPitch, agl and agl < climbTo and 10 or (nav.marker_type == "checkpoint" and -2 or 2)), -5, 12)
                self.status = "Разворот: отход с набором высоты"
            else
                -- Height first: below 45 m the bank eases to 35 deg and the nose stays up (recorded:
                -- 55 deg of bank with the throttle closed lost 70 m in 5 s and ended in the water).
                local low = agl and agl < 45
                goalRoll, wantedYaw, goalSpeed = turnback.dir * (low and math.min(bankMax, 35) or bankMax), turnback.dir * 30, 170
                -- Hold the ring's height through the turn: too high is as much a miss as too low
                -- (simulated: +8 deg in a 60 deg bank climbed 150 m and the ring passed 40 m below).
                local high = finite(nav.altitude_error_m) and nav.altitude_error_m < -10
                goalPitch = clamp(math.max(goalPitch, high and -4 or (low and 6 or 2)), -6, high and 0 or 10)
                if nav.marker_type == "checkpoint" and agl then
                    goalPitch = math.min(goalPitch, agl > 60 and -6 or agl > 35 and -2 or agl > 22 and 2 or 8)
                end
                self.status = low and "Разворот на пропущенный маркер: набор высоты" or "Разворот на пропущенный маркер"
            end
            if agl and agl < 40 then goalPitch = math.max(goalPitch, 6) end
            if nav.marker_type == "checkpoint" and agl then
                -- Cylinders have no target altitude: maintain clearance instead of descending to
                -- their ground-level centres in the recovery turn.
                goalPitch = clamp((45 - agl) * 0.4 - data.climb_mps * 0.5 + trim, -4, 10)
            end
            self.phase = "turnback"
            self.detail.turnback_extend, self.detail.turnback_radius_m = turnback.extend or false, radius
            self.detail.airborne_hold = self.detail.airborne_hold or turnback.reason
            self.detail.turnback_dir, self.detail.turnback_ms = turnback.dir, elapsed(now, turnback.since)
            self.detail.turnback_bank_max, self.detail.turnback_reason = bankMax, turnback.reason
        end
        if self.finalLanding and agl then
            local clearance = math.max(0, agl - self.contactAgl)
            local predictedClearance = math.max(0, clearance + data.climb_mps)
            -- Sink up to 4.5 m/s while there is height, 1 m/s at the flare (recorded: 3 m/s max floated
            -- 145 m past the last ring and the wheels met the first corner at 73 km/h).
            local goalClimb = -clamp(1.0 + predictedClearance * 0.7, 1.0, 4.5)
            local flightPath = math.deg(math.atan2(data.climb_mps, math.max(20, horizontalSpeed)))
            local goalPath = math.deg(math.atan2(goalClimb, math.max(20, horizontalSpeed)))
            goalPitch = clamp(data.pitch_deg + (goalPath - flightPath) * 1.6, -9, 3)
            goalRoll, goalSpeed = clamp(goalRoll, -5, 5), math.min(110, math.max(95, arrivalLimit or 110))
            self.status = "Снижение до касания по направлению полосы"
            self.detail.landing_stage, self.detail.landing_heading_deg = "touchdown", self.landingHeading
            self.detail.wheel_clearance_m, self.detail.goal_climb_mps = clearance, goalClimb
            self.detail.predicted_wheel_clearance_m = predictedClearance
            self.detail.flight_path_deg, self.detail.goal_flight_path_deg = flightPath, goalPath
        end
        -- Aggressive arrival, recorded by hand: engine cut and nose up bleed 240 -> 70 km/h in about
        -- 250 m (~7 m/s^2); the aircraft touches down short of the checkpoint and rolls in, instead of
        -- a long slow final. Trigger distance is the stopping distance to ~70 km/h plus 60 m.
        -- Only for the yellow parking marker: the red chain is skimmed or rolled through.
        local landingTail = plan and plan.stopIndex == 2 and (not cornerNow or cornerNow.turn < 35)
        if self.aggressiveNow and self.landing and not self.aerobrake and not nav.awaiting_marker and not self.turnback
            and nav.marker_type == "checkpoint" and (yellowTarget or landingTail) and self.routeKnown
            and data.on_ground == false and agl and agl < 30 and math.abs(courseError) < 35 and finite(data.speed_kmh) then
            local v = data.speed_kmh / 3.6
            -- Trigger distance assumes 4 m/s^2 in the flare (the bot's, not the pilot's 7) plus 70 m.
            local stopDistance = landingTail and plan.distances[plan.stopIndex] or nav.distance_2d_m
            if stopDistance < (v * v - 400) / 8 + 70 then
                self.aerobrake = {since = now, speed = data.speed_kmh, distance = nav.distance_2d_m}
                -- From here it is a landing: after touchdown the rollout brakes hard toward the marker.
                self.finalLanding = true
                self.landingHeading = finite(nav.bearing_deg) and nav.bearing_deg or data.heading_deg
            end
        end
        if self.aerobrake and (nav.marker_type ~= "checkpoint" or data.on_ground == true
            or elapsed(now, self.aerobrake.since) > 20000 or (agl and agl > 40)) then self.aerobrake = nil end
        if self.aerobrake then
            -- The engine cut is a pulse, not a state (recorded by hand): off while there is speed to
            -- shed, back on below 130 km/h or near the deck so the aircraft stays controllable through
            -- the touchdown. Below 100 km/h with height left the nose comes down to avoid a stall drop.
            -- Below 110 km/h the nose comes down and the touchdown law keeps its pitch; holding it up
            -- floats the aircraft in ground effect instead of putting the wheels down (recorded).
            local slow = data.speed_kmh < 75  -- the flare bleeds speed down to a 70 km/h touchdown, as flown by hand
            -- Flare pitch eases with height so the aircraft does not balloon over the checkpoint
            -- (recorded: +10 deg at 113 km/h lifted it from 4 to 12 m and past the marker).
            local height = agl or 0
            -- Fast and high the nose still goes up: shedding speed matters more than the balloon
            -- (recorded: 4 deg at 14 m left 184 km/h to overfly the yellow marker at 121).
            if not slow then goalPitch = data.speed_kmh > 150 and 10 or height > 10 and 4 or height > 7 and 8 or 12 end
            goalRoll, goalSpeed = clamp(goalRoll, -8, 8), 0
            -- Cut only with the nose already up: engine off in level or nose-down flight turns the
            -- aircraft into a falling body that accelerates instead of slowing (recorded by hand).
            -- Hysteresis lets the cut repeat while the nose stays high, like a pilot pulsing the key.
            -- Pulsed, as flown by hand (recorded: 0.4-0.6 s cuts at 240, 161 and 93 km/h, nose from level
            -- to +16 deg): half a second off, half a second on, while there is speed to shed, the nose
            -- is not down and the aircraft is not sinking fast. Never below 100 km/h or 3 m.
            local age = elapsed(now, self.aerobrake.since)
            local cut = data.speed_kmh > 100 and agl and agl > 3 and data.pitch_deg > 2 and data.climb_mps > -8
                and age >= 300 and (age % 1000) < 500 or false
            self.aerobrake.cut = cut
            out.engine_off = cut
            self.detail.aerobrake_engine_cut = cut
            self.gearDeployReason = self.gearDeployReason or "aerobrake"
            self.phase = "aerobrake"
            self.status = slow and "Агрессивная посадка: скорость сброшена, выравнивание"
                or "Агрессивная посадка: двигатель выключен, нос вверх"
            self.detail.aerobrake_ms, self.detail.aerobrake_trigger_speed_kmh = elapsed(now, self.aerobrake.since), self.aerobrake.speed
            self.detail.aerobrake_trigger_distance_m = self.aerobrake.distance
        end
        local diving = data.pitch_deg < -30
        local recovery = math.abs(data.roll_deg) > ((turnback or skim) and 80 or 65) or diving
        if recovery then
            goalRoll, goalPitch = 0, clamp(goalPitch, -3, 5)
            self.status = diving and "Вывод из пикирования" or "Выравнивание большого крена"
        end
        if math.abs(data.roll_deg) > 75 or data.pitch_deg < -45 then
            -- Upset in flight: the recovery law keeps working; letting go here is the one sure way to crash.
            self.detail.airborne_hold = "attitude_recovery"
        end
        local bank, pitch = math.rad(data.roll_deg), math.rad(data.pitch_deg)
        local pitchCommand = clamp((goalPitch - data.pitch_deg) * 1.4 - pitchRate * 0.6, -14, 14)
        local yawCommand = wantedYaw + clamp((wantedYaw - yawRate) * 0.4, -3, 3)
        -- This game turns far faster than g*tan(bank)/v (recorded: 28 deg/s at 65 deg bank, 270 km/h);
        -- the aerodynamic cap stays for the gentle intercept only, never for a turn-back.
        local achievableYaw = (turnback or skim) and 40 or (7 + math.deg(9.81 * math.tan(math.rad(bankLimit)) / math.max(25, horizontalSpeed)))
        yawCommand = clamp(yawCommand, -achievableYaw, achievableYaw)
        if recovery then yawCommand = 0 end
        -- Rotate desired world pitch/heading rates into the banked aircraft axes.
        local turnPull = yawCommand * math.cos(pitch) * math.sin(bank)
        local pitchAxis = pitchCommand * math.cos(bank) + turnPull
        local yawAxis = yawCommand * math.cos(pitch) * math.cos(bank) - pitchCommand * math.sin(bank)
        -- Roll damping doubled: the level-off after a marker change overshot +-5 deg for a second
        -- (recorded), which reads as rocking from the cockpit.
        local rollProportional, rollDamping = (goalRoll - data.roll_deg) * 0.040, -rollRate * 0.016
        out.aileron = clamp(rollProportional + rollDamping, -0.85, 0.85)
        out.elevator = clamp(pitchAxis / (pitchAxis >= 0 and 30.5 or 21), -0.7, 0.7)
        -- In a turn-back the coordination pull must not run the nose away from its goal (simulated:
        -- 60 deg of bank pulled the pitch to 20 deg and 100 m of climb; the ring then passed below).
        if data.pitch_deg > goalPitch + 6 then out.elevator = math.min(out.elevator, -0.05)
        elseif data.pitch_deg < goalPitch - 8 then out.elevator = math.max(out.elevator, 0.15) end
        if turnback and data.pitch_deg > goalPitch + 4 then out.elevator = math.min(out.elevator, 0) end
        self.detail.pitch_limited = data.pitch_deg > goalPitch + 6 or data.pitch_deg < goalPitch - 8
        local bankYaw = math.deg(9.81 * math.tan(math.rad(math.abs(data.roll_deg))) / math.max(25, horizontalSpeed))
        local rudderCap = (rudderTurn or not turnback and math.abs(wantedYaw) > bankYaw + 2) and 1 or 0.35
        out.rudder = recovery and 0 or clamp(yawAxis / 12.4, -rudderCap, rudderCap)
        self.detail.rudder_turn = rudderTurn
        -- Q/E are held down while the rudder turns the aircraft (recorded by hand: long holds, not
        -- taps); the pulse-width mode stays for fine trimming only.
        if rudderTurn and math.abs(out.rudder) >= 0.25 then self.detail.rudder_control = "hold" end
        if nav.marker_type == "ring" and not turnback and not pass and alongTrack > 0 and math.abs(courseError) > 15 then
            -- Closing time must cover both the heading correction and roll-in, especially low over a runway.
            local availableYaw = 7 + math.deg(9.81 * math.tan(math.rad(bankLimit)) / math.max(25, horizontalSpeed))
            local seconds = 1.0 + 2 * math.abs(courseError) / math.max(3, availableYaw)
            local interceptLimit = math.max(125, alongTrack / seconds * 3.6)
            goalSpeed = math.min(goalSpeed, interceptLimit)
            self.detail.intercept_speed_limit_kmh = interceptLimit
        end
        local speedError = goalSpeed - data.speed_kmh
        if speedError > -2 then out.throttle = clamp(0.65 + speedError * 0.04, 0, 1)
        elseif speedError < -5 then out.brake = clamp(-speedError * 0.025, 0, self.landing and 0.8 or 0.35) end
        if self.finalLanding or diving then out.throttle = 0 end
        -- The turn-back flies on power: the bank bleeds speed by itself, the throttle holds the height.
        if self.turnbackPower then out.throttle = math.max(out.throttle or 0, 0.8); out.brake = 0 end
        self.turnbackPower = nil
        local contactSeconds = agl and data.climb_mps < -0.5
            and math.max(0, agl - self.contactAgl) / -data.climb_mps or nil
        if self.landing then
            if not self.gearDeployReason then
                if self.finalLanding then self.gearDeployReason = "final_landing"
                elseif not agl then self.gearDeployReason = "agl_unavailable"
                elseif data.landing_gear_down == true then self.gearDeployReason = "already_down_on_approach"
                -- Gear costs ~50 km/h (recorded: 267 -> 218 right after extension); aggressive keeps it
                -- up until the last 15 m / 4 s, the way the pilot flies it.
                elseif agl <= (self.aggressiveNow and 15 or 60) and not skim then self.gearDeployReason = "approach_altitude"
                elseif agl <= 100 and contactSeconds and contactSeconds <= (self.aggressiveNow and 4 or 10) and not skim then self.gearDeployReason = "descent_time" end
            end
            out.gear_down = self.gearDeployReason ~= nil
            -- A missed ring is re-flown with the gear up: it costs ~50 km/h and the turn needs the
            -- speed (recorded: gear out, 160 km/h, three laps). It comes back out once aligned,
            -- through the same deploy reasons, or at once below 12 m.
            if turnback and not self.finalLanding and agl and agl > 12 then
                out.gear_down = false
                self.gearDeployReason = nil
                self.detail.gear_command_reason = "turnback_retract"
            end
        elseif agl and (self.aggressiveNow or agl > 8 and elapsed(now, self.airSince) > 1000) then out.gear_down = false end
        self.detail.gear_command_reason = self.detail.gear_command_reason or self.gearDeployReason or (self.landing and "approach_wait" or nil)
        self.detail.gear_contact_linear_s, self.detail.gear_deploy_agl_m = contactSeconds, self.landing and 60 or nil
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
local VERSION = "1.2.116"
-- The server speedometer shows |velocity| * 127 (Spee.lua); telemetry keeps physical game metres per
-- second (velocity * 50, i.e. * 180 for km/h). The HUD shows the number the pilot is used to.
local SPEED_DISPLAY_MUL = 0.702  -- 270 ours = 189.5 on the server speedometer (measured)
-- Engine toggle keybind of this server (the pilot pulses it by hand during the aerobrake).
local ENGINE_KEY = "z"

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
    controlSince = {}, hudEnabled = false, hudMode = "full", aggressive = false, autonomy = false,
    safetyAlerts = {}, safetyContacts = setmetatable({}, {__mode = "k"}),
    safetyNearby = setmetatable({}, {__mode = "k"}),
    learnedChains = {},
    status = "Готов к ручному полёту. Нажми «Начать запись».",
}
-- (declared after state and NULL: memory lookups use both)
-- Marker memory. The server creates the job's markers one at a time, so the scanner never sees more
-- than the arrow's one step ahead; memory of chains flown before gives the controller the whole
-- chain, and the yellow passenger marker in particular, before it exists. {x, y, z, radius, yellow}.
-- No built-in chains: the bot knows an airport only after flying it once in this session
-- (by hand or cautiously); the second visit gets the plan. {x, y, z, radius, yellow, permit}
local MARKER_MEMORY = {}

-- The chain after the marker at `position`, from memory or from chains learned this session. The
-- arrow of the current marker must agree with memory, otherwise the route differs and memory is ignored.
local function memoryChainAfter(position, arrow)
    -- A ring of a flight flown before: the rest of that flight's rings and its ground chain follow.
    -- This is what makes the route known from the first descent ring, not from the last one
    -- (recorded: with only the last ring's look-ahead the whole approach flew the cautious law).
    local function searchRings(chains)
        for _, chain in ipairs(chains) do
            for i, ring in ipairs(chain.rings or {}) do
                if math.abs(ring[1] - position[1]) < 3 and math.abs(ring[2] - position[2]) < 3 then
                    local nextRing = chain.rings[i + 1]
                    local expected = nextRing or chain.points[1]
                    if expected and type(arrow) == "table" and arrow ~= NULL
                        and (math.abs(expected[1] - arrow[1]) > 5 or math.abs(expected[2] - arrow[2]) > 5) then return nil end
                    local rest = {}
                    for j = i + 1, #chain.rings do
                        local q = chain.rings[j]
                        rest[#rest + 1] = {position = {q[1], q[2], q[3]}, size_m = 30, marker_type = "ring", yellow = false}
                    end
                    for _, q in ipairs(chain.points) do
                        rest[#rest + 1] = {position = {q[1], q[2], q[3]}, size_m = q[4], marker_type = "checkpoint", yellow = q[5] == true, permit = q[6] == true, direct = q[7] == true}
                    end
                    if #rest > 0 then return rest, chain.name end
                end
            end
        end
    end
    local ringRest, ringName = searchRings(state.learnedChains)
    if ringRest then return ringRest, ringName end
    local function search(chains)
        for _, chain in ipairs(chains) do
            for i, p in ipairs(chain.points) do
                if math.abs(p[1] - position[1]) < 3 and math.abs(p[2] - position[2]) < 3 then
                    local nextPoint = chain.points[i + 1]
                    if not nextPoint or (type(arrow) == "table" and arrow ~= NULL
                        and (math.abs(nextPoint[1] - arrow[1]) > 5 or math.abs(nextPoint[2] - arrow[2]) > 5)) then return nil end
                    local rest = {}
                    for j = i + 1, #chain.points do
                        local q = chain.points[j]
                        rest[#rest + 1] = {position = {q[1], q[2], q[3]}, size_m = q[4], marker_type = "checkpoint", yellow = q[5] == true, permit = q[6] == true, direct = q[7] == true}
                    end
                    return rest, chain.name, p[7] == true
                end
            end
        end
    end
    -- Chains learned this session first: they are the ones that unlock the aggressive plan.
    local rest, name = search(state.learnedChains)
    if rest then return rest, name end
    return search(MARKER_MEMORY)
end
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
    return (state.apSessionActive or autopilot.enabled) and state.lastOnGround == false
end

local function syncSafetyMonitor()
    local active = safetyActive()
    if state.safetyMonitorActive == active then return end
    state.safetyMonitorActive = active
    native.alertMonitor(active)
    if not active then
        state.safetyNearby = setmetatable({}, {__mode = "k"})
        state.streamedPlayers, state.streamedPlayersAt = nil, nil
    end
end

local function safetyAlert(key, cooldown, text, data)
    if not safetyActive() then return false end
    local now = getTickCount()
    if state.safetyAlerts[key] and elapsed(now, state.safetyAlerts[key]) < cooldown then return false end
    state.safetyAlerts[key] = now
    local played = native.alert() == true
    -- Both recorded autopilot stalls followed an alert; the timings below show whether the native
    -- sound call itself is the cost or whether the stall happens in the engine afterwards.
    local playCallMs = elapsed(getTickCount(), now)
    if text then outputChatBox("#FF5555[Pilot Safety] #FFFFFF" .. text, 255, 255, 255, true) end
    -- Buffered, not forced: the alert frame is already the heaviest one of the session (sound + chat).
    -- The buffer lands on disk within 250 ms anyway.
    emit("safety_alert", {kind = key, text = text or NULL, played = played, context = data or NULL,
        play_call_ms = playCallMs, handler_ms = elapsed(getTickCount(), now)})
    return played
end

local function cleanNick(value)
    return tostring(value or ""):gsub("#%x%x%x%x%x%x", "")
end

local function playerNick(player)
    return cleanNick(read("getPlayerNametagText", player) or read("getPlayerName", player) or "?")
end

-- Only people close enough to actually watch the aircraft matter; 200 m flagged half the airfield.
local SAFETY_RADIUS_M = 50
local SAFETY_RADIUS_SQ = SAFETY_RADIUS_M * SAFETY_RADIUS_M
local SAFETY_LIST_CACHE_MS = 500

local function localWorld()
    return read("getElementDimension", localPlayer), read("getElementInterior", localPlayer)
end

local function sameWorld(player, dimension, interior)
    if dimension == nil then dimension, interior = localWorld() end
    return read("getElementDimension", player) == dimension
        and read("getElementInterior", player) == interior
end

-- getElementsByType allocates a fresh table every call; a chat burst used to pay for one per line.
local function streamedPlayers(now)
    now = now or getTickCount()
    if state.streamedPlayers and elapsed(now, state.streamedPlayersAt) < SAFETY_LIST_CACHE_MS then
        return state.streamedPlayers
    end
    state.streamedPlayers = read("getElementsByType", "player", root, true) or {}
    state.streamedPlayersAt = now
    return state.streamedPlayers
end

local function nearbyPlayerById(id)
    id = tostring(id)
    local dimension, interior = localWorld()
    for _, player in ipairs(streamedPlayers()) do
        -- id first: one read instead of three for the players that cannot match anyway.
        if player ~= localPlayer
            and tostring(read("getElementData", player, "id") or "") == id
            and sameWorld(player, dimension, interior) then
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
    -- Stamped before the position guard: an unreadable local position used to leave the timer
    -- untouched, so the scan re-entered on every single frame instead of every 2 s.
    state.lastSafetyScan = now
    local px, py, pz = read("getElementPosition", localPlayer)
    if not finite(px) or not finite(py) or not finite(pz) then return end
    local position = _G.getElementPosition
    if type(position) ~= "function" then return end

    -- Hot loop: one pcall per player instead of three, squared distances instead of sqrt,
    -- and the local dimension/interior pair read once instead of once per streamed player.
    local dimension, interior = localWorld()
    local known = state.safetyNearby
    local current = setmetatable({}, {__mode = "k"})
    local nearest, nearestSquared
    for _, player in ipairs(streamedPlayers(now)) do
        if player ~= localPlayer then
            local ok, x, y, z = pcall(position, player)
            if ok and finite(x) and finite(y) and finite(z) then
                local dx, dy, dz = x - px, y - py, z - pz
                local squared = dx * dx + dy * dy + dz * dz
                -- The expensive checks run only for the handful inside the radius.
                if squared <= SAFETY_RADIUS_SQ and valid(player)
                    and sameWorld(player, dimension, interior) then
                    current[player] = true
                    if not known[player] and (not nearestSquared or squared < nearestSquared) then
                        nearest, nearestSquared = player, squared
                    end
                end
            end
        end
    end
    state.safetyNearby = current
    if nearest then
        local nick = playerNick(nearest)
        local distance = math.sqrt(nearestSquared)
        safetyAlert("player_near", 30000,
            string.format("Человек рядом: %s (%.1f м)", nick, distance),
            {player = elementId(nearest), name = nick, distance_m = distance,
                radius_m = SAFETY_RADIUS_M})
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

-- BEGIN AIM MEMORY
-- Declared ahead: aimOffsetAt below needs both, and they are defined further down.
local markerVisitAt, studyOffset
local function aimMemoryAt(x, y)
    for _, a in ipairs(state.aimMemory) do
        if math.abs(a.x - x) < 3 and math.abs(a.y - y) < 3 then return a end
    end
end

local function aimOffsetAt(position)
    if type(position) ~= "table" or not finite(position[1]) or not finite(position[2]) then return nil end
    local a = aimMemoryAt(position[1], position[2])
    return a and {a.ox, a.oy, hits = a.hits, via = a.via and {a.via[1], a.via[2]} or nil} or nil
end

local function aimMemoryNear(x, y, radius)
    for _, a in ipairs(state.aimMemory) do
        if (a.x - x)^2 + (a.y - y)^2 < radius * radius then return true end
        if a.via and (a.via[1] - x)^2 + (a.via[2] - y)^2 < radius * radius then return true end
    end
    return false
end

-- Visits and leg times per marker (position-keyed, session-wide): how often a marker was taken and
-- how long the leg to it took, last and best. Three clean visits let the probe go lean.
markerVisitAt = function(x, y)
    if not state.markerVisits then return nil end
    for _, v in ipairs(state.markerVisits) do
        if math.abs(v.x - x) < 3 and math.abs(v.y - y) < 3 then return v end
    end
end

-- BEGIN ROUTE STUDY
-- The bot compares its own alternatives on the real route and keeps only what wins.
--  * Median, never the record: selecting by best time picks lucky noise, not a better line.
--  * A BLOCK is measured (the leg into the marker plus the leg out of it), so a change that saves
--    a second here and costs three at the next corner registers as a loss.
--  * Paired A/B with alternating order cancels drift between flights (FPS, server load).
--  * The candidate is a PACE multiplier for the block. The recorded 182 m block ran 10.5 s behind
--    what its length allows, while a 4 m aim shift moved it by 0.24 s: pace is where the seconds
--    are. Every physical limit is applied after the multiplier.
--  * Safety comes from the admission rules and the live probe, never from the statistic.
local STUDY = {pace = 1.15, pairsNeeded = 3, minGain = 0.6, minSamples = 3,
    timerFloor = 8, keepTimes = 8, cruise = 26, maxPace = 2.0}

local function median(values)
    local n = #values
    if n == 0 then return nil end
    local sorted = {}
    for i = 1, n do sorted[i] = values[i] end
    table.sort(sorted)
    if n % 2 == 1 then return sorted[(n + 1) / 2] end
    return (sorted[n / 2] + sorted[n / 2 + 1]) / 2
end

-- One marker under study at a time: the cause of any change stays unambiguous.
local function studyMarker()
    for _, v in ipairs(state.markerVisits) do
        if v.study then return v end
    end
end

-- Admission rules. These, not the statistics, are what protect the flight.
local function mayStudy(v)
    if v.locked or v.study then return false end
    if not v.blockTimes or #v.blockTimes < STUDY.minSamples then return false end
    if (v.hits or 0) > 0 then return false end
    if v.minTimer and v.minTimer < STUDY.timerFloor then return false end
    if v.yellow or v.ring then return false end
    if (v.pace or 1) >= STUDY.maxPace then return false end
    return true
end

-- Priority: how far the block runs behind what its own length allows. A long leg is not a
-- problem; a long leg for its distance is.
local function studyPriority(v)
    local m = median(v.blockTimes)
    if not m or not v.blockLength or v.blockLength < 20 then return nil end
    return m - v.blockLength / STUDY.cruise
end

local function openStudy()
    if studyMarker() then return end
    local best, bestScore
    for _, v in ipairs(state.markerVisits) do
        if mayStudy(v) then
            local score = studyPriority(v)
            if score and score > 1.5 and (not bestScore or score > bestScore) then best, bestScore = v, score end
        end
    end
    if not best then return end
    best.study = {pairs = {}, phase = "B", flip = false}
    emit("study_open", {position = {best.x, best.y}, behind_s = bestScore,
        median_s = median(best.blockTimes), length_m = best.blockLength, pace = best.pace or 1}, true)
end

-- Pace offered to the controller for this marker: the proven one, times the candidate while it is
-- being tried.
studyOffset = function(v)
    local pace = v.pace or 1
    if v.study and v.study.phase == "B" then pace = pace * STUDY.pace end
    return pace
end

-- One paired comparison finished.
local function studyPair(v, tA, tB)
    local s = v.study
    local d = tA - tB
    s.pairs[#s.pairs + 1] = d
    emit("study_pair", {position = {v.x, v.y}, a_s = tA, b_s = tB, gain_s = d, pairs = #s.pairs}, true)
    if d <= 0 then
        v.study, v.locked = nil, true
        emit("study_done", {position = {v.x, v.y}, result = "no_gain", pace = v.pace or 1}, true)
        return
    end
    if #s.pairs >= STUDY.pairsNeeded then
        local gain = median(s.pairs)
        if gain >= STUDY.minGain then
            v.pace = (v.pace or 1) * STUDY.pace
            v.blockTimes = {}
            v.study = nil
            -- A confirmed gain is worth trying again from the new baseline.
            emit("study_done", {position = {v.x, v.y}, result = "accepted", gain_s = gain, pace = v.pace}, true)
        else
            v.study, v.locked = nil, true
            emit("study_done", {position = {v.x, v.y}, result = "gain_too_small", gain_s = gain}, true)
        end
    end
end
-- END ROUTE STUDY

local function recordMarkerVisit(position, seconds, markerType, r, g, b)
    if type(position) ~= "table" or not finite(position[1]) or not finite(position[2]) then return end
    local v = markerVisitAt(position[1], position[2])
    if not v then v = {x = position[1], y = position[2], count = 0}; state.markerVisits[#state.markerVisits + 1] = v end
    v.count = v.count + 1
    v.ring = markerType == "ring"
    v.yellow = (finite(r) and finite(g) and finite(b) and r > 200 and g > 160 and b < 80) or false
    local known = aimMemoryAt(v.x, v.y)
    v.hits = known and known.hits or 0
    -- Direction of the approach: the study offsets ACROSS it.
    local before = state.legPrevMarker
    if before and (before.x ~= v.x or before.y ~= v.y) then
        v.inbound = {v.x - before.x, v.y - before.y}
    end
    if finite(seconds) and seconds > 0.5 and seconds < 120 then
        v.last_s = seconds
        v.best_s = v.best_s and math.min(v.best_s, seconds) or seconds
        state.lastLeg = {seconds = seconds, best = v.best_s, visits = v.count}
        -- Close the BLOCK of the previous marker: its own leg plus this one.
        local prev = state.legPrevMarker
        if prev and state.legPrevSeconds then
            local block = state.legPrevSeconds + seconds
            local inLen = prev.inbound and math.sqrt(prev.inbound[1]^2 + prev.inbound[2]^2) or 0
            prev.blockLength = math.sqrt((v.x - prev.x)^2 + (v.y - prev.y)^2) + inLen
            prev.minTimer = state.legTimerMin
            local s = prev.study
            if s then
                if s.phase == "B" then s.tB = block else s.tA = block end
                if s.tA and s.tB then
                    local tA, tB = s.tA, s.tB
                    s.tA, s.tB = nil, nil
                    s.flip = not s.flip
                    studyPair(prev, tA, tB)
                    if prev.study then prev.study.phase = prev.study.flip and "A" or "B" end
                else
                    s.phase = s.phase == "B" and "A" or "B"
                end
            else
                prev.blockTimes = prev.blockTimes or {}
                prev.blockTimes[#prev.blockTimes + 1] = block
                while #prev.blockTimes > STUDY.keepTimes do table.remove(prev.blockTimes, 1) end
                prev.blockMedian = median(prev.blockTimes)
            end
        end
        state.legPrevMarker, state.legPrevSeconds = v, seconds
        state.legPrevLength = 0
        state.legTimerMin = nil
        openStudy()
        emit("leg_time", {position = {v.x, v.y}, seconds = seconds, best_s = v.best_s, visits = v.count}, true)
    end
end

-- Move one marker's aim by (dx, dy), clamped to 0.8 of its radius. Returns the distance moved.
local function shiftAim(x, y, z, size, dx, dy, base)
    local a = aimMemoryAt(x, y)
    if not a then
        a = {x = x, y = y, z = finite(z) and z or 0, size = finite(size) and size > 0 and size or 30, ox = 0, oy = 0, hits = 0}
        -- The first lesson starts from where the bot was actually aiming (the corner cut), not from
        -- the centre: moving back to the centre could be a step toward the obstacle.
        if type(base) == "table" and finite(base[1]) and finite(base[2]) then a.ox, a.oy = base[1] - x, base[2] - y end
        state.aimMemory[#state.aimMemory + 1] = a
    end
    local ox, oy = a.ox + dx, a.oy + dy
    local limit = a.size * 0.8
    local len = math.sqrt(ox * ox + oy * oy)
    if len > limit then ox, oy = ox / len * limit, oy / len * limit end
    local moved = math.sqrt((ox - a.ox)^2 + (oy - a.oy)^2)
    a.ox, a.oy, a.hits = ox, oy, a.hits + 1
    return moved, a
end

local function learnAim(lesson)
    if type(lesson) ~= "table" or not finite(lesson[1]) or not finite(lesson[2]) or not finite(lesson.dx) or not finite(lesson.dy) then return end
    local moved, a = shiftAim(lesson[1], lesson[2], lesson[3], lesson.size, lesson.dx, lesson.dy, lesson.base)
    local escalated = false
    -- A hit well before the marker: the obstacle sits on the leg, so a VIA point beside the place of
    -- the hit is learned (and moved further by every later hit there); the marker's aim moves too.
    if type(lesson.hit) == "table" and finite(lesson.hit[1]) and finite(lesson.hit[2]) and finite(lesson.distance) and lesson.distance > 25 then
        if a.via then a.via = {a.via[1] + lesson.dx, a.via[2] + lesson.dy}
        else a.via = {lesson.hit[1] + lesson.dx, lesson.hit[2] + lesson.dy} end
    end
    -- Saturated: the same shift goes to the marker before, so the approach line itself moves.
    if moved < 3 and type(lesson.previous) == "table" and finite(lesson.previous[1]) and finite(lesson.previous[2]) then
        shiftAim(lesson.previous[1], lesson.previous[2], lesson.previous[3], lesson.previous_size, lesson.dx, lesson.dy)
        escalated = true
    end
    emit("aim_learned", {position = {a.x, a.y}, offset = {a.ox, a.oy}, hits = a.hits, moved_m = moved,
        escalated_previous = escalated, via = a.via or NULL, reason = lesson.reason or NULL}, true)
end
-- END AIM MEMORY

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
        -- Checkpoint arrows point at the next marker: that is the look-ahead the ground planner needs.
        nav.next_position = (nav.marker_type == "ring" or nav.marker_type == "checkpoint")
            and vector("getMarkerTarget", state.target) or NULL
        -- The chain ahead: from memory (chains flown before or learned this session), verified against
        -- the arrow; otherwise the arrow's single step with its type unknown.
        local memory, source, direct = memoryChainAfter(targetPosition, nav.next_position)
        nav.direct = direct == true
        if not memory and nav.marker_type == "ring" and nav.next_position ~= NULL then
            -- The last rings point at the runway checkpoint: if a remembered chain starts there, the
            -- whole ground route is known already on the approach.
            local rest, name = memoryChainAfter(nav.next_position, nil)
            if rest and #rest > 0 then
                memory, source = {{position = {nav.next_position[1], nav.next_position[2], nav.next_position[3]},
                    size_m = NULL, marker_type = "checkpoint", yellow = false}}, name
                for _, m in ipairs(rest) do memory[#memory + 1] = m end
            end
        end
        nav.aim_offset = aimOffsetAt(targetPosition) or NULL
        local visit = markerVisitAt(targetPosition[1], targetPosition[2])
        nav.study_pace = visit and studyOffset(visit) or NULL
        if memory then
            for _, m in ipairs(memory) do m.aim_offset = aimOffsetAt(m.position) or NULL end
        end
        if memory and #memory > 0 then
            nav.chain_ahead, nav.chain_source = memory, source
        elseif nav.next_position ~= NULL then
            nav.chain_ahead = {{position = {nav.next_position[1], nav.next_position[2], nav.next_position[3]},
                size_m = NULL, marker_type = NULL, yellow = false, unknown = true}}
            nav.chain_source = "arrow"
        else
            nav.chain_ahead, nav.chain_source = NULL, NULL
        end
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
            current = state.waypointSnapshot, color_rgba = nav.color_rgba, marker_size_m = nav.marker_size_m or NULL}, true)
        -- Learn the chain as it is flown: checkpoints in order, the yellow one closing it. A chain
        -- learned on the way out serves the way back and every later cycle of the same job.
        do
            local tick = getTickCount()
            recordMarkerVisit(targetPosition, state.legTick and elapsed(tick, state.legTick) / 1000 or nil, nav.marker_type, r, g, b)
            state.legTick = tick
        end
        if nav.marker_type == "checkpoint" then
            local yellow = finite(r) and finite(g) and finite(b) and r > 200 and g > 160 and b < 80
            local chain = state.learningChain
            if not chain then
                -- The rings flown since the last ground chain belong to this arrival: the route is
                -- recognised from its first ring next time.
                chain = {name = "learned_" .. tostring(state.waypointNumber), points = {}, rings = state.recentRings or {}}
                state.learningChain, state.recentRings = chain, {}
            end
            chain.points[#chain.points + 1] = {targetPosition[1], targetPosition[2], targetPosition[3], number(nav.marker_size_m) or 30, yellow}
            if yellow then
                state.learnedChains[#state.learnedChains + 1] = chain; state.learningChain = nil
                emit("chain_learned", {name = chain.name, points = #chain.points, rings = #(chain.rings or {}), reason = "yellow"}, true)
            end
        else
            state.learningChain = nil
            if nav.marker_type == "ring" then
                state.recentRings = state.recentRings or {}
                state.recentRings[#state.recentRings + 1] = {targetPosition[1], targetPosition[2], targetPosition[3]}
                if #state.recentRings > 40 then table.remove(state.recentRings, 1) end
            end
        end
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
        -- On foot the recorder still logs where the player is: the walk from the aircraft to the job
        -- pickup is the route the autonomy will follow.
        item.foot = {position = vector("getElementPosition", localPlayer) or NULL,
            rotation = number(read("getPedRotation", localPlayer)) or NULL,
            interior = read("getElementInterior", localPlayer), dimension = read("getElementDimension", localPlayer),
            move_state = read("getPedMoveState", localPlayer) or NULL, health = read("getElementHealth", localPlayer)}
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
        if (state.recording or autopilot.enabled or state.hudEnabled) and elapsed(now, state.lastScan) >= 250 then
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
    bridge.update("hud_mode", state.hudMode or "full")
    bridge.update("autopilot_aggressive", state.aggressive and "1" or "0")
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
        aggressive = state.aggressive, speed_display_multiplier = SPEED_DISPLAY_MUL * 180,
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
        safety = {player_radius_m = SAFETY_RADIUS_M, scan_ms = 2000, alert_cooldown_ms = 30000,
            player_list_cache_ms = SAFETY_LIST_CACHE_MS},
        unavailable_or_errors = state.failures}, true)
    emit("job_context", {destination = state.destination or NULL, notification = state.lastJobNotification or NULL,
        observed_tick_ms = state.jobNotificationTick or NULL, instruction = autopilot.instruction,
        notification_count = state.jobNotificationCount or 0, cached_before_recording = true}, true)
    pollNotifications()
    bridge.update("recording", state.recording and "1" or "0")
end

-- "Трудоустроиться" from the panel: the same honest cycle the autonomy runs (step off the pickup,
-- step back on, press "Работать" in the window), started by hand. Nothing is sent to the server
-- directly; the window's own button handler does the talking.
local function acceptJob()
    if state.nextJob then return end
    if not state.pilotReady then
        state.status = "Трудоустройство недоступно: province_pilot не запущен."
        return
    end
    if read("getPedOccupiedVehicle", localPlayer) then
        state.status = "Трудоустройство: сначала выйди из машины и встань на метку работы."
        return
    end
    state.nextJob = {stage = "wait_exit", began = getTickCount(), manual = true}
    autopilot.status = "Трудоустройство: заход на метку работы"
    state.status = "Трудоустройство: заход на метку работы"
    bridge.update("autopilot_waiting", "1")
    return true
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

-- The job pickup (the shirt "Пилот") inside the terminal, interior 8. After a job ends the player is
-- spawned on it; it fires again only after stepping off and back on.
local PICKUP = {1105.06, 1050.027, 1795.672, interior = 8}

local function stopWalking()
    read("setPedControlState", localPlayer, "forwards", false)
    read("setPedControlState", localPlayer, "sprint", false)
    if state.walkCamera then read("setCameraTarget", localPlayer); state.walkCamera = nil end
end

-- Walk the local player toward a point. Movement follows the camera, so the camera sits behind the
-- player looking at the target; knee- and chest-height probes ahead trip a sidestep around a pole.
local function walkToward(target)
    local px, py, pz = read("getElementPosition", localPlayer)
    if not finite(px) or not finite(py) or not finite(pz) then return nil end
    local dx, dy = target[1] - px, target[2] - py
    local distance = math.sqrt(dx * dx + dy * dy)
    if distance < 0.7 then stopWalking(); return true, distance end
    local heading = math.atan2(dx, dy)
    local function clear(offset)
        local a = heading + offset
        local ax, ay = px + math.sin(a) * 1.6, py + math.cos(a) * 1.6
        return read("isLineOfSightClear", px, py, pz - 0.4, ax, ay, pz - 0.4, true, false, false, true, false, false, false, localPlayer) ~= false
            and read("isLineOfSightClear", px, py, pz + 0.4, ax, ay, pz + 0.4, true, false, false, true, false, false, false, localPlayer) ~= false
    end
    local offset = 0
    if not clear(0) then
        offset = clear(math.rad(50)) and math.rad(50) or clear(math.rad(-50)) and math.rad(-50) or math.rad(120)
    end
    local look = heading + offset
    read("setCameraMatrix", px - math.sin(look) * 3, py - math.cos(look) * 3, pz + 1.3,
        px + math.sin(look) * 6, py + math.cos(look) * 6, pz + 0.6)
    state.walkCamera = true
    read("setPedControlState", localPlayer, "forwards", true)
    read("setPedControlState", localPlayer, "sprint", distance > 3)
    return false, distance, offset ~= 0
end

-- The job pickup as the pilot resource created it: the nearest pickup element within 30 m in the
-- player's interior, under the pilot root first, anywhere second. The spawn point is only near it.
local function nearestJobPickup(px, py)
    local interior = read("getElementInterior", localPlayer)
    local best, bestDistance
    for _, scope in ipairs({state.pilotRoot, root}) do
        if valid(scope) then
            for _, pickup in ipairs(read("getElementsByType", "pickup", scope) or {}) do
                local x, y, z = read("getElementPosition", pickup)
                if finite(x) and finite(y) and finite(z) and read("getElementInterior", pickup) == interior then
                    local d = math.sqrt((x - px)^2 + (y - py)^2)
                    if d < 30 and (not bestDistance or d < bestDistance) then best, bestDistance = {x, y, z}, d end
                end
            end
            if best then return best, bestDistance end
        end
    end
    return nil
end


-- The job window is built from hdx elements of the pilot resource, but their text and screen
-- rectangles live inside province_dx (hdxSetData is an event into that resource, hdxGetData its
-- export). Ask it the honest way; remember which access path this VM has.
local function hdxData(element, key)
    if not valid(element) then return nil end
    local ok, value = pcall(function() return exports.province_dx:hdxGetData(element, key) end)
    if ok then state.hdxAccess = "exports"; return value end
    local dx = read("getResourceFromName", "province_dx")
    if dx then
        local okCall, called = pcall(call, dx, "hdxGetData", element, key)
        if okCall then state.hdxAccess = "call"; return called end
        value = called
    end
    state.hdxAccess = "none: " .. tostring(value):sub(1, 80)
    return nil
end

local function hdxButtonInfo(button)
    local data, position, size = hdxData(button, "data"), hdxData(button, "position"), hdxData(button, "size")
    local text = type(data) == "table" and data.text or nil
    local x = type(position) == "table" and number(position.x) or nil
    local y = type(position) == "table" and number(position.y) or nil
    local w = type(size) == "table" and number(size.w) or nil
    local h = type(size) == "table" and number(size.h) or nil
    return {element = button, text = type(text) == "string" and text or nil, visible = hdxData(button, "visible"),
        x = x, y = y, w = w, h = h}
end

-- Find the "Работать" button of the open job window: by its hdx text when province_dx answers, by
-- creation order otherwise (the pilot creates "Закрыть" first, "Работать" second). The alternate
-- flag flips that guess after a press that produced nothing. Never guesses when texts are readable.
-- The queue window belongs to the same pilot resource, so its button lives under the same root.
-- Text decides the state: "Записаться в очередь" = outside, "Выйти из очереди" = already queued.
local function locateQueueButton()
    local pilotRoot = state.pilotRoot
    local buttons = valid(pilotRoot) and read("getElementsByType", "hdxButton", pilotRoot) or {}
    if #buttons == 0 then buttons = read("getElementsByType", "hdxButton", root) or {} end
    for _, button in ipairs(buttons) do
        local info = hdxButtonInfo(button)
        if info.text and info.visible ~= false then
            if info.text:find("Записаться в очередь", 1, true) then
                info.method, info.inQueue = "queue_join", false
                return info
            elseif info.text:find("Выйти из очереди", 1, true) then
                info.method, info.inQueue = "queue_leave", true
                return info
            end
        end
    end
    return nil
end

local function locateJobButton(alternate)
    local pilotRoot = state.pilotRoot
    local buttons = valid(pilotRoot) and read("getElementsByType", "hdxButton", pilotRoot) or {}
    local scope = "pilot_root"
    if #buttons == 0 then buttons, scope = read("getElementsByType", "hdxButton", root) or {}, "root" end
    local infos, textual = {}, false
    for index, button in ipairs(buttons) do
        infos[index] = hdxButtonInfo(button)
        textual = textual or infos[index].text ~= nil
    end
    for _, info in ipairs(infos) do
        if info.text and info.text:find("Работать", 1, true) and info.visible ~= false then
            info.method = "hdx_text"; return info, infos, scope
        end
    end
    if not textual and scope == "pilot_root" and #infos >= 2 then
        local info = infos[alternate and 1 or 2]
        info.method = alternate and "creation_order_alt" or "creation_order"
        return info, infos, scope
    end
    return nil, infos, scope
end

-- Press the button the way a hand does: cursor onto it, mouse down, mouse up. province_dx turns that
-- into onHdxElementPressed(button, "left", false) for the pilot's own handler, which then talks to
-- the server itself. Returns "done" after the release, nil while busy, false when a click is
-- impossible (no rectangle, or the DarkFlame menu would swallow it).
local function clickJobButton(press, now)
    local info = press.button
    if not valid(info.element) then return false end
    if press.step == "move" then
        if not (info.x and info.y and info.w and info.h) or read("dfMenuOpen") == true then return false end
        local cx, cy = math.floor(info.x + info.w / 2), math.floor(info.y + info.h / 2)
        -- Two moves: the second guarantees a mouse-move message even if the cursor already sat there.
        read("setCursorPosition", cx + 1, cy)
        press.moved = read("setCursorPosition", cx, cy) == true
        press.cx, press.cy, press.step, press.at = cx, cy, "down", now
    elseif press.step == "down" then
        if elapsed(now, press.at) < 150 then return nil end
        press.downOk = read("dfEmulateKey", "lmb", true) == true
        press.step, press.at = "up", now
    elseif press.step == "up" then
        if elapsed(now, press.at) < 120 then return nil end
        press.upOk = read("dfEmulateKey", "lmb", false) == true
        press.step, press.at = "done", now
        return "done"
    end
    return nil
end

-- Same arguments province_dx passes on mouse release: (button, "left", false). Used only while the
-- window is open, when the real click did not get through.
local function pressJobButtonEvent(info)
    if not info or not valid(info.element) then return false end
    return read("triggerEvent", "onHdxElementPressed", info.element, "left", false) ~= false
end

stopAutopilot = function(reason)
    stopWalking()
    local wasEnabled = state.apSessionActive or autopilot.enabled or state.controlsOwned
    state.completionGrace = wasEnabled and state.autonomy and reason == "Потеря самолёта / места пилота"
        and getTickCount() or nil
    -- No notification arrives on a silent firing; this is the only witness left.
    state.silentEndTick = state.completionGrace
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
    state.lastHold, state.gearGaveUp, state.gearUnknownLogged = nil, nil, nil
    if state.engineKeyDown then read("dfEmulateKey", ENGINE_KEY, false); state.engineKeyDown = nil end
    state.engineKeyTick = nil
    if #failed > 0 then autopilot.status = reason .. "; не удалось отпустить: " .. table.concat(failed, ", ") end
    if wasEnabled then
        local stats = state.flightStats
        if stats then
            local nowTick = getTickCount()
            local key = tostring(autopilot.phase or "?")
            stats.phases[key] = (stats.phases[key] or 0) + elapsed(nowTick, stats.phaseSince)
            local seconds = {}
            for phase, ms in pairs(stats.phases) do seconds[phase] = math.floor(ms / 100 + 0.5) / 10 end
            stats.total_s = elapsed(nowTick, stats.started) / 1000
            stats.reason = reason
            emit("flight_stats", {total_s = stats.total_s, phases_s = seconds, markers = stats.markers, turnbacks = stats.turnbacks,
                holds = stats.holds, aerobrakes = stats.aerobrakes, engine_cuts = stats.engineCuts,
                slowest_marker_gap_s = stats.slowestMarkerGap / 1000, reason = reason, completed_routes = state.completedRoutes or 0}, true)
            state.lastFlightStats, state.flightStats = stats, nil
        end
        emit("autopilot_stop", {reason = reason, previous = previous, release_failures = failed}, true)
        playAutopilotSound(false)
    end
    if wasEnabled and state.recording then
        -- A takeover keeps the recording running to the end of the job: the flight is the unit of
        -- analysis, not the autopilot session. Only a finished job, damage or death close it.
        local terminal = false
        for _, word in ipairs({"Рейс выполнен", "Работа завершена", "Повреждение", "погиб", "Выгрузка", "Ресурс пилота"}) do
            if tostring(reason):find(word, 1, true) then terminal = true; break end
        end
        if state.recordingOwner == "autopilot" and terminal and not state.autonomy then stop("autopilot_stopped")
        elseif state.recordingOwner == "autopilot" then
            state.recordingOwner = "manual"
            state.status = "Перехват: запись продолжается до конца рейса. Управление у тебя."
            emit("recording_owner", {owner = "manual", reason = reason}, true)
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
    if item.on_ground ~= true then
        if not autopilot.airborne and elapsed(now, state.lastProbe) < 600 then return state.pathClear end
        return nil
    end
    local intent = autopilot:groundIntent(item, now)
    local forward = item.velocity_body_rfu_mps and number(item.velocity_body_rfu_mps[2]) or 0
    local motion = forward < -0.25 and -1 or forward > 0.25 and 1 or intent.direction
    local nav = item.navigation or {}
    local key = table.concat({intent.direction, motion, tostring(intent.runway), tostring(intent.runway_align),
        tostring(nav.id), tostring(nav.waypoint_generation), tostring(item.dimension), tostring(item.interior)}, ":")

    local began = getTickCount()
    local basis, position = item.basis_world_rfu, item.position_m
    if not basis or not position or position == NULL then state.pathClear = nil; return nil end
    local speed = item.speed_kmh / 3.6
    local function compatible(work, continuing)
        local life = continuing and 900 or 250
        if not work or work.vehicle ~= state.vehicle or work.key ~= key or elapsed(now, work.started) > life then return false end
        local room = continuing and math.max(4, speed * 0.8) or math.max(1.5, speed * 0.3)
        local dx, dy = position[1] - work.position[1], position[2] - work.position[2]
        return dx*dx + dy*dy <= room^2
            and math.abs(intent.yaw - work.intentYaw) <= (continuing and 3 or 0.5)
            and math.abs(position[3] - work.position[3]) < 0.6
            and math.abs(angle(item.heading_deg - work.heading)) < (continuing and 15 or 8) and math.abs(speed - work.speed) < 2
            and math.abs(angle(item.pitch_deg - work.pitch)) < 3 and math.abs(angle(item.roll_deg - work.roll)) < 3
    end
    local work = state.probeWork
    if compatible(work, work ~= nil and work.done ~= true) then
        if work.done and elapsed(now, work.started) < 150 then return state.pathClear end
        if work.done then work = nil end
    else
        if work and work.done ~= true then state.probeStarved = (state.probeStarved or 0) + 1 end
        work = nil
    end
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
        work = autopilot:probeGeometry(item, intent, box, motion, (state.probeStarved or 0) >= 3)
        work.started, work.vehicle, work.key = now, state.vehicle, key
        work.position, work.heading, work.speed = position, item.heading_deg, speed
        work.pitch, work.roll, work.intentYaw = item.pitch_deg, item.roll_deg, intent.yaw
        work.next, work.checked, work.clear, work.cost, work.passes = 1, {}, true, 0, 0
        state.probeWork = work
    end
    work.passes = work.passes + 1
    while work.next <= #work.rays and elapsed(getTickCount(), began) < 6 do
        local ray = work.rays[work.next]
        local a, b = ray.from, ray.to
        local result
        if ray.part == "left_wing" or ray.part == "right_wing" or ray.part == "left_engine" or ray.part == "right_engine" then
            -- Only terrain at the measured ground height may be ignored under a wing.
            -- An upward normal alone also describes a static ramp; engines must treat it as blocked.
            local hit, hx, hy, hz, element, nx, ny, nz = read("processLineOfSight", a[1], a[2], a[3], b[1], b[2], b[3],
                true, true, true, true, true, false, false, false, state.vehicle)
            if hit == nil then result = nil
            elseif hit == false then result = true
            else
                local wing = ray.part == "left_wing" or ray.part == "right_wing"
                local terrain = item.surface and item.surface.ground_z_m
                local ground = wing and not valid(element) and finite(nz) and nz > 0.6
                    and finite(hz) and finite(terrain) and hz <= terrain + 0.4
                result = ground
                ray.hit = {number(hx) or NULL, number(hy) or NULL, number(hz) or NULL}
                ray.hit_normal_z, ray.hit_ground = number(nz) or NULL, ground
                ray.hit_element = valid(element) and read("getElementType", element) or NULL
            end
        else
            result = read("isLineOfSightClear", a[1], a[2], a[3], b[1], b[2], b[3], true, true, true, true, true, false, false, state.vehicle)
        end
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
    if work.done then state.probeStarved = 0 end
    local cost = elapsed(getTickCount(), began)
    work.cost = work.cost + cost
    local status = work.clear == false and "blocked" or work.done and (work.clear == true and "clear" or "unavailable") or "pending"
    if status == "blocked" then state.lastClearProbe = nil end
    if status == "clear" then state.lastClearProbe = work end
    local clear = work.done and work.clear or nil
    if work.clear == false then clear = false end
    local function clearStillValid(last)
        if not last or last.vehicle ~= state.vehicle or last.key ~= key then return false end
        if elapsed(now, last.started) > 1500 then return false end
        local dx, dy = position[1] - last.position[1], position[2] - last.position[2]
        return dx * dx + dy * dy <= 36 and math.abs(angle(item.heading_deg - last.heading)) < 20
    end
    local cached = status == "pending" and clearStillValid(state.lastClearProbe)
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
    if out.gear_down ~= nil and type(gearDown) ~= "boolean" then
        -- Unknown gear state: fatal on the ground, logged once in the air.
        if not autopilot.airborne then stopAutopilot("Неизвестно положение шасси"); return end
        if not state.gearUnknownLogged then
            state.gearUnknownLogged = true
            emit("autopilot_gear_unknown", {requested_down = out.gear_down, autopilot_continues = true}, true)
        end
    elseif out.gear_down ~= nil then
        if state.gearDesired ~= out.gear_down then
            state.gearDesired, state.gearAttempts, state.gearAttemptTick, state.gearGaveUp = out.gear_down, 0, nil, nil
        end
        if gearDown == out.gear_down then state.gearAttempts = 0
        elseif elapsed(now, state.gearAttemptTick) >= 2000 and not state.gearGaveUp then
            if (state.gearAttempts or 0) >= 3 then
                if not autopilot.airborne then stopAutopilot("Шасси не отвечают на управление"); return end
                -- Airborne: stop pulsing, keep flying, leave the gear to the pilot.
                state.gearGaveUp = true
                emit("autopilot_gear_unresponsive", {down = out.gear_down, observed_down = gearDown,
                    attempts = state.gearAttempts, autopilot_continues = true}, true)
            else
                state.gearAttempts = (state.gearAttempts or 0) + 1
                state.gearAttemptTick, state.gearPulse = now, now
                emit("autopilot_gear_request", {down = out.gear_down, observed_down = gearDown, attempt = state.gearAttempts,
                    reason = autopilot.detail and autopilot.detail.gear_command_reason or NULL}, true)
            end
        end
    end
    -- Engine follows the controller through the server's own X keybind: one pulse per change, checked
    -- against the observed engine state, because a blind pulse would simply toggle it back.
    local engine = read("getVehicleEngineState", state.vehicle)
    local wantEngine = not out.engine_off
    -- 300 ms between presses keeps up with the half-second pulses of the aerobrake.
    if type(engine) == "boolean" and engine ~= wantEngine and elapsed(now, state.engineKeyTick) >= 300 then
        state.engineKeyTick = now
        local pressed = read("dfEmulateKey", ENGINE_KEY, true) == true
        if pressed then state.engineKeyDown = now end
        emit("autopilot_engine_request", {engine_on = wantEngine, observed = engine, pressed = pressed, phase = autopilot.phase}, true)
        if state.flightStats and not wantEngine then state.flightStats.engineCuts = state.flightStats.engineCuts + 1 end
    end
    if state.engineKeyDown and elapsed(now, state.engineKeyDown) >= 60 then
        if read("dfEmulateKey", ENGINE_KEY, false) == true then state.engineKeyDown = nil end
    end
    local holding = (autopilot.detail and autopilot.detail.rudder_control == "hold") or math.abs(out.rudder or 0) >= 0.22
    local pulse = holding or elapsed(now, autopilot.started) % 180 / 180 < math.abs(out.rudder)
    local side = pulse and (out.rudder < 0 and -1 or out.rudder > 0 and 1) or 0
    if side ~= 0 and state.rudderSideApplied == -side then side = 0 end
    state.rudderSideApplied = side
    local digital = {side < 0, side > 0, out.handbrake,
        state.gearPulse ~= nil and elapsed(now, state.gearPulse) < 100}
    for pass = 1, 2 do
        for i, name in ipairs(ownedDigital) do
            if (pass == 1) == (digital[i] ~= true) then
                if read("setPedControlState", localPlayer, name, digital[i] == true) ~= true then
                    stopAutopilot("Ошибка команды управления: " .. name)
                    return
                end
            end
        end
    end
    state.applied = {tick_ms = now, analog = values, analog_names = ownedAnalog, analog_write_order = order,
        analog_readback = readback, readback_stage = "after_write_before_game_frame",
        digital = digital, digital_names = ownedDigital, rudder_control = holding and "hold" or "pwm",
        rudder_pwm_period_ms = holding and 0 or 180}
end

-- BEGIN JOB TIMER
local function trackJobTimer(channel, element, seconds, header, text)
    if not finite(seconds) then return end
    local now = getTickCount()
    local current = state.jobTimer
    if channel == "plrTimer:init" then
        local kind = "other"
        if type(text) == "string" then
            if text:find("следующей точки", 1, true) then kind = "marker"
            elseif text:find("пассажиров", 1, true) then kind = "boarding" end
        end
        state.jobTimer = {element = element, kind = kind, text = text, total_s = seconds, seconds = seconds, tick = now, since = now}
    elseif current and (current.element == element or not valid(current.element) or elapsed(now, current.tick) > 2500) then
        if current.element ~= element then current.element, current.kind, current.total_s, current.since = element, "unknown", nil, now end
        current.seconds, current.tick = seconds, now
    elseif not current then
        -- Ticks without the start (the script was loaded mid-leg): a timer of unknown purpose.
        state.jobTimer = {element = element, kind = "unknown", seconds = seconds, tick = now, since = now}
    end
end

-- Remaining seconds now (the last tick minus its age), the age of that tick, the kind and the total.
local function jobTimerSnapshot(now)
    local t = state.jobTimer
    if not t or not finite(t.seconds) then return nil end
    local age = elapsed(now or getTickCount(), t.tick)
    if age > 3000 then return nil end
    return math.max(0, t.seconds - age / 1000), age, t.kind, t.total_s, elapsed(now or getTickCount(), t.since) / 1000
end

local function jobTimerText()
    local remaining, _, kind = jobTimerSnapshot()
    if not remaining then return "--" end
    return string.format("%s %.0fs", kind == "marker" and "MARKER" or kind == "boarding" and "BOARD" or "TIMER", remaining)
end
-- END JOB TIMER

updateAutopilot = function(item, now)
    if not autopilot.enabled then return end
    local oldPhase, oldWaypoint = autopilot.phase, autopilot.waypoint
    local clear
    if item.vehicle ~= NULL then clear = groundProbe(item, now) end
    if state.recording and state.probe and state.loggedProbeTick ~= state.probe.tick_ms then
        emit("autopilot_obstacle_probe", state.probe)
        state.loggedProbeTick = state.probe.tick_ms
    end
    state.lastOnGround, state.lastSpeed, state.lastHeading = item.on_ground, item.speed_kmh, item.heading_deg
    item.ground_bump_side = state.groundBumpSide
    local lesson = autopilot.detail and autopilot.detail.aim_learn
    if type(lesson) == "table" and state.lastAimLesson ~= lesson then
        state.lastAimLesson = lesson
        learnAim(lesson)
    end
    item.ground_bump_ms = state.groundBumpTick and elapsed(now, state.groundBumpTick) or nil
    local remaining, age, kind, total, running = jobTimerSnapshot(now)
    if remaining then
        item.job_timer = {kind = kind, remaining_s = remaining, age_ms = age, total_s = total or NULL, elapsed_s = running}
        if kind == "marker" or kind == "unknown" then
            item.waypoint_timer = {remaining_s = remaining, age_ms = age, total_s = total or NULL}
            if not state.legTimerMin or remaining < state.legTimerMin then state.legTimerMin = remaining end
        end
    end
    if state.probe and state.probe.status == "blocked" and state.probe.blocked_ray then
        local blocked = state.probe.rays and state.probe.rays[state.probe.blocked_ray]
        item.obstacle_part = blocked and blocked.part or NULL
    end
    autopilot:update(item, now, clear, state.probe and state.probe.status)
    state.windowRestored = false
    if autopilot.detail and autopilot.detail.timing_resynced then
        emit("autopilot_timing_resync", {frame_gap_ms = autopilot.detail.frame_gap_ms,
            reason = autopilot.detail.timing_resync_reason or NULL,
            minimized = item.window_minimized, restored = item.window_restored}, true)
    end
    -- Airborne hold: the controller kept the aircraft instead of quitting. Sound the alarm once per
    -- episode so the pilot can decide; any flight key still takes over.
    local hold = autopilot.enabled and autopilot.detail and autopilot.detail.airborne_hold or nil
    if hold ~= state.lastHold then
        if hold then
            safetyAlert("autopilot_hold", 10000, "Автопилот удержал самолёт (" .. tostring(hold) .. "): "
                .. tostring(autopilot.status) .. ". Перехват — любая клавиша полёта.", {reason = hold, phase = autopilot.phase})
        elseif state.lastHold then
            outputChatBox("#55FF55[Pilot] #FFFFFFОриентир восстановлен, автопилот продолжает.", 255, 255, 255, true)
        end
        emit("autopilot_hold", {reason = hold or NULL, previous = state.lastHold or NULL, phase = autopilot.phase}, true)
        state.lastHold = hold
    end
    if not autopilot.enabled then stopAutopilot(autopilot.status)
    elseif oldPhase ~= autopilot.phase or oldWaypoint ~= autopilot.waypoint then
        local stats = state.flightStats
        if stats then
            if oldPhase ~= autopilot.phase then
                local key = tostring(oldPhase or "?")
                stats.phases[key] = (stats.phases[key] or 0) + elapsed(now, stats.phaseSince)
                stats.phaseSince = now
                if autopilot.phase == "turnback" then stats.turnbacks = stats.turnbacks + 1 end
                if autopilot.phase == "obstacle_hold" or autopilot.phase == "obstacle_escape" then stats.holds = stats.holds + 1 end
                if autopilot.phase == "aerobrake" then stats.aerobrakes = stats.aerobrakes + 1 end
            end
            if oldWaypoint ~= autopilot.waypoint then
                if stats.lastMarkerTick then stats.slowestMarkerGap = math.max(stats.slowestMarkerGap, elapsed(now, stats.lastMarkerTick)) end
                stats.markers, stats.lastMarkerTick = stats.markers + 1, now
            end
        end
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
    state.completionGrace, state.silentEndTick = nil, nil
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
    state.flightStats = {started = getTickCount(), phaseSince = getTickCount(), phases = {}, markers = 0,
        turnbacks = 0, holds = 0, aerobrakes = 0, engineCuts = 0, lastMarkerTick = nil, slowestMarkerGap = 0}
    playAutopilotSound(true)
    bridge.update("autopilot", "1")
    bridge.update("autopilot_status", autopilot.status)
    return true
end

-- BEGIN TERRAIN MAP
local TERRAIN = {step = 2, radius = 90, budgetFrozen = 4, budgetMoving = 0.4,
    flat = 0.35, engineTop = 3.0, wingTop = 5.5, canopy = 5.5}
-- Cell codes: 1 unknown, 2 clear, 3 blocks the engines/body, 4 blocks the wings only.

local TERRAIN_BIAS = 32768
local function terrainKey(cx, cy)
    return (cx + TERRAIN_BIAS) * 65536 + (cy + TERRAIN_BIAS)
end
local function terrainUnkey(key)
    local cy = key % 65536 - TERRAIN_BIAS
    return math.floor(key / 65536) - TERRAIN_BIAS, cy
end

local function terrainCell(x, y)
    if not state.terrain then return nil end
    return state.terrain[terrainKey(math.floor(x / TERRAIN.step), math.floor(y / TERRAIN.step))]
end

-- Queue cells outward from the aircraft, nearest first.
local function terrainEnqueue(px, py)
    local step, radius = TERRAIN.step, TERRAIN.radius
    local ox, oy = math.floor(px / step), math.floor(py / step)
    local cells = math.floor(radius / step)
    local queue = {}
    for ring = 0, cells do
        for dx = -ring, ring do
            for dy = -ring, ring do
                if math.abs(dx) == ring or math.abs(dy) == ring then
                    local cx, cy = ox + dx, oy + dy
                    if not state.terrain or state.terrain[terrainKey(cx, cy)] == nil then
                        queue[#queue + 1] = {cx, cy}
                    end
                end
            end
        end
    end
    state.terrainQueue, state.terrainQueueAt = queue, 1
end

-- One cell: a ray down to find the top surface, and when that top is a roof, a second ray up
-- through the band the aircraft occupies to learn what is under it.
local function terrainScanCell(cx, cy)
    local base = state.terrainBase
    if not base then return end
    local x = cx * TERRAIN.step + TERRAIN.step * 0.5
    local y = cy * TERRAIN.step + TERRAIN.step * 0.5
    local hit, _, _, hz = read("processLineOfSight", x, y, base + 60, x, y, base - 8,
        true, true, false, true, false, false, false, false, state.vehicle)
    local code
    if hit ~= true or not finite(hz) then
        -- Nothing answered: either empty air or, just as likely, unstreamed collision.
        code = 1
    else
        local above = hz - base
        if above > TERRAIN.canopy then
            -- A roof. Ask what is underneath, inside the band the aircraft really occupies.
            local under, _, _, uz = read("processLineOfSight", x, y, base + 0.3, x, y, base + 5.2,
                true, true, false, true, false, false, false, false, state.vehicle)
            if under ~= true then code = 2
            elseif finite(uz) and uz - base > TERRAIN.engineTop then code = 4
            else code = 3 end
        elseif above > TERRAIN.wingTop then code = 2
        elseif above > TERRAIN.engineTop then code = 4
        elseif above > TERRAIN.flat then code = 3
        else code = 2 end
    end
    state.terrain[terrainKey(cx, cy)] = code
    state.terrainScanned = (state.terrainScanned or 0) + 1
    if state.terrainZone then state.terrainZone.scanned = state.terrainScanned end
end

local function terrainTick(now, item)
    if not state.recording and not autopilot.enabled then return end
    local position = item and item.position_m
    if type(position) ~= "table" or not finite(position[1]) or not finite(position[2])
        or not finite(position[3]) or item.vehicle == NULL then return end
    -- Airborne there is nothing to map and every ray is wasted frame time.
    if item.on_ground ~= true then return end
    -- One map per 2 km zone: an airport keeps its map across flights instead of being rescanned.
    state.terrainZones = state.terrainZones or {}
    local zx, zy = math.floor(position[1] / 2000), math.floor(position[2] / 2000)
    local zoneKey = (zx + 1024) * 4096 + (zy + 1024)
    local zone = state.terrainZones[zoneKey]
    if not zone then
        zone = {base = position[3], cells = {}, scanned = 0, zx = zx, zy = zy}
        state.terrainZones[zoneKey] = zone
        emit("terrain_zone", {zone = {zx, zy}, base_z = zone.base, created = true}, true)
    end
    if state.terrainZoneKey ~= zoneKey then
        state.terrainZoneKey, state.terrainQueue = zoneKey, nil
        emit("terrain_zone", {zone = {zx, zy}, base_z = zone.base, cells = zone.scanned,
            created = false}, true)
    end
    state.terrain, state.terrainBase = zone.cells, zone.base
    state.terrainScanned = zone.scanned
    state.terrainZone = zone
    if not state.terrainQueue or state.terrainQueueAt > #state.terrainQueue
        or not state.terrainOrigin or (state.terrainOrigin[1] - position[1])^2
            + (state.terrainOrigin[2] - position[2])^2 > 40 * 40 then
        state.terrainOrigin = {position[1], position[2]}
        terrainEnqueue(position[1], position[2])
    end
    local frozen = item.frozen == true
    local budget = frozen and TERRAIN.budgetFrozen or TERRAIN.budgetMoving
    local began = getTickCount()
    local done = 0
    while state.terrainQueueAt <= #state.terrainQueue and elapsed(getTickCount(), began) < budget do
        local cell = state.terrainQueue[state.terrainQueueAt]
        state.terrainQueueAt = state.terrainQueueAt + 1
        if state.terrain[terrainKey(cell[1], cell[2])] == nil then
            terrainScanCell(cell[1], cell[2])
            done = done + 1
        end
    end
    if done > 0 and elapsed(now, state.terrainReport or 0) > 3000 then
        state.terrainReport = now
        local counts = {0, 0, 0, 0}
        for _, code in pairs(state.terrain) do counts[code] = (counts[code] or 0) + 1 end
        emit("terrain_progress", {scanned = state.terrainScanned, queued = #state.terrainQueue,
            at = state.terrainQueueAt, unknown = counts[1], clear = counts[2],
            low = counts[3], wing = counts[4], frozen = frozen}, true)
    end
end
-- END TERRAIN MAP

local function updateAutonomy(now)
    local pending = state.nextJob
    if not pending then
        -- The job ended without a word from the server: re-employ anyway once the aircraft is gone.
        if state.silentEndTick and state.autonomy and not autopilot.enabled
            and elapsed(now, state.silentEndTick) > 3000 then
            state.silentEndTick = nil
            if not read("getPedOccupiedVehicle", localPlayer) then
                state.nextJob = {stage = "wait_exit", began = now}
                autopilot.status = "Рейс оборван без уведомления: беру новую работу"
                emit("autonomy_queued", {reason = "silent_job_end", wait_for_exit = true}, true)
                bridge.update("autopilot_waiting", "1")
                bridge.update("autopilot_status", autopilot.status)
            end
        end
        return
    end
    state.silentEndTick = nil
    if pending.stage ~= pending.loggedStage then
        pending.loggedStage = pending.stage
        emit("autonomy_stage", {stage = pending.stage, attempts = pending.attempts or 0, status = autopilot.status,
            position = vector("getElementPosition", localPlayer) or NULL}, true)
    end
    -- A cycle started by hand from the panel runs without the autonomy switch.
    if (not state.autonomy and not pending.manual) or not state.pilotReady then stopAutopilot("Автономный цикл отменён"); return end
    local queueStage = pending.stage == "queue_join" or pending.stage == "queue_wait"
    if not queueStage and elapsed(now, pending.began) > 60000 then stopAutopilot("Новый рейс не начался: ожидание истекло"); return end
    local vehicle = read("getPedOccupiedVehicle", localPlayer)
    if pending.stage == "wait_exit" then
        if vehicle or elapsed(now, pending.began) < 750 then return end
        state.destination = nil
        autopilot.terminal, autopilot.instruction = false, "unknown"
        emit("autonomy_job_request", {completed_routes = state.completedRoutes}, true)
        -- The server spawns the player on the job pickup: that spot is the marker. Step off, step back
        -- on; the job window opening is the only proof of standing on it. No shortcuts to the server.
        local px, py, pz = read("getElementPosition", localPlayer)
        if not finite(px) or not finite(py) or not finite(pz) then return end
        -- The spawn is next to the pickup, not on it (recorded): aim at the pickup element itself.
        local pickup, pickupDistance = nearestJobPickup(px, py)
        pending.origin = pickup or {px, py, pz}
        -- The spawn point is the only spot known to be walkable: retries step back onto it, never
        -- into a wall. When the spawn is already off the pickup, walk straight onto it.
        pending.spawn, pending.stepTarget = {px, py, pz}, {px, py, pz}
        local offPickup = pickup and (pickupDistance or 0) >= 1.2
        pending.stage, pending.since, pending.attempts = offPickup and "step_on" or "step_off", now, 0
        state.pickupHitTick, state.jobWindowTick = nil, nil
        emit("autonomy_pickup_origin", {position = pending.origin, from_pickup_element = pickup ~= nil,
            pickup_distance_m = pickupDistance or NULL, spawn = {px, py, pz}, interior = read("getElementInterior", localPlayer) or NULL}, true)
        autopilot.status = offPickup and "Автономно: захожу на метку работы" or "Автономно: отхожу от метки работы"
    elseif pending.stage == "step_off" then
        local done, distance = walkToward(pending.stepTarget)
        local ox, oy = pending.origin[1], pending.origin[2]
        local px, py = read("getElementPosition", localPlayer)
        local clearOfPickup = finite(px) and finite(py) and math.sqrt((px - ox)^2 + (py - oy)^2) >= 1.5
        if done or done == nil or clearOfPickup or elapsed(now, pending.since) > 3000 then
            stopWalking()
            pending.stage, pending.since = "step_on", now
            state.pickupHitTick, state.jobWindowTick = nil, nil
            autopilot.status = "Автономно: захожу на метку работы"
        end
    elseif pending.stage == "step_on" then
        local done = walkToward(pending.origin)
        if state.jobWindowTick or state.pickupHitTick or done then
            stopWalking()
            pending.stage, pending.since, pending.press = "wait_window", now, nil
            autopilot.status = "Автономно: жду окно трудоустройства"
        elseif done == nil or elapsed(now, pending.since) > 6000 then
            stopWalking()
            pending.stage, pending.since, pending.attempts = "step_off", now, pending.attempts + 1
            pending.stepTarget = pending.spawn or pending.origin
        end
    elseif pending.stage == "wait_window" then
        -- Every work place is taken: the server answered the pickup with the queue window.
        if state.queueWindowTick and not state.jobWindowTick then
            pending.stage, pending.since, pending.queuePressed = "queue_join", now, nil
            emit("autonomy_queue", {action = "window_opened", attempts = pending.attempts or 0}, true)
            autopilot.status = "Автономно: мест нет, открылась очередь"
            return
        end
        -- The pilot shows the cursor together with its window: a second witness of it opening.
        if not state.jobWindowTick and state.pickupHitTick and elapsed(now, state.pickupHitTick) >= 300
            and read("isCursorShowing") == true then
            state.jobWindowTick = now
            emit("autonomy_window_by_cursor", {attempts = pending.attempts}, true)
        end
        local function goAround(status)
            pending.stage, pending.since, pending.attempts, pending.press = "step_off", now, pending.attempts + 1, nil
            pending.stepTarget = pending.spawn or pending.origin
            state.pickupHitTick, state.jobWindowTick = nil, nil
            autopilot.status = status
        end
        if state.jobWindowTick then
            if not pending.press then
                if elapsed(now, state.jobWindowTick) < 600 then return end
                local info, infos, scope = locateJobButton(pending.alternate)
                if info then
                    pending.press = {button = info, step = "move", at = now}
                    emit("autonomy_job_button", {method = info.method, text = info.text or NULL, scope = scope,
                        rect = {info.x or NULL, info.y or NULL, info.w or NULL, info.h or NULL},
                        visible = info.visible == nil and NULL or info.visible, buttons = #infos,
                        hdx_access = state.hdxAccess or NULL, attempts = pending.attempts,
                        alternate = pending.alternate or false}, true)
                elseif elapsed(now, state.jobWindowTick) > 3000 then
                    emit("autonomy_job_press", {method = "no_button", buttons = #infos, scope = scope,
                        hdx_access = state.hdxAccess or NULL, attempts = pending.attempts}, true)
                    goAround("Автономно: кнопка «Работать» не найдена, захожу снова (" .. (pending.attempts + 1) .. ")")
                end
                return
            end
            local press = pending.press
            if press.step == "event" then
                local ok = pressJobButtonEvent(press.button)
                emit("autonomy_job_press", {method = "hdx_event", ok = ok, button = press.button.method,
                    attempts = pending.attempts}, true)
                pending.stage, pending.requested, pending.pressedAt, pending.fallbackAt = "wait_plane", now, now, nil
                autopilot.status = "Автономно: нажал «Работать», ожидание самолёта"
                return
            end
            local result = clickJobButton(press, now)
            if result == "done" then
                emit("autonomy_job_press", {method = "cursor_click", x = press.cx, y = press.cy, moved = press.moved,
                    down = press.downOk, up = press.upOk, button = press.button.method, attempts = pending.attempts}, true)
                pending.stage, pending.requested, pending.pressedAt, pending.fallbackAt = "wait_plane", now, now, now
                autopilot.status = "Автономно: нажал «Работать», ожидание самолёта"
            elseif result == false then
                press.step = "event"
            end
        elseif elapsed(now, pending.since) > 4000 then
            if pending.attempts + 1 >= 6 then stopAutopilot("Окно трудоустройства не открылось после 6 заходов на метку"); return end
            goAround("Автономно: окно не открылось, захожу снова (" .. (pending.attempts + 1) .. ")")
        end
    elseif pending.stage == "queue_join" then
        local info = locateQueueButton()
        if info then
            if info.inQueue then
                pending.stage, pending.since = "queue_wait", now
                state.queueJoined = true
                emit("autonomy_queue", {action = "already_in", text = info.text or NULL}, true)
                autopilot.status = "Автономно: стою в очереди, жду вызова"
            elseif not pending.queuePressed then
                local ok = pressJobButtonEvent(info)
                pending.queuePressed = now
                emit("autonomy_queue", {action = "join_pressed", ok = ok, text = info.text or NULL}, true)
                autopilot.status = "Автономно: записываюсь в очередь"
            elseif elapsed(now, pending.queuePressed) > 2500 then
                -- The resource swallows a second press for 1.5 s; by now the text has flipped.
                pending.stage, pending.since = "queue_wait", now
                state.queueJoined = true
                autopilot.status = "Автономно: стою в очереди, жду вызова"
            end
        elseif elapsed(now, pending.since) > 6000 then
            emit("autonomy_queue", {action = "no_button"}, true)
            pending.stage, pending.since = "step_off", now
            pending.attempts, pending.queuePressed = (pending.attempts or 0) + 1, nil
            pending.stepTarget = pending.spawn or pending.origin
            state.queueWindowTick = nil
            autopilot.status = "Автономно: кнопка очереди не найдена, захожу снова"
        end
    elseif pending.stage == "queue_wait" then
        if state.jobWindowTick then
            pending.stage, pending.since, pending.press = "wait_window", now, nil
            autopilot.status = "Автономно: окно трудоустройства открылось"
        elseif state.queueCalledTick then
            -- Our turn: the server gives 60 s to step on the job pickup again.
            state.queueCalledTick, state.queueJoined = nil, nil
            pending.began, pending.queuePressed = now, nil
            pending.stage, pending.since, pending.attempts = "step_off", now, 0
            pending.stepTarget = pending.spawn or pending.origin
            state.pickupHitTick, state.jobWindowTick, state.queueWindowTick = nil, nil, nil
            emit("autonomy_queue", {action = "called"}, true)
            autopilot.status = "Автономно: очередь подошла, иду на метку работы"
        elseif elapsed(now, pending.since) > 1200000 then
            stopAutopilot("Очередь не подошла за 20 минут")
        end
    elseif pending.stage == "wait_plane" then
        local ready = vehicle and read("getElementModel", vehicle) == 519
            and read("getPedOccupiedVehicleSeat", localPlayer) == 0
            and read("getVehicleController", vehicle) == localPlayer
        if not ready and pending.fallbackAt and elapsed(now, pending.fallbackAt) > 1500 then
            pending.fallbackAt = nil
            -- The click did not close the window: press the same button through its element event.
            local closedAfterPress = state.jobWindowClosedTick
                and elapsed(now, state.jobWindowClosedTick) < elapsed(now, pending.pressedAt)
            if read("isCursorShowing") == true and not closedAfterPress then
                local ok = pressJobButtonEvent(pending.press and pending.press.button)
                emit("autonomy_job_press", {method = "hdx_event_after_click", ok = ok, attempts = pending.attempts}, true)
            end
        end
        if not ready and pending.pressedAt and elapsed(now, pending.pressedAt) > 8000 then
            -- The press did not put us in an aircraft: go around the pickup again. A creation-order
            -- guess flips to the other button next time.
            local guessed = pending.press and pending.press.button.method or "?"
            emit("autonomy_press_unanswered", {attempts = pending.attempts, button = guessed,
                window_closed = state.jobWindowClosedTick ~= nil}, true)
            if guessed:find("creation_order", 1, true) then pending.alternate = not pending.alternate end
            pending.stage, pending.since, pending.attempts, pending.pressedAt = "step_off", now, pending.attempts + 1, nil
            pending.press, pending.fallbackAt = nil, nil
            pending.stepTarget = pending.spawn or pending.origin
            state.pickupHitTick, state.jobWindowTick = nil, nil
            autopilot.status = "Автономно: самолёт не появился, захожу снова (" .. pending.attempts .. ")"
            return
        end
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
        -- The first job is always the pilot's own: walk to the pickup and press "Работать" in the game.
        -- The autonomy takes over only after a job ends.
        state.status = "Первый рейс запускается вручную: подойди к метке и нажми «Работать»."
    elseif value == "start" then start()
    elseif value == "stop" then stop("user")
    elseif value == "autopilot_start" then startAutopilot()
    elseif value == "autopilot_stop" then stopAutopilot("Остановлен кнопкой")
    elseif value == "autopilot_autonomy:1" or value == "autopilot_autonomy:0" then
        state.autonomy = value == "autopilot_autonomy:1"
        if not state.autonomy then
            state.completionGrace = nil
            if state.nextJob and not state.nextJob.manual then stopAutopilot("Автономность выключена") end
        end
        emit("autonomy_setting", {enabled = state.autonomy})
    elseif value == "autopilot_hud:1" or value == "autopilot_hud:0" then
        state.hudEnabled = value == "autopilot_hud:1"
        state.hudSmooth, state.hudLastFrame, state.hudError = nil, nil, nil
        emit("autopilot_hud", {enabled = state.hudEnabled})
    elseif value:sub(1, 9) == "hud_mode:" then
        local mode = value:sub(10)
        if mode == "full" or mode == "left" or mode == "right" then
            state.hudMode, state.hudError = mode, nil
            emit("autopilot_hud", {enabled = state.hudEnabled, mode = mode})
        end
    elseif value == "autopilot_aggressive:1" or value == "autopilot_aggressive:0" then
        state.aggressive = value == "autopilot_aggressive:1"
        autopilot.aggressive = state.aggressive
        emit("autopilot_aggressive", {enabled = state.aggressive}, true)
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
    terrainTick(now, state.latest)
    now = getTickCount()
    syncSafetyMonitor()
    if safetyActive() and elapsed(now, state.lastSafetyScan) >= 2000 then scanSafetyPlayers(now) end
    -- The alert path (sound, chat, log) can cost tens of milliseconds; never bill them to the controller's dt.
    now = getTickCount()
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
    -- The HUD reads the latest sample; keep it fresh whenever it is on, not only under autopilot.
    if autopilot.enabled or state.hudEnabled then interval = math.min(50, interval) end
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

-- World-anchored diamond with a label; returns whether it landed on screen.
local function drawWorldDiamond(wx, wy, wz, size, color, label, scale, width, height, diamond)
    local sx, sy = getScreenFromWorldPosition(wx, wy, wz, 0, false)
    if not finite(sx) or not finite(sy) then return false end
    local pad = 20 * scale
    if sx < pad or sx > width - pad or sy < pad or sy > height - pad then return false end
    local r = size * scale
    if diamond then
        dxDrawLine(sx, sy - r, sx + r, sy, color, 2 * scale, false); dxDrawLine(sx + r, sy, sx, sy + r, color, 2 * scale, false)
        dxDrawLine(sx, sy + r, sx - r, sy, color, 2 * scale, false); dxDrawLine(sx - r, sy, sx, sy - r, color, 2 * scale, false)
    end
    if label then
        dxDrawText(label, sx - 90 * scale, sy + r + 2 * scale, sx + 90 * scale, sy + r + 22 * scale,
            color, 0.8 * scale, "default-bold", "center", "top", false, false, false)
    end
    return true
end

-- Markers through fog and draw distance: the current target with distance and height difference,
-- and the ring after it, so the pilot sees the route before the game renders it.
-- A circle in the world: flat on the ground for checkpoints, upright and facing its target for rings.
local function drawWorldCircle(x, y, z, radius, color, upright, dirX, dirY)
    if not (finite(x) and finite(y) and finite(z) and finite(radius) and radius > 0) then return end
    local segments, px, py, pz = 36, nil, nil, nil
    for i = 0, segments do
        local a = i / segments * 2 * math.pi
        local cx, cy, cz
        if upright then
            cx, cy, cz = x + dirY * math.cos(a) * radius, y - dirX * math.cos(a) * radius, z + math.sin(a) * radius
        else
            cx, cy, cz = x + math.cos(a) * radius, y + math.sin(a) * radius, z
        end
        if px then dxDrawLine3D(px, py, pz, cx, cy, cz, color, 2, false) end
        px, py, pz = cx, cy, cz
    end
end

local function drawMarkerOverlay(nav, scale, width, height, includeCurrent)
    if type(nav) ~= "table" or nav == NULL then return end
    local p = nav.position
    if type(p) == "table" and p ~= NULL and finite(p[1]) and finite(p[2]) and finite(p[3]) then
        -- Rings are spheres (precise); checkpoints are circles of unlimited height (any altitude).
        -- [MEM] marks a chain the bot knows from memory: the aggressive plan applies only then.
        local known = type(nav.chain_source) == "string" and nav.chain_source ~= "arrow"
        local label = string.format("%s%s  %s M  DZ %s", nav.marker_type == "ring" and "RING PRECISE"
                or nav.marker_type == "checkpoint" and "CHKPT ANY ALT" or "TARGET", known and " [MEM]" or "",
            finite(nav.distance_3d_m) and string.format("%.0f", nav.distance_3d_m) or "--",
            finite(nav.altitude_error_m) and string.format("%+.0f", nav.altitude_error_m) or "--")
        drawWorldDiamond(p[1], p[2], p[3], 12, nav.ambiguous and 0xFFFFC36A or 0xFF70FF92, label, scale, width, height, includeCurrent)
        -- The bot's own geometry: outer = the size it believes the marker has, inner = the corridor it
        -- aims inside (0.65 R on checkpoints, the 0.45 R capture on rings).
        local size = finite(nav.marker_size_m) and nav.marker_size_m or nil
        if size then
            local c = nav.color_rgba or {}
            local yellow = finite(c[1]) and finite(c[2]) and finite(c[3]) and c[1] > 200 and c[2] > 160 and c[3] < 80
            local outer = nav.marker_type == "ring" and 0xB070FF92 or (yellow and 0xB0FFD24A or 0xB0FF5A5A)
            if nav.marker_type == "ring" then
                local n, dx, dy = nav.next_position, 0, 1
                if type(n) == "table" and n ~= NULL and finite(n[1]) and finite(n[2]) then
                    dx, dy = n[1] - p[1], n[2] - p[2]
                    local l = math.sqrt(dx * dx + dy * dy)
                    if l > 1 then dx, dy = dx / l, dy / l else dx, dy = 0, 1 end
                end
                drawWorldCircle(p[1], p[2], p[3], size, outer, true, dx, dy)
                drawWorldCircle(p[1], p[2], p[3], size * 0.7, 0x90FFFFFF, true, dx, dy)
            else
                drawWorldCircle(p[1], p[2], p[3], size, outer, false)
            end
        end
        local pts = autopilot.detail and autopilot.detail.plan_points
        if type(pts) == "table" then
            for i = 1, #pts - 1 do
                local a, b = pts[i], pts[i + 1]
                if type(a) == "table" and type(b) == "table" and finite(a[1]) and finite(b[1]) then
                    dxDrawLine3D(a[1], a[2], a[3] + 2, b[1], b[2], b[3] + 2, 0xD0FFFFFF, 2, false)
                    drawWorldCircle(b[1], b[2], b[3] + 2, 2.5, 0xD0FFFFFF, false)
                end
            end
        end
    end
    local chain = nav.chain_ahead
    if type(chain) == "table" then
        local prev = nav.position
        for i = 2, math.min(#chain, 10) do
            local m = chain[i]
            local q = type(m) == "table" and m.position or nil
            if type(q) == "table" and finite(q[1]) and finite(q[2]) and finite(q[3]) and type(prev) == "table" and finite(prev[1]) then
                local col = m.yellow and 0xA0FFD24A or (m.marker_type == "ring" and 0xA070FF92 or 0xA0FF8A8A)
                drawWorldDiamond(q[1], q[2], q[3], 6, col, tostring(i - 1), scale, width, height, true)
                local ax, ay = getScreenFromWorldPosition(prev[1], prev[2], prev[3], 0, false)
                local bx, by = getScreenFromWorldPosition(q[1], q[2], q[3], 0, false)
                if finite(ax) and finite(ay) and finite(bx) and finite(by) then dxDrawLine(ax, ay, bx, by, 0x60AFD8B5, 1.5 * scale, false) end
            end
            prev = q or prev
        end
    end
    for _, a in ipairs(state.aimMemory) do
        drawWorldDiamond(a.x + a.ox, a.y + a.oy, a.z, 5, 0xFFFF70E0, "LEARN " .. a.hits, scale, width, height, true)
        if a.via then drawWorldDiamond(a.via[1], a.via[2], a.z, 5, 0xFFFF70E0, "VIA", scale, width, height, true) end
    end
    local aim = autopilot.detail and autopilot.detail.aim_point
    if type(aim) == "table" and finite(aim[1]) and finite(aim[2]) and type(nav.position) == "table" and finite(nav.position[3]) then
        drawWorldDiamond(aim[1], aim[2], nav.position[3], 6, 0xFFFFFFFF, "AIM", scale, width, height, true)
    end
    local n = nav.next_position
    if type(n) == "table" and n ~= NULL and finite(n[1]) and finite(n[2]) and finite(n[3]) then
        drawWorldDiamond(n[1], n[2], n[3], 8, 0xC0AFD8B5, "NEXT", scale, width, height, true)
        -- The leg between the two markers: the route as the bot plans it, through fog or darkness.
        if type(p) == "table" and p ~= NULL and finite(p[1]) and finite(p[2]) and finite(p[3]) then
            local ax, ay = getScreenFromWorldPosition(p[1], p[2], p[3], 0, false)
            local bx, by = getScreenFromWorldPosition(n[1], n[2], n[3], 0, false)
            if finite(ax) and finite(ay) and finite(bx) and finite(by) then dxDrawLine(ax, ay, bx, by, 0x80AFD8B5, 2 * scale, false) end
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
    text(autopilot.enabled and ("AP  /  " .. string.upper(autopilot.phase)) or "MANUAL  /  HUD", -322, -211, "left", warning and amber or green, 0.94, 370)
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
        -- Marker bearing bug on the tape; off the tape it becomes an arrow pointing the short way round.
        if finite(nav.bearing_deg) then
            local rel = angle(nav.bearing_deg - course)
            local x = rel * 5
            if math.abs(x) <= 170 then
                line(x - 6, -166, x + 6, -166, amber, 2); line(x - 6, -166, x, -158, amber, 2); line(x + 6, -166, x, -158, amber, 2)
            else
                local edge = rel > 0 and 170 or -170
                local back = rel > 0 and -12 or 12
                line(edge, -162, edge + back, -169, amber, 2); line(edge, -162, edge + back, -155, amber, 2)
                text(string.format("%.0f", math.abs(rel)), edge + back * 2.2, -170, "center", amber, 0.7, 40)
            end
        end
    else text("HDG --", 0, -132, "center", amber) end

    text("SPEED  KM/H x127", -216, -70, "right", dim)
    box(-314, -45, 98, 35); text(numberText(finite(speed) and speed * SPEED_DISPLAY_MUL), -227, -42, "right", green, 1.4)
    text("ALT  M", 216, -70, "left", dim)
    box(216, -45, 98, 35); text(numberText(altitude), 303, -42, "right", green, 1.4)
    text("AGL  " .. numberText(agl) .. " M", 216, 5)
    text("V/S  " .. numberText(climb, "%+.1f") .. " M/S", 216, 29)
    text("PITCH  " .. numberText(pitch, "%+.1f"), -216, 5, "right")
    text("BANK  " .. numberText(roll, "%+.1f"), -216, 29, "right")
    -- Marker height next to own height: the one number a pilot has to remember for a second pass.
    local markerAlt = type(nav.position) == "table" and nav.position[3] or nil
    text("MRK ALT  " .. numberText(markerAlt) .. " M", 216, 53, "left", amber, 1.0)
    text("DZ  " .. numberText(nav.altitude_error_m, "%+.0f") .. " M", -216, 53, "right", amber, 1.0)
    text(item.on_ground == true and "GROUND" or item.on_ground == false and "AIR" or "GROUND --", -216, 79, "right")
    local gear = item.landing_gear_state == "down" and "DOWN" or item.landing_gear_state == "up" and "UP" or "--"
    text("GEAR  " .. gear, 216, 79)

    local turn = nav.heading_error_deg
    local turnText = finite(turn) and (numberText(math.abs(turn), "%.0f") .. " DEG "
        .. (math.abs(turn) < 1 and "AHEAD" or turn < 0 and "LEFT" or "RIGHT")) or "--"
    text("TURN  " .. turnText, 0, 122, "center", nav.ambiguous and amber or green)
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
    drawMarkerOverlay(nav, scale, width, height, false)
    if valid(state.vehicle) then
        local x1, y1, z1, x2, y2, z2 = read("getElementBoundingBox", state.vehicle)
        local m = read("getElementMatrix", state.vehicle, false)
        if finite(x1) and finite(y2) and type(m) == "table" and type(m[1]) == "table" and type(m[4]) == "table" then
            local function world(x, y, z)
                return m[4][1] + m[1][1] * x + m[2][1] * y + m[3][1] * z, m[4][2] + m[1][2] * x + m[2][2] * y + m[3][2] * z,
                    m[4][3] + m[1][3] * x + m[2][3] * y + m[3][3] * z
            end
            local function seg(ax, ay, az, bx, by, bz, color)
                local wx1, wy1, wz1 = world(ax, ay, az); local wx2, wy2, wz2 = world(bx, by, bz)
                dxDrawLine3D(wx1, wy1, wz1, wx2, wy2, wz2, color, 2, false)
            end
            -- floor and roof rectangles of the box, the wing line at contact height, the nose marked
            local zc = math.max(1.2, z1 * 0.65)
            for _, z in ipairs({z1}) do
                seg(x1, y1 + 2.0, z, x2, y1 + 2.0, z, 0x90FFFFFF); seg(x2, y1 + 2.0, z, x2, y2 + 2.0, z, 0x90FFFFFF)
                seg(x2, y2 + 2.0, z, x1, y2 + 2.0, z, 0x90FFFFFF); seg(x1, y2 + 2.0, z, x1, y1 + 2.0, z, 0x90FFFFFF)
            end
            seg(x1, 0, zc, x2, 0, zc, 0xFF70FF92)
            seg(0, y2 - 2 + 2.0, zc, 0, y2 + 2.0, zc, 0xFFFF5A5A)
        end
    end
    if state.terrain and state.terrainBase and type(item.position_m) == "table" and finite(item.position_m[1]) then
        local px, py = item.position_m[1], item.position_m[2]
        local base, half, drawn = state.terrainBase, 0.9, 0
        if type(state.terrainOrigin) == "table" and finite(state.terrainOrigin[1]) then
            drawWorldCircle(state.terrainOrigin[1], state.terrainOrigin[2], base + 0.2, 90,
                0x6040C0FF, false)
        end
        for key, code in pairs(state.terrain) do
            if (code == 3 or code == 4) and drawn < 200 then
                local cx, cy = terrainUnkey(key)
                local x, y = cx * 2 + 1, cy * 2 + 1
                if (x - px)^2 + (y - py)^2 < 45 * 45 then
                    drawn = drawn + 1
                    local z = base + (code == 4 and 3.0 or 0.1)
                    local color = code == 3 and 0xC0FF5A5A or 0xC0FFD24A
                    dxDrawLine3D(x, y, z, x, y, z + (code == 4 and 1.6 or 1.0), color, 3, false)
                end
            end
        end
    end
    local probe = state.probe
    if probe and type(probe.rays) == "table" and elapsed(now, probe.tick_ms or 0) < 1500 then
        for _, ray in ipairs(probe.rays) do
            local a, b = ray.from, ray.to
            if type(a) == "table" and type(b) == "table" and finite(a[1]) and finite(b[1]) then
                local blocked = ray.clear == false
                dxDrawLine3D(a[1], a[2], a[3], b[1], b[2], b[3], blocked and 0xE0FF4040 or 0x5060FF80, blocked and 3 or 1, false)
                local h = ray.hit
                if blocked and type(h) == "table" and finite(h[1]) then drawWorldCircle(h[1], h[2], h[3], 1.2, 0xFFFF4040, false) end
            end
        end
    end

    line(-322, 177, 322, 177, dim)
    if autopilot.enabled then
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
        local stats = state.flightStats
        if stats then
            local total = elapsed(now, stats.started) / 1000
            local sinceMarker = stats.lastMarkerTick and elapsed(now, stats.lastMarkerTick) / 1000 or nil
            text(string.format("T+%d:%02d   PHASE %.0fs   MARKERS %d (+%ss)   TB %d   HOLD %d   Z-CUT %d",
                math.floor(total / 60), math.floor(total % 60), elapsed(now, stats.phaseSince) / 1000, stats.markers,
                sinceMarker and string.format("%.0f", sinceMarker) or "--", stats.turnbacks, stats.holds, stats.engineCuts), -322, 240, "left", dim, 0.78, 644)
        end
        local aggrText = not state.aggressive and "AGGR OFF: SWITCH" or detail.aggressive_now and "AGGR ON"
            or (detail.route_known and "AGGR ARMED" or "AGGR OFF: ROUTE UNKNOWN (1ST VISIT)")
        text(aggrText .. "   ROUTE " .. (detail.route_known and "KNOWN" or "UNKNOWN") .. "   CHAIN MIN " .. numberText(detail.skim_chain_min_kmh or detail.ring_chain_min_kmh)
            .. "   " .. jobTimerText() .. (state.lastLeg and string.format("   LEG %.1fs BEST %.1fs", state.lastLeg.seconds, state.lastLeg.best) or ""),
            -322, 256, "left", detail.aggressive_now and green or amber, 0.78, 644)
    else
        -- Manual flight: everything needed to come back to a missed marker at the right height.
        text("MARKER  ALT " .. numberText(markerAlt) .. " M     DZ " .. numberText(nav.altitude_error_m, "%+.0f")
            .. " M     DIST " .. numberText(nav.distance_3d_m) .. " M", -322, 187, "left", amber, 0.95, 644)
        local rel = finite(nav.bearing_deg) and finite(item.heading_deg) and angle(nav.bearing_deg - item.heading_deg) or nil
        local relText = rel and (numberText(math.abs(rel), "%.0f") .. " DEG "
            .. (math.abs(rel) < 1 and "AHEAD" or rel < 0 and "LEFT" or "RIGHT")) or "--"
        text("BRG " .. numberText(nav.bearing_deg, "%03.0f") .. "     TURN " .. relText .. "     SIZE " .. numberText(nav.marker_size_m)
            .. " M     " .. string.upper(type(nav.marker_type) == "string" and nav.marker_type or "no target")
            .. "     " .. jobTimerText(), -322, 216, "left", dim, 0.85, 644)
        local last = state.lastFlightStats
        if last and finite(last.total_s) then
            text(string.format("LAST FLIGHT %d:%02d   MARKERS %d   TB %d   HOLD %d   Z-CUT %d   SLOWEST GAP %.0fs   %s",
                math.floor(last.total_s / 60), math.floor(last.total_s % 60), last.markers, last.turnbacks, last.holds, last.engineCuts,
                last.slowestMarkerGap / 1000, tostring(last.reason or "")), -322, 240, "left", dim, 0.78, 644)
        end
    end
end

-- Compact panel at the chosen screen edge: own height, marker height and the difference. Flying by
-- hand a pilot needs these three numbers; everything else stays in the log.
local function drawAltitudePanel(item, now)
    local width, height = guiGetScreenSize()
    if not finite(width) or not finite(height) or width < 1 or height < 1 then return end
    local scale = math.min(width / 1280, height / 800, 1.3)
    local nav = type(item.navigation) == "table" and item.navigation ~= NULL and item.navigation or nil
    local p = item.position_m
    local altitude = type(p) == "table" and p ~= NULL and p[3] or nil
    local markerAlt = nav and type(nav.position) == "table" and nav.position[3] or nil
    local dz = nav and nav.altitude_error_m or nil
    local w, h = 230 * scale, 150 * scale
    local x = state.hudMode == "right" and (width - w - 24 * scale) or 24 * scale
    local y = height * 0.5 - h * 0.5
    dxDrawRectangle(x, y, w, h, 0xA0060C08, false)
    local function num(value, pattern) return finite(value) and string.format(pattern or "%.0f", value) or "--" end
    local function row(label, value, dy, size, color)
        dxDrawText(label, x + 12 * scale, y + dy * scale, x + w, y + (dy + 18) * scale,
            0xFFB0C4B8, 0.8 * scale, "default-bold", "left", "top", false, false, false)
        dxDrawText(value, x + 12 * scale, y + (dy + 14) * scale, x + w - 12 * scale, y + (dy + 50) * scale,
            color, size * scale, "default-bold", "right", "top", false, false, false)
    end
    row("ALT  M", num(altitude), 6, 1.9, 0xFFEEFFF2)
    row("MARKER  M", num(markerAlt), 56, 1.4, 0xFFFFC36A)
    local dzColor = finite(dz) and (math.abs(dz) <= 10 and 0xFF70FF92 or 0xFFFFC36A) or 0xFFB0C4B8
    row("DZ  M", num(dz, "%+.0f"), 100, 1.4, dzColor)
    drawMarkerOverlay(nav, scale, width, height, true)
end

local function onHudRender()
    local now, item = getTickCount(), state.latest
    if not state.hudEnabled or state.hudError or state.minimized or not item
        or read("dfMenuOpen") == true or elapsed(now, state.lastSample) > 300 or not valid(state.vehicle) then
        state.hudSmooth, state.hudLastFrame = nil, nil
        return
    end
    local ok, message = pcall(state.hudMode == "full" and drawAutopilotHud or drawAltitudePanel, item, now)
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
state.aimMemory = state.aimMemory or {}
state.markerVisits = state.markerVisits or {}
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

-- The job window ("Работать") is built from hdx elements of the pilot resource. Log their tree once
-- when it opens, so the honest press can be aimed at the right button instead of guessed.
local function inspectJobWindow()
    local pilotRoot = state.pilotRoot
    if not state.recording or not valid(pilotRoot) then return end
    local buttons = {}
    for _, element in ipairs(read("getElementsByType", "hdxButton", pilotRoot) or {}) do
        local parent = read("getElementParent", element)
        local grand = parent and read("getElementParent", parent)
        local info = hdxButtonInfo(element)
        buttons[#buttons + 1] = {id = elementId(element), element_id = read("getElementID", element) or NULL,
            data = read("getElementData", element, "data") or NULL, text = info.text or NULL,
            rect = {info.x or NULL, info.y or NULL, info.w or NULL, info.h or NULL},
            visible = info.visible == nil and NULL or info.visible,
            parent = parent and {id = elementId(parent), type = read("getElementType", parent), element_id = read("getElementID", parent) or NULL} or NULL,
            grandparent = grand and {id = elementId(grand), type = read("getElementType", grand)} or NULL}
    end
    emit("job_window_buttons", {count = #buttons, buttons = buttons, hdx_access = state.hdxAccess or NULL,
        cursor = read("isCursorShowing"), screen = {read("guiGetScreenSize")}}, true)
end

local function notification(channel, payload, origin, text)
    -- The pilot listens for this event on localPlayer, so its source is the player, not the pilot
    -- resource: accept it by name. Its window elements are created by that handler; look after it ran.
    if channel == "pilot:OpenWorkGui" then
        state.jobWindowTick, state.jobWindowClosedTick = getTickCount(), nil
        read("setTimer", guard(inspectJobWindow, "job_window", true), 400, 1)
    elseif channel == "Pilot:OpenOrderWindow" then
        state.queueWindowTick, state.queueClosedTick = getTickCount(), nil
        read("setTimer", guard(inspectJobWindow, "queue_window", true), 400, 1)
    elseif channel == "Pilot:CloseGUIOrder" then
        state.queueClosedTick, state.queueWindowTick = getTickCount(), nil
    elseif channel == "Pilot:CloseGUIWork" then
        state.jobWindowClosedTick = getTickCount()
    end
    if type(text) ~= "string" then text = table.concat(notificationText(payload), "\n") end
    local plain = text:gsub("#%x%x%x%x%x%x", "")
    -- "У Вас есть 60 секунд, чтобы начать работу пилота. Зайдите на пикап старта работы."
    if plain:find("секунд, чтобы начать работу", 1, true) then
        state.queueCalledTick = getTickCount()
        emit("autonomy_queue", {action = "server_call", text = plain}, true)
    end
    local trusted = origin == "province_pilot" and channel == "province:sendNotification"
    if trusted then
        if plain:find("Взлет разрешен", 1, true) or plain:find("Взлёт разрешен", 1, true) then
            -- The departure chain ends where the server freezes the aircraft and clears the takeoff:
            -- remember that marker as the chain's stop, so the next time the taxi is planned to it.
            local chain = state.learningChain
            if chain and #chain.points > 0 then
                chain.points[#chain.points][6] = true
                chain.name = chain.name .. "_takeoff"
                state.learnedChains[#state.learnedChains + 1] = chain
                state.learningChain = nil
                emit("chain_learned", {name = chain.name, points = #chain.points, reason = "takeoff_permit"}, true)
            end
        end
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
        if mode == "job_ended" and autopilot.enabled and autopilot.airborne then
            -- Fired in the air: the controller keeps flying under alarm and stops itself after touchdown.
            autopilot.status = "Работа завершена в воздухе: удерживаю самолёт до касания"
            emit("autopilot_job_ended_airborne", {text = plain}, true)
            bridge.update("autopilot_status", autopilot.status)
        elseif mode == "completed" or mode == "job_ended" then
            -- A finished job or a firing by the server both put the player on the pickup; the autonomy
            -- re-employs after either. A voluntary quit ("работа завершена") ends the cycle.
            local fired = mode == "job_ended" and (plain:find("уволен", 1, true) or plain:find("не успели", 1, true))
            local repeatJob = (mode == "completed" or fired) and working and state.autonomy
            -- An aggressive flight that got the pilot fired: the next job flies the cautious way, and
            -- the aggression comes back by itself once a job completes. The switch stays as set.
            if fired and autopilot.aggressive then
                autopilot.aggressive, state.aggressiveSuspended = false, true
                emit("aggressive_suspended", {reason = plain, next_job = "cautious"}, true)
            elseif mode == "completed" and state.aggressiveSuspended then
                autopilot.aggressive, state.aggressiveSuspended = state.aggressive, nil
                emit("aggressive_restored", {}, true)
            end
            if repeatJob then emit("autonomy_queued", {reason = "route_completed", wait_for_exit = true}, true) end
            stopAutopilot(autopilot.status)
            -- The job is over: close the recording whether the autopilot or the pilot finished it.
            -- With the autonomy on the recording runs through the cycle on foot and into the next job.
            if state.recording and not repeatJob then stop("job_" .. mode) end
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
    "Pilot:OpenOrderWindow", "Pilot:CloseGUIOrder", "Pilot:SendOrderTableToClient",
    "Pilot:AddToOrderTable", "Pilot:DelFromOrderTable",
}
installNotificationObservers = function()
    for _, event in ipairs(notificationEvents) do
        if not state.hookStatus[event] then
            local name = event
            hook(name, root, function(...)
                local owner = ownership(source)
                if name == "plrTimer:init" or name == "plrTimer:secs" then trackJobTimer(name, source, ...) end
                if not state.recording and not autopilot.enabled and not state.nextJob
                    and owner ~= "province_pilot" and not name:find("^[Pp]ilot:") then return end
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
    if state.jobTimer and source == state.jobTimer.element then state.jobTimer = nil end
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
    -- A real impact on the ground (raw force >= 5; taxi scrapes log 0.5, a hit 45).
    if state.lastOnGround == true and finite(force) and force >= 5 then
        state.groundBumpTick = getTickCount()
        -- Which side of the aircraft the impact point is on (+1 right, -1 left), from the heading.
        local vx, vy = read("getElementPosition", source)
        if finite(x) and finite(y) and finite(vx) and finite(vy) and finite(state.lastHeading) then
            local h = math.rad(state.lastHeading)
            local side = (x - vx) * math.cos(h) - (y - vy) * math.sin(h)
            state.groundBumpSide = side >= 0 and 1 or -1
        else state.groundBumpSide = nil end
    end
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
    -- A ground impact is a bump for the escape logic (groundBumpTick above); handing the aircraft
    -- back at 9 km/h beside the terminal only cost the marker timer (recorded at Liberty).
end, true)
hook("onClientVehicleDamage", root, function(attacker, weapon, loss, x, y, z, tyre)
    if source ~= read("getPedOccupiedVehicle", localPlayer) then return end
    if state.recording then emit("vehicle_damage", {attacker = valid(attacker) and elementId(attacker) or NULL,
        weapon = weapon, loss = loss, tyre = tyre, position = {x or NULL, y or NULL, z or NULL}, context = impactContext()}, true) end
    if autopilot.enabled and not autopilot.airborne and finite(loss) and loss >= 10 then
        if state.lastOnGround == true and finite(state.lastSpeed) and state.lastSpeed < 50 then
            state.groundBumpTick = getTickCount()
            emit("ground_bump", {loss = loss, speed_kmh = state.lastSpeed}, true)
        else
            stopAutopilot("Самолёт получил повреждение")
        end
    end
end, true)
hook("onClientPickupHit", root, function(player)
    if player ~= localPlayer then return end
    state.pickupHitTick = getTickCount()
    if not state.recording then return end
    emit("pickup_hit", {pickup = elementId(source), position = vector("getElementPosition", source) or NULL,
        pickup_type = read("getPickupType", source), interior = read("getElementInterior", source),
        dimension = read("getElementDimension", source)}, true)
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

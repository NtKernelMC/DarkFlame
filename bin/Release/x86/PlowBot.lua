-- PlowBot 2.11.2 — бот дорожной службы (поливомоечная машина, province_snowPlow).
-- Скрипт внедряется DarkFlame прямо в VM ресурса province_snowPlow, поэтому видит его
-- глобалы: tRoutesPoints / tDist / tWaterCapacity / IDEAL_SPEED / CURRENT_ROUTE_ID /
-- CURRENT_POSITION_ID / getJobVehicle / isActiveWaterSpray / getVehicleSpeed.
-- Скорости внутри кода — внутренние км/ч ресурса (getVehicleSpeed = |velocity| * 180).
-- В панель и отчёты уходит скорость спидометра сервера: внутренняя * 0.702.

-- BEGIN EMBEDDED PLOW CONTROLLER
local VERSION = "2.11.2"
-- Спидометр сервера показывает ровно то, что отдаёт getVehicleSpeed ресурса.
-- Проверено на 4604 замерах: getVehicleSpeed / (путь по координатам) = 0.698, то есть
-- это НЕ километры в час движка, а собственные единицы province — и именно их видит
-- игрок. Прежний множитель 0.702 занижал показания на треть (бот «ехал 70» при 49).
local SPEEDO = 1.0
local REAL_KMH = 1 / 0.698   -- перевод в настоящие км/ч, нужен только для расчёта воды
-- Замерено по эталонному заезду (маршрут 1, 6704 сэмпла): вода уходит по ВРЕМЕНИ, не по
-- пути — 1.46 л/с при включённой установке и почти ноль (0.017 л/с) при выключенной.
-- Бак = норма маршрута, то есть 461 л на маршруте 1 = ровно 316 с полива на 5354 м.
-- Отсюда средняя по пути обязана быть не ниже ~61 внутр, иначе вода кончится раньше
-- финиша — именно так и сорвался эталонный заезд (48 точек из 54, оплата нулевая).
-- Поэтому крейсер держим выше IDEAL_SPEED: потеря коэффициента оплаты лучше, чем ноль.
local WATER_RATE = 1.46    -- л/с при включённой установке
local CRUISE = 50          -- по спидометру сервера
-- Полив включаем с 30 по спидометру, выключаем ниже 25 (гистерезис, чтобы не дёргался).
local SPRAY_ON_SPEED = 30
-- Порог выключения держим близко к порогу включения: при прежних 12 (8 по спидометру)
-- установка, раз включившись, продолжала лить на медленном ходу.
local SPRAY_OFF_SPEED = 25
-- Метки светофоров ставились впритык к стоп-линии, поэтому вставать надо почти
-- вплотную: прежние 8 м не давали дотянуться до колсферы точки радиусом 7 м.
local LIGHT_STOP = 3
-- Перед светофором тормозим мягче, чем можем: полный тормозной путь на крейсере
-- почти равен дистанции видимости, и запаса на реакцию не осталось бы совсем.
local LIGHT_DECEL = 3.5    -- м/с², настоящие: мягко, при запасе тормозов около 10
local CAPTURE = 7.0        -- радиус колсферы контрольной точки (work/route-c.lua)

-- ЕДИНИЦЫ. Скорость в контроллере — в единицах спидометра province, а вся физика
-- (тормозной путь, радиус поворота) — в настоящих метрах в секунду. До 2.6.0 формулы
-- делили единицы спидометра на 3.6, как будто это км/ч, и занижали скорость в 1.43
-- раза: тормозной путь выходил вдвое короче настоящего, а в поворот бот разрешал себе
-- в 1.43 раза больше, чем машина может. Отсюда и «поторопился», и «не вписался».
local UNIT_MS = REAL_KMH / 3.6
local function toMs(units) return units * UNIT_MS end
local function toUnits(ms) return ms / UNIT_MS end

-- ФИЗИКА МАШИНЫ, снятая с телеметрии пяти заездов (28.09.2026), а не придуманная.
-- Торможение: медиана 10 м/с², планируем с запасом — контроллер жмёт тормоз не в пол.
local DECEL = 5.5
-- Минимальный радиус на ПОЛНОМ руле растёт со скоростью: GTA урезает выворот колёс.
-- Прежняя модель (база 4 м, руль 35°) давала 5.7 м на любой скорости — в 2.5–6 раз
-- круче реального, поэтому бот был уверен, что успеет довернуть, а не успевал.
-- Пары {скорость м/с, радиус м}.
local TURN_TABLE = {{0, 6.0}, {3.5, 6.3}, {6.5, 9.6}, {9.5, 12.9}, {13, 18.9},
    {19, 32.9}, {30, 60}}
local function minRadius(ms)
    ms = math.abs(ms)
    for i = 2, #TURN_TABLE do
        local v1, r1 = TURN_TABLE[i][1], TURN_TABLE[i][2]
        if ms <= v1 then
            local v0, r0 = TURN_TABLE[i - 1][1], TURN_TABLE[i - 1][2]
            return r0 + (r1 - r0) * (ms - v0) / (v1 - v0)
        end
    end
    return TURN_TABLE[#TURN_TABLE][2]
end
-- Обратная задача: самая большая скорость (м/с), на которой радиус ещё достижим.
local function speedForRadius(radius)
    if radius <= TURN_TABLE[1][2] then return 0 end
    for i = 2, #TURN_TABLE do
        local v1, r1 = TURN_TABLE[i][1], TURN_TABLE[i][2]
        if radius <= r1 then
            local v0, r0 = TURN_TABLE[i - 1][1], TURN_TABLE[i - 1][2]
            return v0 + (v1 - v0) * (radius - r0) / (r1 - r0)
        end
    end
    return TURN_TABLE[#TURN_TABLE][1]
end
-- Частичный руль: кривизна растёт примерно как квадрат отклонения
-- (замер: руль 0.3 -> радиус 132 м, 0.5 -> 40 м, 0.9 -> 14 м).
local function steerForCurvature(curvature, ms)
    local full = 1 / minRadius(ms)
    local share = math.min(1, math.abs(curvature) / full)
    return (curvature >= 0 and 1 or -1) * math.sqrt(share)
end

local Controller = {}
Controller.__index = Controller

local function finite(v) return type(v) == "number" and v == v and math.abs(v) < math.huge end
local function clamp(v, low, high) return math.max(low, math.min(high, v)) end
local function angle(v) return (v + 180) % 360 - 180 end

-- Курс машины в MTA: 0 = север (+Y), растёт против часовой стрелки.
local function bearingTo(dx, dy) return math.deg(math.atan2(-dx, dy)) end

-- Скорость в повороте, по спидометру — нижняя граница эталонного заезда 28.09 (ride2):
-- 12–20° водитель проходил на 39–49, 30–37° на 36–46, 50–60° на 23–43, 86–91° на 27–32,
-- метку 9 (больше 100°) на 27. Бот раньше входил в 50° на 45, в 100° на 28 и уже в самом
-- повороте тормозил в пол. Между узлами таблицы — плавно.
local CORNER_TABLE = {{15, 48}, {30, 42}, {50, 36}, {65, 31}, {90, 26}, {110, 16}, {130, 12}}
local function cornerSpeed(turn, cruise)
    cruise = cruise or CRUISE
    turn = math.abs(turn or 0)
    local first, last = CORNER_TABLE[1], CORNER_TABLE[#CORNER_TABLE]
    if turn < first[1] then return cruise end
    if turn >= last[1] then return math.min(last[2], cruise) end
    for i = 2, #CORNER_TABLE do
        local a, b = CORNER_TABLE[i - 1], CORNER_TABLE[i]
        if turn < b[1] then
            return math.min(a[2] + (b[2] - a[2]) * (turn - a[1]) / (b[1] - a[1]), cruise)
        end
    end
    return cruise
end

-- Объезд: ширина полосы и на каком запасе считаем соседнюю полосу свободной.
local LANE = 3.5
local AVOID_TRIGGER = 28   -- ближе этого начинаем искать объезд
local AVOID_CLEAR = 45     -- дальше этого помеха уже не мешает
local AVOID_RATE = 0.09    -- м за кадр: смещаемся плавно, а не рывком

-- Путь (м), нужный чтобы сбросить скорость с v до goal; оба — в единицах спидометра.
local function brakeDistance(v, goal)
    local from, to = toMs(v), toMs(goal)
    if to >= from then return 0 end
    return (from * from - to * to) / (2 * DECEL)
end

function Controller.new()
    return setmetatable({stuckSince = nil, escapeUntil = nil, escapeSide = 0,
        blockedSince = nil, offset = 0, avoidSide = 0, gasOn = nil, obstacleHold = false,
        detourSide = 0, detourUntil = nil}, Controller)
end

function Controller:reset()
    self.stuckSince, self.escapeUntil, self.escapeSide = nil, nil, 0
    self.blockedSince = nil
    self.offset, self.avoidSide, self.gasOn = 0, 0, nil
    self.obstacleHold, self.detourSide, self.detourUntil = false, 0, nil
    self.nudgeUntil, self.nudgeSteer, self.afterSteer = nil, nil, nil
    self.avoidX, self.avoidY, self.pendingNudge, self.steerFirstUntil = nil, nil, nil, nil
    self.rescan = nil
    self.escapeFrom, self.escapeDistance = nil, nil
end

-- Куда объезжать упёршуюся помеху: -1 — влево, 1 — вправо. Сторона цели, если там
-- хватает сдвига до 3 м, иначе та, где сдвиг меньше. obstacleSide — где помеха
-- относительно оси машины (плюс — справа).
local DETOUR = 2.5          -- на столько метров уводим прицел после отката
local DETOUR_MS = 6000      -- и держим так, пока машина не обойдёт помеху
local function detourSide(err, obstacleSide)
    local toward = err >= 0 and -1 or 1
    if not finite(obstacleSide) then return toward end
    -- Сдвиг оси, чтобы кузов (1.25 м) прошёл мимо помехи с запасом 0.4 м.
    local needLeft = math.max(0, obstacleSide + 1.65)
    local needRight = math.max(0, 1.65 - obstacleSide)
    local need = toward < 0 and needLeft or needRight
    if need <= 3 then return toward end
    return needLeft < needRight and -1 or 1
end

-- Помеха, которую объезжаем, ещё впереди (или сбоку — корпус ещё не прошёл её)?
function Controller:avoidAhead(data)
    -- Помеха сбоку ближе 3 м — корпус ещё идёт вдоль неё (длину машины впереди мы не знаем:
    -- луч видел только её задний край). В тесте бот возвращался в полосу и шёл бортом по
    -- стоящей машине.
    if self.avoidSide < 0 and finite(data.sideRight) and data.sideRight < 3 then return true end
    if self.avoidSide > 0 and finite(data.sideLeft) and data.sideLeft < 3 then return true end
    if not self.avoidX or not finite(data.heading) then return false end
    local rad = math.rad(data.heading)
    local dx, dy = self.avoidX - data.position.x, self.avoidY - data.position.y
    if dx * dx + dy * dy > 45 * 45 then return false end
    return dx * -math.sin(rad) + dy * math.cos(rad) > -6
end

-- Решение об объезде. Помеха впереди, соседняя полоса свободна — уходим на неё и
-- возвращаемся, когда помеха осталась позади. Если обе стороны заняты — тормозим.
-- data.avoid: nil — объезд в любую сторону; "toward" — только в сторону цели (towardSide:
-- -1 влево, 1 вправо); "none" — не перестраиваемся вовсе.
function Controller:planAvoid(data, want, towardSide)
    local gap = data.obstacle
    -- В депо узко — перестраиваться некуда. К своей метке — только в её сторону: путь
    -- проложил водитель, а бот уводил машину от метки (у будки — вправо, метка слева).
    if data.avoid == "none" then self.avoidSide = 0 end
    -- Машина с водителем стоит (очередь, светофор) — она уедет: по встречке её не
    -- объезжаем. Брошенную машину, столб, пешехода — объезжаем.
    -- На соседнюю полосу — только ради брошенной машины (и застрявшей с водителем), человека
    -- или столба прямо по оси. Столб, стена, бордюр у края полосы — это «протиснуться» внутри
    -- полосы и тормоз, не перестроение: в заезде 2.10.3 бот 25 раз уходил в объезд от обочины
    -- на поворотах — на тротуар, трамвайные пути и встречку. Машину в потоке не обгоняем.
    local kind = data.obstacleKind or (data.obstacleVehicle and "queue") or "static"
    local central = finite(gap) and finite(data.obstacleCentral) and data.obstacleCentral <= gap + 1.5
    local worthy = kind == "parked" or kind == "ped"
        or (kind == "static" and (central or (finite(data.obstacleSide) and math.abs(data.obstacleSide) < 0.9)))
    local blocked = data.avoid ~= "none" and finite(gap) and gap < AVOID_TRIGGER and worthy
    if blocked and self.avoidSide == 0 then
        local left = not finite(data.gapLeft) or data.gapLeft > AVOID_CLEAR
        local right = not finite(data.gapRight) or data.gapRight > AVOID_CLEAR
        if data.avoid == "toward" then
            if towardSide < 0 and left then self.avoidSide = -1
            elseif towardSide > 0 and right then self.avoidSide = 1 end
        -- Левая полоса приоритетнее: обгон по встречной короче, чем по обочине.
        elseif left then self.avoidSide = -1
        elseif right then self.avoidSide = 1 end
    elseif not blocked and (not finite(gap) or gap > AVOID_CLEAR) and not self:avoidAhead(data) then
        -- Помеха пропала из лучей — это ещё не значит, что мы её прошли: лучи идут по дуге
        -- объезда мимо неё. Возвращались раньше времени — и прямо в неё (тест 2.9.3: четыре
        -- касания стоящей машины). Сторону отпускаем, только когда помеха позади.
        self.avoidSide, self.avoidX, self.avoidY = 0, nil, nil
    end
    if self.avoidSide ~= 0 and finite(gap) and finite(data.obstacleX) then
        self.avoidX, self.avoidY = data.obstacleX, data.obstacleY
    end
    -- Ушли на полосу, а там тоже занято — откатываемся и тормозим.
    if self.avoidSide == -1 and finite(data.gapLeft) and data.gapLeft < 12 then
        self.avoidSide = 0
    elseif self.avoidSide == 1 and finite(data.gapRight) and data.gapRight < 12 then
        self.avoidSide = 0
    end
    -- Не дальше полосы от пути: объезд, обход после отката и «протиснуться» складывались до 7.5 м.
    local target = clamp(want + self.avoidSide * LANE, -LANE - 0.5, LANE + 0.5)
    if self.offset < target then
        self.offset = math.min(target, self.offset + AVOID_RATE)
    elseif self.offset > target then
        self.offset = math.max(target, self.offset - AVOID_RATE)
    end
    return self.offset
end

-- Выход из тупика. Раньше — всегда 1.8 с назад с рулём в сторону обхода, вслепую: в СТО
-- между машинами бот сдавал назад и снова упирался. Теперь бот щупает лучами дуги вперёд
-- и назад (data.scanEscape) и выбирает: объехать вперёд без отката, откатиться по свободной
-- дуге на нужные метры и выйти вперёд, или стоять, если выхода нет. Без скана (тесты
-- контроллера) — прежний откат.
local NUDGE_MS = 4000
function Controller:startEscape(out, data, err, now, reason)
    self.stuckSince, self.blockedSince = nil, nil
    self.obstacleHold = false
    local speed = finite(data.speed) and data.speed or 0
    -- С какой стороны была помеха, в которую упёрлись (плюс — справа): после прямого выхода
    -- обходим в другую сторону. Новый скан после отката помнит её с первого раза.
    if reason ~= "escape_rescan" and finite(data.obstacleSide) then self.blockSide = data.obstacleSide end
    local plan = type(data.scanEscape) == "function" and data.scanEscape(err) or nil
    if plan and plan.mode == "boxed" then
        out.reason = "boxed"
        out.detail.escape = "boxed"
        if speed < 3 then out.handbrake = true else out.brake = 1 end
        return out
    end
    -- Руль сначала выворачиваем на месте, как водитель: колёса поворачиваются не сразу, и
    -- в тесте машина, тронувшись с прямыми колёсами, развернулась на 7° вместо 28°.
    self.steerFirstUntil = speed < 1 and now + 350 or nil
    if plan and plan.mode == "forward" then
        self.nudgeSteer, self.nudgeUntil = plan.steer, now + NUDGE_MS + (self.steerFirstUntil and 350 or 0)
        self.nudgeFrom = {x = data.position.x, y = data.position.y}
        -- Выход прямо — обход держим в сторону от помехи: иначе после выхода руль вёл к цели
        -- и снова в тот же столб (тест 2.10.3: столб в 1.9 м от прямой, второй удар).
        local side = plan.side
        if side == 0 and finite(self.blockSide) then side = self.blockSide > 0 and -1 or 1 end
        self.detourSide, self.detourUntil = side, now + NUDGE_MS + DETOUR_MS
        out.reason = "nudge_start"
        out.steer = plan.steer
        -- Лучи следующего кадра — по дуге выхода, а не по прежней: иначе они видели машину
        -- впереди ближе 2 м и отменяли выход (в тесте бот так простоял 20 минут).
        out.detail.curvature = (plan.steer >= 0 and 1 or -1) * plan.steer ^ 2 / minRadius(2)
        out.detail.escape = "forward"
        return out
    end
    self.escapeUntil = now + (plan and plan.ms or 1800) + (self.steerFirstUntil and 350 or 0)
    self.escapeFrom = {x = data.position.x, y = data.position.y}
    self.escapeDistance = plan and plan.back or nil
    -- Задним ходом машина отклоняется в сторону, обратную повороту руля.
    self.escapeSide = plan and plan.steer or detourSide(err, data.obstacleSide)
    self.afterSteer = plan and plan.forward or nil
    local side = plan and plan.side or self.escapeSide
    self.detourSide = side
    self.detourUntil = self.escapeUntil + (self.afterSteer and NUDGE_MS or 0) + DETOUR_MS
    out.reason = reason
    out.brake, out.throttle, out.steer = 1, 0, self.escapeSide
    out.detail.escape = plan and "scan" or "blind"
    return out
end

function Controller:update(data, now)
    local out = {throttle = 0, brake = 0, steer = 0, handbrake = false, detail = {}}
    if type(data) ~= "table" or type(data.position) ~= "table" then
        out.reason = "no_data"
        return out
    end
    if data.frozen then
        out.reason = "frozen"
        return out
    end
    if type(data.target) ~= "table" then
        out.reason = "no_target"
        out.brake, out.handbrake = 1, true
        return out
    end

    local speed = finite(data.speed) and data.speed or 0
    local cruise = finite(data.limit) and data.limit > 5 and data.limit or CRUISE
    -- Лёгкий дрейф крейсера: живая машина не едет ровно по линейке.
    if finite(data.jitter) then cruise = math.max(12, cruise + data.jitter) end
    local dx = data.target.x - data.position.x
    local dy = data.target.y - data.position.y
    local dist = finite(data.distance) and data.distance or math.sqrt(dx * dx + dy * dy)
    local heading = finite(data.heading) and data.heading or 0

    -- Поворот в самой точке считаем по ИСХОДНОМУ направлению, до всяких смещений.
    local turn = 0
    if type(data.nextTarget) == "table" and dist > 0.5 then
        local tx, ty = data.nextTarget.x - data.target.x, data.nextTarget.y - data.target.y
        turn = angle(bearingTo(tx, ty) - bearingTo(dx, dy))
    end
    out.detail.turn = turn
    -- Поворот за СЛЕДУЮЩЕЙ целью. У метки 4 бот ехал к точке 22 на 49 и узнал о повороте
    -- 66° за меткой, только взяв точку, — за 15 м: тормоз в пол и руль 0.87 на 47.
    local turnAfter, afterGap = 0, nil
    if type(data.nextTarget) == "table" and type(data.afterTarget) == "table" then
        local ax, ay = data.nextTarget.x - data.target.x, data.nextTarget.y - data.target.y
        local bx, by = data.afterTarget.x - data.nextTarget.x, data.afterTarget.y - data.nextTarget.y
        local gap = math.sqrt(ax * ax + ay * ay)
        if gap > 0.5 and bx * bx + by * by > 0.25 then
            turnAfter = angle(bearingTo(bx, by) - bearingTo(ax, ay))
            afterGap = gap
            out.detail.turn_after = turnAfter
        end
    end

    -- Вынос в поворот. Если целиться строго в точку, машина срезает угол и уезжает
    -- на тротуар или встречку. Поэтому на подходе к крутой точке держимся СНАРУЖИ
    -- дуги, а у самой точки вынос сходит на нет — там едем по центру.
    -- К своим меткам и в депо выноса нет: траекторию там уже проложил водитель, а вынос
    -- на 3 м наружу перед правым поворотом за меткой 3 увёл машину в столбик ворот депо.
    -- С 2.9.0 выноса нет: поворот начинается заранее и плавно (заход в поворот в
    -- цепочке), а вынос наружу тянул прицел в обратную сторону — поворот выходил поздним
    -- и резким.
    local apex = 0

    -- Боковое смещение цели: сюда складываются вынос, объезд и подсказка Q/E.
    -- Целимся не в саму точку, а в точку, сдвинутую поперёк линии движения.
    -- После отката от помехи держим прицел в стороне обхода, пока не проедем её:
    -- иначе pure pursuit сразу возвращал машину на ту же линию и в тот же столб.
    local detour = 0
    if self.detourUntil and now < self.detourUntil then
        detour = self.detourSide * DETOUR
        out.detail.detour = detour
    else
        self.detourUntil, self.detourSide = nil, 0
    end
    local squeeze = finite(data.squeeze) and data.squeeze or 0
    if squeeze ~= 0 then out.detail.squeeze = squeeze end
    -- Сторона цели — до всяких смещений и по самой цели, а не по прицелу руля: у своей
    -- метки прицел уже тянет в поворот за ней, и объезд «в сторону метки» уводил машину
    -- к столбу за меткой 6 (тест, 2.9.2).
    local toward = angle(bearingTo(dx, dy) - heading) >= 0 and -1 or 1
    -- Руль ведём на точку упреждения на линии, если она есть; торможение и поворот
    -- выше посчитаны по настоящей цели.
    if type(data.steerTarget) == "table" then
        dx = data.steerTarget.x - data.position.x
        dy = data.steerTarget.y - data.position.y
    end
    local aimLen = math.sqrt(dx * dx + dy * dy)
    local offset = self:planAvoid(data, apex + detour + squeeze, toward)
    out.detail.offset = offset
    out.detail.avoid_side = self.avoidSide
    if math.abs(offset) > 0.05 and aimLen > 0.5 then
        -- Смещение считаем не от далёкой цели, а от точки примерно в секунде пути:
        -- иначе сдвиг на 2 м при цели в 300 м дал бы доворот в четверть градуса.
        local lead = clamp(toMs(speed) * 1.2, 10, 30)
        lead = math.min(lead, aimLen)
        local nx, ny = dx / aimLen, dy / aimLen
        dx = nx * lead + ny * offset
        dy = ny * lead - nx * offset
    end
    local err = angle(bearingTo(dx, dy) - heading)
    out.detail.err = err
    out.detail.dist = dist

    -- Цель-остановка (база локации): в неё надо встать, а не проехать её насквозь.
    -- В место сдачи встаём почти в центре (arrive = 2 м): раньше бот вставал за 3.5 м, и
    -- сервер не засчитывал «Сдачу транспорта»; водитель в эталоне встал в 0.9 м. База — как
    -- раньше, 5 м. Если центр уже сбоку или позади — тоже встали, не кружим.
    local arrive = finite(data.arrive) and data.arrive or 5
    if data.stopAt and (dist <= arrive or (dist <= 5 and math.abs(err) > 90)) then
        out.reason = "arrived"
        out.brake, out.handbrake = speed >= 3 and 1 or 0, true
        out.spray = false
        self.stuckSince, self.blockedSince = nil, nil
        return out
    end

    -- Освобождение из застревания: короткий ход назад с рулём в сторону нужного курса.
    -- Откат кончается по времени, по пройденным метрам (скан сказал, сколько свободно) или
    -- когда задний бампер подошёл к помехе ближе метра.
    if self.escapeUntil and now < self.escapeUntil then
        local moved = self.escapeFrom and math.sqrt((data.position.x - self.escapeFrom.x) ^ 2
            + (data.position.y - self.escapeFrom.y) ^ 2) or 0
        -- Сколько ещё прокатится назад, когда отпустим: грузовик гасит ход ~4 м/с². Скорость —
        -- по модулю: где она со знаком, «меньше 6» было верно всегда, и бот разгонялся назад.
        local backSpeed = math.abs(speed)
        local rolling = toMs(backSpeed) ^ 2 / 8
        if (self.escapeDistance and moved + rolling >= self.escapeDistance)
            or (finite(data.rearGap) and data.rearGap < 1.0 + rolling) then
            self.escapeUntil = now
        elseif self.steerFirstUntil and now < self.steerFirstUntil then
            out.reason = "escape"
            out.steer, out.handbrake = self.escapeSide, true
            out.detail.escape_ms_left = self.escapeUntil - now
            return out
        else
            out.reason = "escape"
            -- Задний ход не быстрее ~2.5 м/с: в тесте бот разгонялся до 5 м/с и укатывался
            -- на 13 м вместо 6.
            out.brake = backSpeed < 6 and 1 or 0
            out.steer = self.escapeSide
            out.detail.escape_ms_left = self.escapeUntil - now
            return out
        end
    end
    if self.escapeUntil then
        self.escapeUntil, self.stuckSince = nil, nil
        self.escapeFrom, self.escapeDistance = nil, nil
        -- После отката — снова скан, уже с того места, где машина реально встала: откат
        -- выходит не точно по плану (руль доворачивается не сразу), и дуга «прямо», выбранная
        -- до отката, в тесте вела обратно в тот же столб. Без скана — дуга из плана.
        if type(data.scanEscape) == "function" then
            self.rescan, self.afterSteer = true, nil
        elseif self.afterSteer then
            self.pendingNudge, self.afterSteer = self.afterSteer, nil
        end
    end
    -- Машина катится назад не по команде отката (после отката, после удара): гасим ход
    -- газом с прямым рулём. Скорость у игры без знака, и контроллер решал «быстро —
    -- тормози», а тормоз на заднем ходу в MTA разгоняет назад ещё сильнее: в тесте
    -- грузовик так уехал задом на полмаршрута. Руль «как вперёд» уводил бы нос не туда.
    if data.reversing then
        out.reason = "settle"
        out.throttle = 1
        self.stuckSince, self.blockedSince = nil, nil
        return out
    end

    -- Выход вперёд по дуге, которую скан нашёл свободной: руль держим, едем тихо. Помеха
    -- у самого носа — дуга всё-таки занята: стоп, дальше решаем заново.
    if self.rescan and math.abs(speed) < 1 then
        self.rescan = nil
        return self:startEscape(out, data, err, now, "escape_rescan")
    end
    if self.pendingNudge then
        self.nudgeSteer, self.nudgeUntil, self.pendingNudge = self.pendingNudge, now + NUDGE_MS + 350, nil
        self.nudgeFrom = {x = data.position.x, y = data.position.y}
        self.steerFirstUntil = speed < 1 and now + 350 or nil
    end
    -- Выход длится 7 м пути (не дольше NUDGE_MS): за 1.5 с бот проезжал 3–4 м, машину
    -- впереди не обходил, и управление тут же возвращало его к цели — в неё же.
    local nudged = self.nudgeFrom and math.sqrt((data.position.x - self.nudgeFrom.x) ^ 2
        + (data.position.y - self.nudgeFrom.y) ^ 2) or 0
    if self.nudgeUntil and now < self.nudgeUntil and nudged < 7 then
        -- Дорогу на выходе проверяем тем же, чем скан, — путём углов кузова (nudgeFree). Лучи
        -- от носа по дуге рисуют нос на той же дуге, что и центр, хотя его выносит шире, и
        -- видели помеху там, где кузов проходит.
        local blockedAhead = finite(data.nudgeFree) and data.nudgeFree < 0.8
            or (not finite(data.nudgeFree) and finite(data.obstacle) and data.obstacle < 2)
        if blockedAhead then
            self.nudgeUntil = nil
        elseif self.steerFirstUntil and now < self.steerFirstUntil then
            out.reason = "nudge"
            out.steer, out.handbrake = self.nudgeSteer, true
            out.detail.curvature = (self.nudgeSteer >= 0 and 1 or -1) * self.nudgeSteer ^ 2 / minRadius(2)
            return out
        else
            local ms = math.max(toMs(speed), 2)
            out.reason = "nudge"
            out.steer = self.nudgeSteer
            out.detail.curvature = (self.nudgeSteer >= 0 and 1 or -1) * self.nudgeSteer ^ 2 / minRadius(ms)
            if speed < 8 then out.throttle = 0.5 elseif speed > 12 then out.brake = 0.3 end
            self.stuckSince, self.blockedSince, self.obstacleHold = nil, nil, false
            return out
        end
    elseif self.nudgeUntil then
        self.nudgeUntil = nil
    end

    local corner = cornerSpeed(turn, cruise)
    local goal = cruise
    -- Тормозной путь считаем до входа в сферу, а не до центра точки.
    -- Тормозим к повороту заранее и с запасом: тормоз дозируется мягче расчётных 5.5 м/с²,
    -- и к повороту у метки 5 бот приходил на 26 вместо 20.
    if dist - 5 <= brakeDistance(speed, corner) * 1.25 then goal = corner end
    -- Поворот за следующей целью: скорость — по кривой торможения до него (тот же запас
    -- 1.25, что и выше), без ступеньки. Ступенька «крейсер ↔ поворот» на сравнении
    -- тормозного пути дёргала тормоз: сбросили — условие отпустило — снова газ.
    if afterGap then
        local cornerAfter = cornerSpeed(turnAfter, cruise)
        local room = math.max(0, dist + afterGap - 5)
        local allowed = toUnits(math.sqrt(toMs(cornerAfter) ^ 2 + 2 * DECEL * room / 1.25))
        if allowed < goal then
            goal = allowed
            out.detail.corner_after = allowed
        end
    end
    -- Своя ошибка курса тоже ограничивает скорость: нельзя вписаться в 7 м боком.
    goal = math.min(goal, cornerSpeed(err, cruise))
    -- Изгибы пути впереди: путь построен заранее, и скорость под каждый изгиб известна до него
    -- (с тормозным путём) — как у самолёта перед поворотом.
    if finite(data.pathSpeed) then
        goal = math.min(goal, math.max(data.pathSpeed, 8))
        out.detail.path_speed = data.pathSpeed
    end
    if data.finishing then goal = math.min(goal, 20) end

    -- Разворот: цель за спиной, рулём тут не помочь, нужно почти остановиться.
    if math.abs(err) >= 130 then goal = math.min(goal, 8) end

    -- Подъезд к базе: гасим скорость так, чтобы хватило тормозного пути до остановки.
    if data.stopAt then
        local room = math.max(0, dist - math.min(arrive, 3) * 0.5)
        goal = math.min(goal, toUnits(math.sqrt(2 * DECEL * room)))
    end

    -- Препятствие впереди (трафик, бордюр, столб): точки соединены прямой, дорога — нет.
    if finite(data.obstacle) then
        local gap = data.obstacle
        out.detail.obstacle = gap
        -- Порог каждой ступени берётся с запасом к её тормозному пути: 24 км/ч гасятся
        -- за 4.5 м, 40 км/ч — за 12 м, крейсерские 52 — за 21 м.
        -- Встали — стоим, пока помеха не дальше 7.5 м. Без запаса бот качался на
        -- границе 6 м «газ-тормоз» по 10–30 с, а таймер отката каждый раз сбрасывался.
        if gap < 6 or (self.obstacleHold and gap < 7.5) then
            goal = 0
            self.obstacleHold, self.holdClear = true, nil
        elseif gap < 12 then goal = math.min(goal, 12)
        elseif gap < 24 then goal = math.min(goal, 24)
        elseif gap < 38 then goal = math.min(goal, 40)
        end
        if gap >= 7.5 then self.obstacleHold = false end
    elseif self.obstacleHold then
        -- Помеха пропала — отпускаем стоп не сразу: луч мог моргнуть. У метки 4 бот 20 с
        -- качался «стоп — газ — стоп», и откат назад так и не включился.
        self.holdClear = self.holdClear or now
        if now - self.holdClear >= 600 then
            self.obstacleHold, self.holdClear = false, nil
        else
            goal = 0
        end
    end
    -- Человек на дороге: мимо — не быстрее 25, даже если соседняя полоса свободна.
    if data.obstacleKind == "ped" and finite(data.obstacle) and data.obstacle < 30 then
        goal = math.min(goal, 25)
    end
    -- Ударились о машину спереди (подрезали, встали перед носом): стоим, а не толкаем.
    if data.yield then
        goal = 0
        out.detail.yield = true
    end
    -- Низкая помеха (бордюр, подъём дороги) — не стена, машина её переезжает. Только
    -- не на скорости: у точки 26 и метки 13 бот стоял перед таким по 13 с.
    -- С 25 м: с крейсерских 50 до 25 машине нужно ~15 м тормозного пути.
    if finite(data.curb) and data.curb < 25 then goal = math.min(goal, 25) end
    out.detail.corner = corner

    -- Светофор. Состояние игры одно на все перекрёстки, поэтому у каждого запомнено
    -- своё «зелёное» значение — оно зависит от направления улицы (как в TramBot).
    local atLight = false
    if data.light then
        out.detail.light = data.light.green and "green" or "red"
        out.detail.light_distance = data.light.distance
        if not data.light.green then
            local room = math.max(0, data.light.distance - LIGHT_STOP)
            goal = math.min(goal, toUnits(math.sqrt(2 * LIGHT_DECEL * room)))
            -- У самого красного не разгоняемся: только держим или гасим. Дальше 15 м
            -- хватает кривой торможения. Раньше это действовало на любом расстоянии, и бот,
            -- вставший перед столбом в 20 м от красного (у метки 7), не мог тронуться:
            -- цель min(…, 0) = 0, пока не загорится зелёный.
            if data.light.distance <= LIGHT_STOP + 12 then goal = math.min(goal, speed) end
            -- Доползать вплотную к линии не нужно: если уже почти встали рядом — стоим.
            -- Иначе бот газовал в паре метров от красного и уходил в откат назад.
            if data.light.distance <= LIGHT_STOP
                or (speed < 6 and data.light.distance <= LIGHT_STOP + 5) then
                goal = 0
                atLight = true
            end
        end
    end

    -- Выезд из депо: до первой серверной точки едем 15–20 и не поливаем — там узко,
    -- а сразу за выездом поворот. Скорость выезда своя на каждый рейс.
    if data.depot then
        goal = math.min(goal, finite(data.depotSpeed) and data.depotSpeed or 18)
        out.detail.depot = true
    end

    -- Полив: вода уходит по времени, поэтому на стоянке и ползком установку глушим —
    -- это прямая экономия бюджета, а зачёт точки полива не требует (проверено логом).
    local wantSpray = data.spray ~= false
    if speed >= SPRAY_ON_SPEED then wantSpray = true
    elseif speed < SPRAY_OFF_SPEED then wantSpray = false end
    -- На красный воду глушим сразу: стоять с открытой установкой — чистый слив бака.
    if atLight or (data.light and not data.light.green) then wantSpray = false end
    if finite(data.liters) and finite(data.routeLeft) and data.liters > 0 then
        local waterSeconds = data.liters / WATER_RATE
        -- Время до конца считаем на ОЖИДАЕМОЙ скорости, а не на текущей: на старте и в
        -- повороте скорость мала, и правило решало, что воды не хватит, глуша полив до 45.
        local expected = toMs(math.max(speed, cruise * 0.9))
        local routeSeconds = data.routeLeft / math.max(1, expected)
        out.detail.water_seconds = waterSeconds
        out.detail.route_seconds = routeSeconds
        out.detail.water_ok = waterSeconds >= routeSeconds
        -- Бюджет трещит: глушим установку на всём, что медленнее крейсера.
        if waterSeconds < routeSeconds and speed < 45 then wantSpray = false end
    end
    if data.depot then wantSpray = false end
    out.spray = wantSpray

    -- Итоговая цель по скорости пишется только здесь: выше её правят и поворот,
    -- и помеха, и светофор, и подъезд к базе.
    -- Руль по методу pure pursuit: считаем дугу, которая приводит машину точно в цель
    -- (кривизна = 2·sin(ошибки курса) / расстояние), и переводим её в положение руля по
    -- измеренной характеристике машины. Прежний закон «ошибка / коэффициент» давал на
    -- 10° ошибки руль 0.3–0.5, а на реальной машине это почти прямая (радиус 130 м) —
    -- отсюда «плавно, как на геймпаде» и вечный недоворот в метку.
    local ms = toMs(speed)
    local aimDist = math.sqrt(dx * dx + dy * dy)
    -- Дуга — на расстояние до прицела. Короткую «резкую» дугу при ошибке курса от 15°
    -- водитель забраковал: на точке 22 руль уходил в упор, машину выкидывало к краю, и
    -- дальше шла цепочка до столба у точки 26. Повороты теперь начинаются заранее —
    -- прицел сам переходит на следующий отрезок (см. «заход в поворот» в цепочке).
    -- Но если цель ушла в сторону больше чем на 50° (поворот 87° у метки 5), дуга на всё
    -- расстояние выходит радиусом 28 м вместо 10–15 и выносит машину на 27 м: там дугу
    -- постепенно укорачиваем до 1.5 с пути, к 90° — полностью.
    -- Дуга — на 2 с пути вперёд (15–40 м), как у автопилотов: на всё расстояние до далёкой
    -- цели она выходила ленивой (у метки 5 и точки 23 — вынос на 10–20 м), а на 1.5 с с
    -- порогом 15° — резкой (точка 22). Сразу после угла круче 80° — короче, 1.2 с: угол
    -- проходим «квадратом», как водитель у метки 5, пока не выровнялись.
    local lookahead = math.min(aimDist, clamp(ms * 2.0, 15, 40))
    if data.sharp and math.abs(err) > 15 then lookahead = math.min(aimDist, clamp(ms * 1.2, 8, 20)) end
    out.detail.lookahead = lookahead
    local curvature
    if math.abs(err) >= 90 then
        -- Цель сбоку или сзади: дуга pure pursuit тут вырождается, крутим до упора.
        curvature = (err >= 0 and 1 or -1) / minRadius(ms)
    else
        curvature = 2 * math.sin(math.rad(err)) / math.max(lookahead, 1)
    end
    out.steer = steerForCurvature(curvature, ms)
    out.detail.curvature = curvature

    -- Борт. Если с той стороны, куда крутим руль, близко препятствие — доворот туда
    -- режем: именно так бот трижды снёс столбы, имея чистую дорогу впереди.
    local sideLeft, sideRight = data.sideLeft, data.sideRight
    out.detail.side_left, out.detail.side_right = sideLeft, sideRight
    local function trim(gap)
        if not finite(gap) then return 1 end
        -- Часть руля оставляем всегда: иначе бот, идущий впритык вдоль столбов,
        -- не смог бы даже поправить курс и ушёл бы с дороги.
        return clamp((gap - 0.5) / 1.1, 0.3, 1)
    end
    if out.steer > 0 then out.steer = out.steer * trim(sideLeft)
    elseif out.steer < 0 then out.steer = out.steer * trim(sideRight) end

    -- Скорость режем только когда реально доворачиваем в сторону помехи. Столбы
    -- вдоль крайней полосы и обгоняющая машина сами по себе — не повод тормозить.
    local nearest = math.min(finite(sideLeft) and sideLeft or 99,
        finite(sideRight) and sideRight or 99)
    local turningInto = (out.steer > 0.15 and finite(sideLeft) and sideLeft < 2.2)
        or (out.steer < -0.15 and finite(sideRight) and sideRight < 2.2)
    if nearest < 0.8 then
        -- Борт почти касается — тут уже неважно, куда руль: надо сбавить.
        goal = math.min(goal, 15)
        out.detail.side_tight = nearest
    elseif turningInto then
        goal = math.min(goal, nearest < 1.4 and 20 or 34)
        out.detail.side_tight = nearest
    end

    -- Скорость, на которой нужная дуга ФИЗИЧЕСКИ достижима: по измеренной таблице
    -- минимальных радиусов, с запасом 15%. Если ехать быстрее, машина просто не сможет
    -- описать дугу в цель, сколько руль ни крути, — и пролетит мимо метки.
    if math.abs(curvature) > 1e-4 then
        local need = 1 / math.abs(curvature)
        local safe = math.max(toUnits(speedForRadius(need * 0.85)), 8)
        out.detail.turn_limit = safe
        goal = math.min(goal, safe)
    end

    -- Жёсткий потолок: под горку машина разгонялась до 90, обзора лучей на это не хватает.
    if speed > cruise + 6 then goal = math.min(goal, cruise) end

    out.detail.goal = goal
    local diff = goal - speed
    if goal <= 0 then
        -- Впритык к препятствию: газ не держим, он толкал бы в него. Тормоз — только
        -- пока едем: на стоящей машине тормоз в MTA включает задний ход, и бот «катался
        -- туда-сюда» перед помехой (в заезде 28.09 — 208 сэмплов хода назад). Стоим на ручнике.
        if speed < 3 then out.handbrake = true else out.brake = 1 end
        self.gasOn = false
    elseif diff < -6 or (diff < -2.5 and goal < cruise - 8) then
        -- Переехали цель — гасим тормозом, одного отпускания газа не хватит. Перед поворотом,
        -- светофором или помехой (цель заметно ниже крейсера) — уже с 2.5 сверху: накатом
        -- грузовик почти не сбрасывает. На крейсере по-прежнему пульсация газом.
        out.brake = clamp(-diff / 12, 0.25, 1)
        self.gasOn = false
    else
        -- Газ держим пульсацией с гистерезисом: разогнался выше цели — отпустили,
        -- скатился ниже — снова нажали. Ровно так же водит человек.
        if speed >= goal + 2.5 then self.gasOn = false
        elseif speed <= goal - 2 then self.gasOn = true end
        if self.gasOn == nil then self.gasOn = diff > 0 end
        -- Нажатый газ дозируем по нехватке скорости: у самой цели это лёгкое
        -- поддержание, а не разгон, иначе пульсация сама себя перебивала бы.
        out.throttle = self.gasOn and clamp(diff / 10, 0.12, 1) or 0
    end
    out.detail.gas_on = self.gasOn == true

    -- Упёрлись в статичную помеху: газа нет, потому что мешает препятствие. Трафик уедет
    -- сам, бордюр или столб — нет, поэтому через 3 с отходим назад и пробуем снова.
    -- На красный не сдаём назад никогда: проскочил стоп-линию — просто стоим и ждём
    -- зелёного, откат назад тут только создаёт аварию на перекрёстке.
    -- Красный держит на месте, только если впереди очередь (машина) или мы у самой
    -- линии. От столба в 20 м до светофора отходим как обычно: у метки 7 бот 9 с стоял
    -- носом в столб и ждал зелёного.
    -- Вид помехи: столб или стена ("static"), пешеход ("ped"), машина в потоке ("traffic"),
    -- стоящая с водителем ("queue") или брошенная ("parked"). Без вида (старые вызовы)
    -- машина считается очередью.
    local kind = data.obstacleKind or (data.obstacleVehicle and "queue") or "static"
    local waitVehicle = kind == "queue" or kind == "traffic" or data.yield
    local redAhead = data.light ~= nil and not data.light.green
        and (waitVehicle or data.light.distance <= LIGHT_STOP + 12)
    if goal <= 0 and speed < 2 and not atLight and not redAhead then
        self.blockedSince = self.blockedSince or now
        -- Машина с водителем — пробка или очередь: она уедет, а откат назад в очереди
        -- только таранит того, кто сзади. Ждём, пока стоит с водителем; через 20 с без
        -- движения (5 с) бот сам переведёт её в брошенные. Столб, стена, брошенная машина —
        -- через 3 с решаем, как выйти; пешеход — через 4 с. Больше 5 с стоять — трата
        -- времени (пользователь: «стоять 20 секунд перебор»).
        local patience = not waitVehicle and (kind == "ped" and 4000 or 3000) or nil
        out.detail.blocked_by = data.yield and "vehicle" or kind
        -- Выходим, только если помеха всё ещё перед носом: стоп держится ещё 0.6 с после
        -- того, как она пропала, и без этой проверки бот сдавал назад от уехавшей машины.
        if patience and now - self.blockedSince >= patience and finite(data.obstacle) then
            return self:startEscape(out, data, err, now, "escape_blocked")
        end
    else
        self.blockedSince = nil
    end

    -- Застревание: газ есть, скорости нет. Сначала фиксируем, через 2.5 с уходим назад.
    -- Перед красным назад не сдаём ни при каких обстоятельствах: стоим и ждём.
    if out.throttle > 0.3 and speed < 3 and not redAhead and not atLight then
        self.stuckSince = self.stuckSince or now
        if now - self.stuckSince >= 2500 then
            return self:startEscape(out, data, err, now, "escape_start")
        end
    else
        self.stuckSince = nil
    end
    out.detail.stuck_ms = self.stuckSince and (now - self.stuckSince) or 0
    out.reason = out.brake > 0 and "brake" or "drive"
    return out
end
-- END EMBEDDED PLOW CONTROLLER

local native = {log = dfPlowLog, update = dfPlowUpdate, command = dfPlowTakeCommand,
    alert = dfPlayAlertSignal, key = dfEmulateKey, marks = dfPlowMarks}
for _, name in ipairs({"log", "update", "command"}) do
    assert(type(native[name]) == "function", "PlowBot: missing bridge " .. name)
end
if type(_G.__DarkFlamePlowCleanup) == "function" then _G.__DarkFlamePlowCleanup() end
local lease = native.update("attach", "")
assert(type(lease) == "string" and lease ~= "", "PlowBot: incompatible native bridge")
local bridge = {
    log = function(text, force) return native.log(text, force, lease) end,
    update = function(key, value) return native.update(key, tostring(value), lease) end,
    command = function() return native.command(lease) end,
}
-- Файл меток PlowMarks.txt ведёт DLL. Старая DLL этой функции не знает — тогда метки
-- берутся из кода, как раньше.
if type(native.marks) == "function" then
    bridge.marks = function(action, text) return native.marks(action, text or "", lease) end
end

local NULL = setmetatable({}, {__tostring = function() return "null" end})
local state = {
    bot = false, autonomy = false, recording = false, closed = false,
    status = "Ожидание", report = {}, failures = {}, hooks = {}, hookStatus = {},
    owned = false, trips = 0, frame = 0, buffer = {}, bufferBytes = 0,
    -- Ограничитель задаётся в единицах спидометра сервера, внутрь идёт пересчёт.
    speedLimitSpeedo = 50, speedLimit = 50, debug = false,
}
local controller = Controller.new()

local function elapsed(now, previous)
    return previous and (now - previous) % 4294967296 or math.huge
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

local function valid(element)
    return element ~= nil and read("isElement", element) == true
end

local function quote(value)
    return '"' .. tostring(value):gsub('[%z\1-\31\\"]', function(char)
        if char == '"' then return '\\"' end
        if char == '\\' then return '\\\\' end
        return string.format("\\u%04x", string.byte(char))
    end) .. '"'
end

local function json(value, depth)
    depth = depth or 0
    if value == NULL or value == nil then return "null" end
    if type(value) == "boolean" then return tostring(value) end
    if type(value) == "number" then
        return finite(value) and string.format("%.10g", value) or "null"
    end
    if type(value) == "string" then return quote(value) end
    if type(value) ~= "table" or depth > 8 then return quote(tostring(value)) end
    local out = {}
    if #value > 0 then
        for i = 1, #value do out[i] = json(value[i], depth + 1) end
        return "[" .. table.concat(out, ",") .. "]"
    end
    local keys = {}
    for key in pairs(value) do keys[#keys + 1] = tostring(key) end
    table.sort(keys)
    for _, key in ipairs(keys) do
        out[#out + 1] = quote(key) .. ":" .. json(value[key], depth + 1)
    end
    return "{" .. table.concat(out, ",") .. "}"
end

local function flush(force)
    if #state.buffer == 0 then return end
    local text = table.concat(state.buffer)
    state.buffer, state.bufferBytes = {}, 0
    bridge.log(text, force == true)
end

local function emit(kind, payload, force)
    if not state.recording and not force then return end
    local line = '{"type":' .. quote(kind) .. ',"tick":' .. getTickCount()
        .. ',"frame":' .. state.frame
        .. (payload ~= nil and (',"data":' .. json(payload)) or "") .. "}\n"
    state.buffer[#state.buffer + 1] = line
    state.bufferBytes = state.bufferBytes + #line
    if force or state.bufferBytes > 16384 then flush(force) end
end

-- Отчёт в панель: последние строки, сверху свежие.
local function note(text)
    local stamp = string.format("%02d:%02d", math.floor(getTickCount() / 60000) % 60,
        math.floor(getTickCount() / 1000) % 60)
    table.insert(state.report, 1, stamp .. "  " .. text)
    while #state.report > 12 do table.remove(state.report) end
    state.status = text
    -- Отчёт и статус уходят в панель сразу, не дожидаясь очередного обновления UI.
    bridge.update("status", text)
    bridge.update("report", table.concat(state.report, "\n"))
    emit("note", {text = text}, true)
end

-- === данные работы из ресурса ===

local function routeId()
    local id = _G.CURRENT_ROUTE_ID
    return type(id) == "number" and id or nil
end

local function routePoints()
    local id = routeId()
    if not id or type(_G.tRoutesPoints) ~= "table" then return nil end
    local points = _G.tRoutesPoints[id]
    return type(points) == "table" and points or nil
end

local function pointAt(index)
    local points = routePoints()
    if not points or type(points[index]) ~= "table" then return nil end
    local v = points[index][1]
    if type(v) ~= "userdata" and type(v) ~= "table" then return nil end
    local ok, x, y, z = pcall(function() return v.x, v.y, v.z end)
    if not ok or not finite(x) or not finite(y) then return nil end
    return {x = x, y = y, z = z}
end

-- Живая цель: ресурс держит колсферу активной контрольной точки и отдаёт её через
-- getCurrentColshape(). Это ровно та метка, что игрок видит как «Контрольная точка»,
-- поэтому она надёжнее индекса в таблице — учитывает и пропуски точек сервером.
local function liveTarget()
    local shape = read("getCurrentColshape")
    if not shape or read("isElement", shape) ~= true then return nil end
    local x, y, z = read("getElementPosition", shape)
    if not finite(x) or not finite(y) then return nil end
    return {x = x, y = y, z = finite(z) and z or 0}
end

local function pointIndex()
    local index = _G.CURRENT_POSITION_ID
    return type(index) == "number" and index > 0 and index or nil
end

local function jobVehicle()
    local vehicle = read("getJobVehicle")
    return valid(vehicle) and vehicle or nil
end

local function occupied()
    local vehicle = read("getPedOccupiedVehicle", localPlayer)
    return valid(vehicle) and vehicle or nil
end

local function sprayOn(vehicle)
    return read("isActiveWaterSpray", vehicle) and true or false
end

local function waterPercent()
    local value = read("getAmmountSpray")
    return finite(value) and value or nil
end

local function routeProgress()
    local value = read("getRouteCompleted")
    return finite(value) and value or nil
end

local function speedOf(vehicle)
    local value = read("getVehicleSpeed", vehicle)
    return finite(value) and value or 0
end

-- Выбор маршрута: оплата с коэффициентом, делённая на время по длине, и только если
-- полного бака (FULL_AMMOUNT_RESOURCE) хватает на маршрут.
-- Средняя скорость, ниже которой воды на маршрут не хватит: бак заливают ровно под
-- норму маршрута, а вода уходит по времени. Для всех девяти выходит 59–64 внутр.
local function requiredSpeed(id)
    local dist = type(_G.tDist) == "table" and _G.tDist[id]
    local water = type(_G.tWaterCapacity) == "table" and _G.tWaterCapacity[id]
    if not finite(dist) or not finite(water) or water <= 0 then return nil end
    -- Переводим в единицы спидометра: расход измерен во времени, путь — в метрах.
    return dist / (water / WATER_RATE) * 3.6 / REAL_KMH
end

local function routeScore(id)
    local dist = type(_G.tDist) == "table" and _G.tDist[id]
    local coef = type(_G.tRouteCoef) == "table" and _G.tRouteCoef[id] or 1
    local info = type(_G.tRoutes) == "table" and _G.tRoutes[id]
    local salary = type(info) == "table" and tonumber(info.salary) or nil
    if not finite(dist) or not finite(salary) then return nil end
    local need = requiredSpeed(id)
    -- Маршрут, который требует больше, чем машина реально держит, брать нельзя.
    if need and need > CRUISE + 4 then return nil, "воды не хватит по времени" end
    local seconds = dist / toMs(CRUISE)
    local score = salary * (finite(coef) and coef or 1) / seconds
    -- При равной выгоде предпочитаем маршрут с большим запасом воды.
    if need then score = score * (1 + (CRUISE - need) / 200) end
    return score
end

local function bestRoute()
    local best, bestId
    for id = 1, 9 do
        local score = routeScore(id)
        if score and (not best or score > best) then best, bestId = score, id end
    end
    return bestId, best
end

local function routeDescription(id)
    local info = type(_G.tRoutes) == "table" and _G.tRoutes[id]
    local dist = type(_G.tDist) == "table" and _G.tDist[id]
    local water = type(_G.tWaterCapacity) == "table" and _G.tWaterCapacity[id]
    local name = type(info) == "table" and tostring(info.name) or ("маршрут " .. id)
    local salary = type(info) == "table" and tonumber(info.salary) or nil
    local need = requiredSpeed(id)
    return string.format("%d. %s — %s м, вода %s, оплата %s, нужно держать %s", id, name,
        finite(dist) and math.floor(dist) or "?",
        finite(water) and math.floor(water) or "?",
        salary and math.floor(salary) or "?",
        need and string.format("%.0f по спидометру", need * SPEEDO) or "?")
end

-- === собственные путевые метки ===
-- Контрольные точки маршрута стоят как попало: между ними прямая может идти по
-- встречке или под забор. Поэтому водитель сам расставляет метки там, где надо
-- проехать, и бот ведёт машину через них, а контрольная точка ждёт своей очереди.
-- Числовые настройки меток, светофоров и памяти помех — в одной таблице: в главном блоке
-- Lua предел 200 локальных переменных, и в него упирались.
local K = {}
K.WAYPOINT_SEE = 70    -- дальше этой дистанции метка нас не касается
K.WAYPOINT_CONE = 50   -- и она должна быть впереди, а не сбоку
K.WAYPOINT_SAME = 9    -- ближе этого считаем, что правим ту же метку
-- У метки есть размер: это не точка, а зона, внутри которой можно ехать как удобно.
-- Маленькая метка ведёт машину строго через себя, большая лишь задаёт коридор.
K.WAYPOINT_MIN = 2
K.WAYPOINT_MAX = 50
K.WAYPOINT_SMALL = 5
K.WAYPOINT_MID = 14
K.WAYPOINT_BIG = 30
-- Размечено вручную на маршруте 1, 27.09.2026; копия PlowMarks.txt от 28.09.2026 (17 меток,
-- с новыми 4, 6 и 7 у столбов) — на случай, если файла нет. Порядок — порядок проезда.
-- Последняя метка считается финишной: в ней бот встаёт и даёт сигнал.
-- Поля: x, y, курс, радиус, маршрут (на другом маршруте метка не участвует).
local WAYPOINTS = {
    {2233.40, 2464.04, 46.2, 2, 1},
    {2207.61, 2447.29, 129.4, 4, 1},
    {2187.98, 2424.32, 136.4, 4, 1},
    {145.40, 2584.30, 89.3, 4, 1},
    {64.48, 2620.90, 90.4, 5, 1},
    {20.83, 2592.08, 158.1, 4, 1},
    {4.60, 2581.76, 95.1, 4, 1},
    {-24.51, 2583.22, 88.8, 4, 1},
    {-25, 2718.64, 356.5, 3, 1},
    {7.62, 2775.53, 310.8, 4, 1},
    {34.97, 2790.74, 288.0, 5, 1},
    {2514.51, 2637.17, 173.9, 5, 1},
    {2448.09, 2586.99, 116.2, 2, 1},
    {2324.09, 2566.38, 94.8, 2, 1},
    {2243.39, 2543.85, 113.4, 2, 1},
    {2195.39, 2509.72, 126.2, 4, 1},
    {2267.22, 2416.86, 210.3, 6, 1},
}

local function waypointNear(position, heading, radius)
    if not position then return nil end
    local best, bestDistance
    for index, point in ipairs(WAYPOINTS) do
        local distance = math.sqrt((point[1] - position.x) ^ 2 + (point[2] - position.y) ^ 2)
        local sameWay = not finite(heading) or not finite(point[3])
            or math.abs(angle(point[3] - heading)) <= 70
        if sameWay and distance <= radius and (not bestDistance or distance < bestDistance) then
            best, bestDistance = index, distance
        end
    end
    return best, bestDistance
end

-- Метки обязательные и идут по порядку: бот ведёт машину к текущей, и только
-- проехав её, берётся за следующую. Так размеченный маршрут исполняется целиком,
-- а не «какая ближе, та и цель».
K.WAYPOINT_REACH = 200   -- дальше этого метка нас пока не касается
K.WAYPOINT_BEHIND = 110  -- метка под таким углом считается оставленной позади

-- Садиться в машину можно в любом месте маршрута, поэтому проезд начинается
-- с БЛИЖАЙШЕЙ метки, а дальше идёт строго по порядку.
local function waypointStart(position)
    if #WAYPOINTS == 0 or not position then return 1 end
    local best, bestDistance = 1, nil
    for index, point in ipairs(WAYPOINTS) do
        local distance = math.sqrt((point[1] - position.x) ^ 2 + (point[2] - position.y) ^ 2)
        if not bestDistance or distance < bestDistance then
            best, bestDistance = index, distance
        end
    end
    return best
end

-- Метка под курсором — обязательный промежуточный пункт: бот ведёт машину ЧЕРЕЗ неё,
-- и уже от неё линия идёт к контрольной точке сервера. Контрольные точки проходятся
-- попутно, потому что метки и расставлены вдоль маршрута.
local function waypointNext(position, heading, targetDistance)
    if #WAYPOINTS == 0 or not position or not finite(heading) then return nil end
    local cursor = state.waypointCursor or 1
    for _ = 1, 4 do
        local point = WAYPOINTS[cursor]
        if not point then
            state.waypointCursor = cursor
            return nil
        end
        local dx, dy = point[1] - position.x, point[2] - position.y
        local distance = math.sqrt(dx * dx + dy * dy)
        local radius = point[4] or K.WAYPOINT_MID
        if distance <= radius and cursor == #WAYPOINTS and state.waypointSkip ~= cursor then
            -- Финишная метка: в неё надо въехать и ВСТАТЬ, а не просто пройти её.
            state.waypointCursor = cursor
            return point, distance, cursor
        elseif distance <= radius and state.waypointSkip == cursor then
            -- Метку только что поставили под машиной: пока не выедем, она не пройдена.
            state.waypointCursor = cursor
            return nil
        elseif distance <= radius then
            -- Въехали в зону — метка засчитана.
            state.waypointPassed = cursor
            cursor = cursor + 1
            state.waypointCursor = cursor
        elseif math.abs(angle(bearingTo(dx, dy) - heading)) > K.WAYPOINT_BEHIND
            and distance > radius * 2 then
            -- Метка осталась позади: разворачиваться за ней бессмысленно.
            state.waypointMissed = cursor
            cursor = cursor + 1
            state.waypointCursor = cursor
        elseif state.waypointSkip == cursor and distance > radius then
            state.waypointSkip = nil
            state.waypointCursor = cursor
            return point, distance, cursor
        elseif distance > K.WAYPOINT_REACH then
            state.waypointCursor = cursor
            return nil           -- ещё далеко, ведём по контрольным точкам
        else
            state.waypointCursor = cursor
            return point, distance, cursor
        end
    end
    return nil
end

-- Ближайшая метка впереди по курсу, до которой ещё не доехали.
local function waypointAhead(position, heading, targetDistance)
    if not position or not finite(heading) then return nil end
    local best, bestDistance, bestIndex
    for index, point in ipairs(WAYPOINTS) do
        local dx, dy = point[1] - position.x, point[2] - position.y
        local distance = math.sqrt(dx * dx + dy * dy)
        local radius = point[4] or K.WAYPOINT_MID
        -- Внутри зоны метка считается пройденной: держать её незачем.
        if distance > radius * 0.8 and distance <= K.WAYPOINT_SEE then
            local err = math.abs(angle(bearingTo(dx, dy) - heading))
            local sameWay = not finite(point[3])
                or math.abs(angle(point[3] - heading)) <= 70
            -- Метку берём всегда, когда она впереди по курсу — даже если контрольная
            -- точка ближе. Смысл метки именно в том, чтобы проехать ДАЛЬШЕ точки и
            -- повернуть там, где надо: прежнее условие «только ближе точки» молча
            -- отбрасывало все метки, поставленные за перекрёстком.
            if sameWay and err <= K.WAYPOINT_CONE
                and (not bestDistance or distance < bestDistance) then
                best, bestDistance, bestIndex = point, distance, index
            end
        end
    end
    if not best then return nil end
    return best, bestDistance, bestIndex
end

-- Метка — это зона, а не точка. Целимся в то место зоны, которое ближе всего к нашей
-- естественной линии к следующей цели: большая метка почти не мешает и лишь задаёт
-- коридор, маленькая ведёт машину строго через себя.
local function waypointAim(point, position, heading)
    local cx, cy = point[1], point[2]
    local radius = point[4] or K.WAYPOINT_MID
    if not finite(heading) then return {x = cx, y = cy, z = position.z} end
    -- Луч нашего текущего курса: если он и так проходит сквозь зону, руль не трогаем.
    local rad = math.rad(heading)
    local fx, fy = -math.sin(rad), math.cos(rad)
    local along = (cx - position.x) * fx + (cy - position.y) * fy
    if along <= 0 then return {x = cx, y = cy, z = position.z} end
    local px, py = position.x + fx * along, position.y + fy * along
    local away = math.sqrt((px - cx) ^ 2 + (py - cy) ^ 2)
    if away <= radius then
        return {x = px, y = py, z = position.z}
    end
    -- Целимся на полпути от края к центру, а не в самую границу: у края машина касалась
    -- зоны ровно на радиусе, а метку ставят в стороне от столба — край зоны ближе к нему.
    local k = radius * 0.5 / away
    return {x = cx + (px - cx) * k, y = cy + (py - cy) * k, z = position.z}
end

-- === файл меток ===
-- Метки живут в PlowMarks.txt рядом с ботом, отдельно от кода: эдитор сохраняет туда
-- каждую правку, а новая версия PlowBot.lua их не затирает. Таблица выше — только
-- начальная разметка на случай, если файла ещё нет.
local function marksSerialize()
    local lines = {
        "-- PlowBot: метки проезда. Файл пишет эдитор меток; порядок строк — порядок проезда.",
        "-- Поля: x, y, курс, радиус зоны, маршрут (nil — на любом маршруте).",
    }
    for _, point in ipairs(WAYPOINTS) do
        lines[#lines + 1] = string.format("{%.2f, %.2f, %.1f, %.2f, %s},",
            point[1], point[2], point[3] or 0, point[4] or K.WAYPOINT_MID,
            point[5] and tostring(point[5]) or "nil")
    end
    return table.concat(lines, "\n") .. "\n"
end

local function marksParse(text)
    local list, bad = {}, 0
    for line in (text .. "\n"):gmatch("([^\n]*)\n") do
        local body = line:gsub("%-%-.*$", "")
        if body:find("%S") then
            local x, y, h, r, route = body:match("^%s*{%s*([-%d%.]+)%s*,%s*([-%d%.]+)%s*,"
                .. "%s*([-%d%.]+)%s*,%s*([-%d%.]+)%s*,%s*(%w+)%s*}%s*,?%s*$")
            x, y, h, r = tonumber(x), tonumber(y), tonumber(h), tonumber(r)
            if finite(x) and finite(y) and finite(h) and finite(r) then
                list[#list + 1] = {x, y, h, clamp(r, K.WAYPOINT_MIN, K.WAYPOINT_MAX), tonumber(route)}
            else
                bad = bad + 1
            end
        end
    end
    return list, bad
end

local function marksSave(reason)
    if not bridge.marks then
        state.marksFile = "DLL без файла меток — правки живут до перезахода"
        return false
    end
    local ok, err = bridge.marks("save", marksSerialize())
    if ok then
        state.marksFile = string.format("Сохранено в PlowMarks.txt: %d меток (%s)", #WAYPOINTS,
            reason or "правка")
    else
        state.marksFile = "PlowMarks.txt не записан: " .. tostring(err)
        note(state.marksFile)
    end
    emit("marks_saved", {ok = ok == true, count = #WAYPOINTS, reason = reason,
        error = ok and nil or tostring(err)}, true)
    return ok == true
end

-- При запуске: метки из файла, если он есть и читается целиком. Файл с ошибкой не
-- принимаем — лучше ездить по разметке из кода, чем по половине меток.
local function marksLoad()
    if not bridge.marks then
        state.marksFile = "DLL без файла меток — метки из кода"
        return
    end
    local text, err = bridge.marks("load")
    if type(text) ~= "string" then
        state.marksFile = err == "missing"
            and "Файла меток нет — метки из кода; первая правка создаст PlowMarks.txt"
            or ("PlowMarks.txt не прочитан: " .. tostring(err) .. " — метки из кода")
        return
    end
    local list, bad = marksParse(text)
    if bad > 0 then
        state.marksFile = string.format("PlowMarks.txt не принят: строк с ошибкой %d — метки из кода", bad)
        emit("marks_rejected", {bad = bad, good = #list}, true)
        return
    end
    WAYPOINTS = list
    state.planDirty = true
    state.marksFile = string.format("Метки из PlowMarks.txt: %d", #list)
end

local function waypointDump()
    local parts = {}
    for _, point in ipairs(WAYPOINTS) do
        parts[#parts + 1] = string.format("    {%.2f, %.2f, %.1f, %g, %s},",
            point[1], point[2], point[3] or 0, point[4] or K.WAYPOINT_MID,
            point[5] and tostring(point[5]) or "nil")
    end
    local text = "local WAYPOINTS = {\n" .. table.concat(parts, "\n") .. "\n}"
    emit("waypoint_table", {count = #WAYPOINTS, lua = text}, true)
    return text
end

-- Куда вставить новую метку, чтобы порядок строк оставался порядком проезда. Определяется
-- ниже, рядом с цепочкой: ей нужны точки маршрута. Раньше новая метка дописывалась в конец
-- списка — и становилась финишной: две метки, поставленные после метки 3, оказались 15-й и
-- 16-й, их пришлось переставлять руками.
local placeInOrder

local function waypointSave(position, heading, radius)
    if not position then
        note("Метка: не вижу машину")
        return
    end
    radius = clamp(finite(radius) and radius or K.WAYPOINT_MID, K.WAYPOINT_MIN, K.WAYPOINT_MAX)
    local index = waypointNear(position, heading, K.WAYPOINT_SAME)
    if index then
        WAYPOINTS[index] = {position.x, position.y, finite(heading) and heading or 0, radius,
            routeId()}
    else
        local point = {position.x, position.y, finite(heading) and heading or 0, radius, routeId()}
        index = placeInOrder and placeInOrder(point) or (#WAYPOINTS + 1)
        table.insert(WAYPOINTS, index, point)
        -- Номера после вставки сдвинулись.
        if state.editSel and state.editSel >= index then state.editSel = state.editSel + 1 end
        if state.waypointCursor and state.waypointCursor >= index then
            state.waypointCursor = state.waypointCursor + 1
        end
        state.editUndo = {}
    end
    -- Последняя тронутая метка — её и будет менять ползунок размера.
    state.lastWaypoint = index
    -- Метку ставят там, где машина стоит прямо сейчас, поэтому засчитывать её
    -- немедленно нельзя: иначе она "пройдена" в момент создания.
    state.waypointSkip = index
    if not state.waypointCursor or state.waypointCursor > #WAYPOINTS then
        state.waypointCursor = index
    end
    note(string.format("Метка #%d, радиус %d м (всего %d)%s", index, math.floor(radius),
        #WAYPOINTS, index < #WAYPOINTS and " — встала по ходу маршрута" or ""))
    state.planDirty = true
    emit("waypoint_saved", {x = position.x, y = position.y, heading = heading,
        radius = radius, index = index, total = #WAYPOINTS}, true)
    waypointDump()
    marksSave("новая метка #" .. index)
end

-- Размер меняется на лету: выбрал 25, передумал — поставил 50, метка выросла.
local function waypointResize(radius)
    if not finite(radius) then return end
    local index = state.lastWaypoint
    if not index or not WAYPOINTS[index] then index = #WAYPOINTS end
    if not WAYPOINTS[index] then
        note("Метка: менять нечего, сначала создай")
        return
    end
    WAYPOINTS[index][4] = clamp(radius, K.WAYPOINT_MIN, K.WAYPOINT_MAX)
    state.planDirty = true
    state.lastWaypoint = index
    note(string.format("Метка #%d: радиус %d м", index, math.floor(WAYPOINTS[index][4])))
    emit("waypoint_resized", {index = index, radius = WAYPOINTS[index][4]}, true)
    marksSave("зона #" .. index)
end

local function waypointForget(position, heading)
    local index = waypointNear(position, heading, K.WAYPOINT_SEE)
    if not index then
        note("Метка: рядом ничего не сохранено")
        return
    end
    table.remove(WAYPOINTS, index)
    state.planDirty = true
    note(string.format("Метка #%d удалена, осталось %d", index, #WAYPOINTS))
    waypointDump()
    -- Номера сдвинулись: отмены по старым номерам больше не верны.
    state.editUndo = {}
    if state.editSel and state.editSel > #WAYPOINTS then state.editSel = #WAYPOINTS end
    marksSave("удалена #" .. index)
end

-- === эдитор меток ===
-- Выбираешь метку в панели и двигаешь её кнопками, в мире она перерисовывается сразу.
-- Вперёд/назад/влево/вправо — как смотрит камера: что видишь на экране слева, туда и
-- уедет метка. Вверх/вниз не нужно — метка плоская, высоты у неё нет.
local EDIT_UNDO_MAX = 200

local function cameraAxes(fallbackHeading)
    local cx, cy, _, lx, ly = read("getCameraMatrix")
    local fx, fy
    if finite(cx) and finite(cy) and finite(lx) and finite(ly) then fx, fy = lx - cx, ly - cy end
    if not fx or fx * fx + fy * fy < 1e-6 then
        local r = math.rad(finite(fallbackHeading) and fallbackHeading or 0)
        fx, fy = -math.sin(r), math.cos(r)
    end
    local length = math.sqrt(fx * fx + fy * fy)
    fx, fy = fx / length, fy / length
    return fx, fy, fy, -fx   -- вперёд и вправо
end

-- Где стоит тот, кто правит метки: машина, а если вышел из неё — сам игрок. Раньше без
-- машины эдитор не видел ни меток, ни ближайшей.
local function carPosition()
    local vehicle = jobVehicle() or occupied() or localPlayer
    if not vehicle then return nil end
    local x, y, z = read("getElementPosition", vehicle)
    local _, _, rz = read("getElementRotation", vehicle)
    if not finite(x) or not finite(y) then return nil end
    return {x = x, y = y, z = finite(z) and z or 0}, finite(rz) and rz or nil
end

local function editorSelect(index)
    if not index or not WAYPOINTS[index] then
        note("Эдитор: такой метки нет")
        return
    end
    state.editSel = index
    -- Старый ползунок размера тоже правит выбранную метку.
    state.lastWaypoint = index
end

local function editorNearest()
    local position = carPosition()
    if not position then return nil end
    local best, bestDistance
    for index, point in ipairs(WAYPOINTS) do
        local d = math.sqrt((point[1] - position.x) ^ 2 + (point[2] - position.y) ^ 2)
        if not bestDistance or d < bestDistance then best, bestDistance = index, d end
    end
    return best
end

local function editorRemember(index)
    local point = WAYPOINTS[index]
    state.editUndo = state.editUndo or {}
    table.insert(state.editUndo, {index = index, count = #WAYPOINTS,
        point = {point[1], point[2], point[3], point[4], point[5]}})
    if #state.editUndo > EDIT_UNDO_MAX then table.remove(state.editUndo, 1) end
end

local function editorNudge(direction, step)
    local index = state.editSel
    local point = index and WAYPOINTS[index]
    step = tonumber(step)
    if not point or not finite(step) then return end
    step = clamp(step, 0.05, 10)
    local _, heading = carPosition()
    local fx, fy, rx, ry = cameraAxes(heading or point[3])
    local dx, dy
    if direction == "fwd" then dx, dy = fx, fy
    elseif direction == "back" then dx, dy = -fx, -fy
    elseif direction == "right" then dx, dy = rx, ry
    elseif direction == "left" then dx, dy = -rx, -ry
    else return end
    editorRemember(index)
    point[1], point[2] = point[1] + dx * step, point[2] + dy * step
    state.planDirty = true
    marksSave(string.format("#%d сдвинута", index))
end

local function editorRadius(delta)
    local index = state.editSel
    local point = index and WAYPOINTS[index]
    delta = tonumber(delta)
    if not point or not finite(delta) then return end
    local radius = clamp((point[4] or K.WAYPOINT_MID) + delta, K.WAYPOINT_MIN, K.WAYPOINT_MAX)
    if radius == point[4] then return end
    editorRemember(index)
    point[4] = radius
    state.planDirty = true
    marksSave(string.format("#%d зона %g м", index, radius))
end

-- Переставить выбранную метку на одну раньше или позже по ходу (среди меток того же
-- маршрута). Отмены по старым номерам после этого не годятся.
local function editorOrder(delta)
    local index = state.editSel
    local point = index and WAYPOINTS[index]
    if not point or (delta ~= 1 and delta ~= -1) then return end
    local j = index + delta
    while WAYPOINTS[j] and WAYPOINTS[j][5] ~= point[5] do j = j + delta end
    if not WAYPOINTS[j] then
        note(delta < 0 and "Эдитор: метка и так первая" or "Эдитор: метка и так последняя")
        return
    end
    WAYPOINTS[index], WAYPOINTS[j] = WAYPOINTS[j], WAYPOINTS[index]
    state.editSel, state.lastWaypoint = j, j
    state.editUndo = {}
    state.planDirty = true
    note(string.format("Эдитор: метка #%d стала #%d", index, j))
    marksSave(string.format("#%d → #%d", index, j))
end

local function editorUndo()
    local undo = state.editUndo or {}
    while #undo > 0 do
        local last = table.remove(undo)
        -- Метки добавляли или удаляли — номер уже про другую метку, такую отмену пропускаем.
        if last.count == #WAYPOINTS and WAYPOINTS[last.index] then
            local point = WAYPOINTS[last.index]
            for k = 1, 5 do point[k] = last.point[k] end
            state.editSel, state.lastWaypoint = last.index, last.index
            state.planDirty = true
            marksSave(string.format("#%d отмена", last.index))
            return
        end
    end
    note("Эдитор: отменять нечего")
end

-- === единая цепочка проезда: серверные точки + свои метки в одном порядке ===
-- Метки ставились по ходу маршрута, значит у каждой есть своё место между двумя
-- серверными точками. Сливаем обе последовательности в одну: S23 → метка → S24 → …
-- и едем строго по ней. Серверную точку так не пропустить, свою метку — тоже, а
-- «следующая цель» для торможения — просто следующее звено, какого бы типа оно ни было.
-- До 2.6.0 было правило «кто ближе, тот и цель»: у метки рядом с серверной точкой та
-- почти всегда чуть ближе, и метка молча выпадала (7 из 14 промахов в прогоне).
local plan = {route = nil, items = {}, serverCount = 0}
-- Путь по меткам: гладкая линия через центры меток, построенная заранее (раздел ниже, у лучей).
local Path = {}
local PLAN_MID = 80     -- метка дальше этого от своего отрезка — она не с этого маршрута
local PLAN_EDGE = 150   -- до старта и после финиша допускаем дальше: выезд из депо, сдача
local PLAN_BEHIND = 110 -- метка под таким углом к курсу осталась позади
local PLAN_DESTINATION = 50 -- дальше этого от линии маршрута последняя метка — место сдачи
-- «Задеть» метку — это коснуться её зоны кузовом, а не центром машины.
local BODY_HALF_WIDTH = 1.3
-- Свои метки проходим через центр (заезд 28.09, 2.9.0): бот засчитывал метку, едва зона
-- касалась кузова, — за 4–5 м до центра, — и сразу уходил к следующей. На метках 4, 8, 9,
-- 10 он проходил в 3–4 м от центра внутрь поворота: у метки 4 — в столб, у 9 и 10 — на
-- бордюр. Водитель ставит метку там, где должна пройти середина машины.
local PASS = {
    markAhead = 1.0,       -- метка взята, когда её центр не дальше 1 м впереди центра машины
    markThrough = 0.75,    -- заход в поворот у метки уводит путь от центра не дальше этого
    -- Точку сервера проходим по линии водителя, даже когда сервер её уже засчитал (он берёт
    -- её за 7 м): между метками 9 и 10 бот сразу рулил на метку 10, прямая к ней на изгибе шла
    -- по правому бордюру — прошёл точку 28 в 3.9 м правее центра, водитель — в 2.4 м (заезд
    -- 2.9.2). Только на пологом изгибе: на углу срезать нужно, иначе не вписаться.
    serverThrough = 1.0,   -- допуск у точки сервера
    ghostTurn = 45,        -- круче этого — угол, там по-старому
    ghostAhead = 1.5,      -- точка впереди центра машины хотя бы на столько
}

local function segmentDistance(px, py, ax, ay, bx, by)
    local dx, dy = bx - ax, by - ay
    local length = dx * dx + dy * dy
    local t = length > 0 and clamp(((px - ax) * dx + (py - ay) * dy) / length, 0, 1) or 0
    return math.sqrt((px - ax - dx * t) ^ 2 + (py - ay - dy * t) ^ 2)
end

-- Привязка меток к местам маршрута. Маршрут — петля: начинается и кончается у базы,
-- поэтому «ближайшая серверная точка» врёт (метка выезда из депо ближе всего к ПОСЛЕДНЕЙ
-- точке). Решаем динамическим программированием: метки идут в порядке записи, их места
-- на маршруте не убывают, суммарное расстояние до маршрута минимально.
-- Место 0 — до первой точки, место n — после последней, место s — отрезок S(s)→S(s+1).
-- Определены ниже, в разделе базы; объявляем заранее, чтобы цепочка их видела.
local locationOfRoute, homePoint

-- Направляющие: точки пути водителя из эталонного заезда 28.09 (ride2) там, где он отходит
-- от прямой между точками сервера больше чем на 1 м. Прямая режет изгибы дороги по
-- внутренней стороне: у S46–S47 бот шёл на 3–4 м правее водителя и тёрся о забор. Это не
-- метки: их не засчитывают, через них только рулят. На отрезках со своими метками их нет —
-- там путь задал водитель. Поля: отрезок (S_i → S_i+1), x, y.
local GUIDES = {
    [1] = {
        {1, 2150.20, 2456.24}, {1, 2134.70, 2468.78}, {1, 2118.97, 2480.42},
        {2, 2082.75, 2502.97}, {2, 2065.60, 2513.08}, {2, 2048.33, 2522.03},
        {2, 2029.30, 2530.86}, {3, 1987.72, 2547.96}, {3, 1968.60, 2554.87},
        {3, 1949.77, 2560.90}, {3, 1929.68, 2566.32}, {4, 1882.20, 2575.95},
        {4, 1862.52, 2578.86}, {4, 1841.64, 2581.27}, {4, 1821.92, 2582.62},
        {22, 138.98, 2596.36}, {23, 109.97, 2622.21}, {23, 90.15, 2622.50},
        {23, 70.17, 2622.67}, {24, 32.46, 2607.03}, {25, 3.09, 2582.47},
        {26, -27.56, 2598.93}, {26, -25.64, 2619.37}, {27, -24.51, 2658.34},
        {27, -24.77, 2678.79}, {27, -24.58, 2698.97}, {27, -22.91, 2719.13},
        {28, -1.17, 2762.74}, {28, 13.90, 2776.46}, {28, 31.63, 2785.87},
        {28, 51.38, 2791.47}, {45, 2094.49, 2792.12}, {45, 2115.22, 2792.14},
        {45, 2134.75, 2792.15}, {45, 2154.62, 2792.17}, {45, 2175.24, 2791.54},
        {45, 2194.61, 2790.08}, {46, 2233.38, 2785.06}, {46, 2252.68, 2781.52},
        {46, 2271.77, 2776.74}, {46, 2291.43, 2771.34}, {46, 2310.70, 2765.56},
        {46, 2329.95, 2758.63}, {47, 2365.98, 2743.18}, {47, 2384.33, 2734.38},
        {47, 2402.06, 2724.78}, {47, 2418.70, 2714.94}, {47, 2435.65, 2704.05},
        {49, 2504.99, 2625.74}, {49, 2488.61, 2612.71}, {50, 2454.69, 2592.79},
        {50, 2436.91, 2584.74}, {50, 2417.53, 2578.69}, {50, 2398.24, 2573.49},
        {51, 2364.59, 2569.94}, {51, 2343.59, 2568.46}, {51, 2324.50, 2566.46},
        {51, 2304.70, 2563.54}, {52, 2261.46, 2552.07}, {52, 2243.61, 2544.85},
        {52, 2226.11, 2536.10}, {53, 2200.18, 2505.06},
    },
}

-- Линия водителя целиком: эталонный заезд 28.09 (ride2, правая полоса), упрощённая до 0.35 м
-- (Дуглас–Пекер) и не реже чем через 40 м. По ней строится путь бота между метками — это его
-- знание дороги: где изгиб, где полоса. Поля: отрезок маршрута, x, y.
Path.LINE = {
    [1] = {
        {0, 2237.63, 2451.34}, {0, 2237.93, 2455.28}, {0, 2236.12, 2458.60}, {0, 2234.05, 2460.04},
        {0, 2232.10, 2460.60}, {0, 2228.41, 2460.07}, {0, 2217.59, 2451.60}, {0, 2195.14, 2432.36},
        {0, 2188.47, 2429.45}, {0, 2185.25, 2429.30}, {0, 2182.44, 2429.84}, {0, 2177.27, 2432.68},
        {1, 2166.56, 2442.62}, {1, 2141.97, 2463.11}, {1, 2126.25, 2475.26}, {2, 2100.89, 2492.16},
        {2, 2070.15, 2510.52}, {2, 2053.13, 2519.73}, {2, 2032.95, 2529.28}, {3, 2005.25, 2541.24},
        {3, 1988.92, 2547.54}, {3, 1959.95, 2557.84}, {3, 1931.02, 2566.02}, {4, 1905.64, 2571.72},
        {4, 1872.30, 2577.57}, {4, 1855.68, 2579.75}, {4, 1829.16, 2582.30}, {5, 1794.47, 2583.36},
        {5, 1777.50, 2583.29}, {5, 1747.26, 2582.67}, {5, 1720.06, 2582.73}, {5, 1704.53, 2582.72},
        {5, 1693.05, 2582.77}, {6, 1683.41, 2582.74}, {6, 1666.49, 2582.83}, {6, 1646.74, 2582.84},
        {6, 1636.77, 2582.90}, {6, 1621.71, 2582.92}, {6, 1614.26, 2582.96}, {6, 1598.45, 2582.99},
        {6, 1584.89, 2583.04}, {6, 1570.09, 2583.07}, {7, 1538.29, 2583.16}, {7, 1534.49, 2583.18},
        {7, 1524.29, 2583.20}, {7, 1484.46, 2583.32}, {7, 1477.32, 2583.35}, {7, 1453.99, 2583.21},
        {7, 1437.07, 2583.38}, {8, 1405.56, 2583.64}, {8, 1380.88, 2583.52}, {8, 1365.07, 2583.11},
        {8, 1353.15, 2583.33}, {8, 1347.36, 2583.38}, {9, 1313.59, 2583.83}, {9, 1306.55, 2583.95},
        {9, 1289.74, 2583.61}, {9, 1285.87, 2583.68}, {9, 1275.21, 2583.76}, {10, 1245.04, 2584.03},
        {10, 1223.39, 2583.62}, {10, 1213.11, 2583.67}, {10, 1201.95, 2583.68}, {10, 1192.04, 2583.71},
        {10, 1187.01, 2583.71}, {10, 1181.73, 2583.73}, {10, 1176.80, 2583.73}, {10, 1156.44, 2583.78},
        {11, 1131.75, 2583.82}, {11, 1124.03, 2583.58}, {11, 1109.55, 2583.60}, {11, 1103.68, 2583.57},
        {12, 1070.83, 2583.54}, {12, 1039.60, 2583.49}, {12, 1034.87, 2583.45}, {12, 1021.19, 2583.45},
        {12, 1011.16, 2583.42}, {13, 993.87, 2583.41}, {13, 982.35, 2583.38}, {13, 945.27, 2583.32},
        {13, 933.67, 2583.34}, {14, 922.60, 2583.30}, {14, 913.88, 2583.30}, {14, 908.61, 2583.28},
        {14, 894.42, 2583.27}, {14, 857.91, 2583.20}, {14, 850.76, 2583.20}, {14, 830.77, 2583.16},
        {15, 805.09, 2583.13}, {15, 804.07, 2583.12}, {15, 776.62, 2583.09}, {15, 767.85, 2583.04},
        {15, 756.85, 2583.07}, {16, 755.00, 2583.05}, {16, 747.46, 2583.06}, {16, 730.86, 2583.04},
        {16, 703.56, 2583.04}, {16, 699.84, 2583.02}, {16, 678.59, 2583.03}, {16, 654.02, 2583.00},
        {17, 636.59, 2583.01}, {17, 631.80, 2582.99}, {17, 615.34, 2583.00}, {17, 608.99, 2582.98},
        {17, 588.62, 2582.98}, {17, 587.42, 2582.97}, {18, 564.72, 2582.97}, {18, 545.62, 2582.96},
        {18, 535.49, 2582.94}, {18, 521.92, 2583.27}, {18, 505.76, 2583.24}, {18, 487.77, 2583.27},
        {19, 449.64, 2583.29}, {19, 423.33, 2583.34}, {20, 386.01, 2583.38}, {20, 376.65, 2583.38},
        {20, 375.75, 2583.39}, {20, 366.23, 2583.39}, {20, 365.01, 2583.40}, {20, 343.22, 2583.41},
        {20, 331.84, 2583.43}, {20, 321.59, 2583.43}, {20, 320.51, 2583.44}, {20, 313.17, 2583.44},
        {20, 304.26, 2583.46}, {21, 296.99, 2583.46}, {21, 295.54, 2583.47}, {21, 256.39, 2583.51},
        {21, 239.93, 2583.54}, {21, 232.35, 2583.54}, {21, 199.10, 2583.59}, {21, 183.53, 2583.60},
        {21, 182.13, 2583.61}, {22, 157.34, 2583.43}, {22, 152.79, 2583.90}, {22, 148.11, 2585.36},
        {22, 144.08, 2587.93}, {22, 141.03, 2591.41}, {22, 139.47, 2594.63}, {22, 138.69, 2598.16},
        {22, 139.43, 2609.48}, {22, 139.00, 2613.42}, {22, 137.33, 2617.61}, {23, 134.25, 2620.15},
        {23, 129.20, 2621.22}, {23, 114.41, 2622.12}, {23, 76.00, 2622.69}, {23, 53.69, 2622.42},
        {24, 48.90, 2621.34}, {24, 44.50, 2619.25}, {24, 38.80, 2615.14}, {24, 35.32, 2611.49},
        {24, 26.80, 2596.28}, {25, 23.37, 2591.62}, {25, 18.39, 2587.27}, {25, 13.30, 2584.73},
        {25, 4.26, 2582.60}, {26, -12.52, 2581.78}, {26, -15.56, 2582.08}, {26, -19.12, 2583.21},
        {26, -23.95, 2586.70}, {26, -26.88, 2591.79}, {26, -27.59, 2597.49}, {26, -25.64, 2619.37},
        {27, -24.89, 2634.76}, {27, -24.43, 2653.16}, {27, -24.81, 2682.14}, {27, -24.76, 2693.96},
        {27, -23.32, 2716.54}, {27, -21.92, 2724.44}, {27, -19.30, 2732.82}, {28, -14.03, 2744.73},
        {28, -8.22, 2753.84}, {28, -1.17, 2762.74}, {28, 9.34, 2772.96}, {28, 18.53, 2779.61},
        {28, 25.32, 2783.18}, {28, 39.54, 2788.93}, {28, 51.38, 2791.47}, {29, 74.79, 2792.66},
        {29, 94.48, 2792.42}, {29, 106.65, 2792.33}, {29, 143.07, 2791.96}, {29, 147.07, 2791.94},
        {30, 162.77, 2792.07}, {30, 173.34, 2792.12}, {30, 208.38, 2792.33}, {30, 230.36, 2792.48},
        {30, 270.10, 2792.72}, {30, 300.44, 2792.71}, {31, 330.32, 2792.51}, {31, 343.58, 2792.41},
        {31, 344.93, 2792.41}, {31, 362.69, 2792.28}, {31, 369.89, 2792.24}, {31, 383.16, 2792.14},
        {31, 392.68, 2792.22}, {31, 412.09, 2791.86}, {31, 418.23, 2791.94}, {32, 421.90, 2791.95},
        {32, 426.59, 2791.99}, {32, 450.65, 2792.12}, {32, 464.18, 2792.21}, {32, 470.84, 2792.24},
        {32, 476.31, 2792.28}, {32, 513.44, 2792.49}, {32, 518.72, 2792.53}, {32, 545.78, 2792.68},
        {33, 581.92, 2792.90}, {33, 615.19, 2792.04}, {33, 642.18, 2791.95}, {33, 656.77, 2791.75},
        {33, 669.29, 2791.79}, {34, 686.10, 2791.79}, {34, 697.95, 2791.81}, {34, 736.54, 2791.84},
        {34, 748.63, 2791.84}, {34, 762.80, 2791.86}, {34, 774.94, 2791.86}, {34, 775.99, 2791.87},
        {34, 788.86, 2791.87}, {34, 789.88, 2791.88}, {35, 801.96, 2791.88}, {35, 815.69, 2791.90},
        {35, 828.94, 2791.90}, {35, 843.11, 2791.92}, {35, 855.97, 2791.92}, {35, 857.01, 2791.93},
        {35, 868.71, 2791.93}, {35, 869.68, 2791.94}, {35, 905.97, 2791.97}, {35, 917.69, 2791.97},
        {35, 918.65, 2791.98}, {35, 931.13, 2791.98}, {35, 932.40, 2791.99}, {36, 948.42, 2791.99},
        {36, 949.64, 2792.00}, {36, 950.80, 2792.00}, {36, 952.00, 2792.00}, {36, 953.15, 2792.00},
        {36, 954.30, 2792.00}, {36, 955.51, 2792.00}, {36, 956.64, 2792.00}, {36, 957.80, 2792.00},
        {36, 958.94, 2792.00}, {36, 960.11, 2792.00}, {36, 961.27, 2792.00}, {36, 962.41, 2792.00},
        {36, 963.62, 2792.00}, {36, 964.79, 2792.00}, {36, 965.99, 2792.00}, {36, 967.10, 2792.00},
        {36, 968.25, 2792.00}, {36, 969.39, 2792.00}, {36, 970.49, 2792.00}, {36, 971.64, 2792.00},
        {36, 972.81, 2792.00}, {36, 973.96, 2792.00}, {36, 975.07, 2792.00}, {36, 976.29, 2792.00},
        {36, 977.52, 2792.00}, {36, 978.64, 2792.00}, {36, 979.77, 2792.00}, {36, 980.84, 2792.00},
        {36, 981.99, 2792.00}, {36, 983.09, 2792.00}, {36, 984.21, 2792.00}, {36, 985.31, 2792.00},
        {36, 986.49, 2792.00}, {36, 987.62, 2792.00}, {36, 988.80, 2792.00}, {36, 989.93, 2792.00},
        {36, 991.00, 2792.00}, {36, 992.18, 2792.00}, {36, 993.29, 2792.00}, {36, 994.42, 2792.00},
        {36, 995.53, 2792.00}, {36, 996.70, 2792.00}, {36, 997.91, 2792.00}, {36, 999.07, 2792.00},
        {36, 1000.24, 2792.00}, {36, 1001.42, 2792.00}, {36, 1002.65, 2792.00}, {36, 1003.79, 2792.00},
        {36, 1004.98, 2792.00}, {36, 1006.13, 2792.00}, {36, 1007.23, 2792.00}, {36, 1008.38, 2792.00},
        {36, 1009.53, 2792.00}, {36, 1010.78, 2792.00}, {36, 1012.00, 2792.00}, {36, 1013.20, 2792.00},
        {36, 1014.38, 2792.00}, {36, 1015.49, 2792.00}, {36, 1016.67, 2792.00}, {36, 1017.81, 2792.00},
        {36, 1019.02, 2792.00}, {36, 1020.11, 2792.00}, {36, 1021.34, 2792.00}, {36, 1022.59, 2792.00},
        {36, 1023.79, 2792.00}, {36, 1025.02, 2792.00}, {36, 1026.11, 2792.00}, {36, 1027.26, 2792.00},
        {36, 1028.37, 2792.00}, {36, 1029.53, 2792.00}, {36, 1030.68, 2792.00}, {36, 1031.99, 2792.00},
        {36, 1033.17, 2792.00}, {36, 1034.40, 2792.00}, {36, 1035.63, 2792.00}, {36, 1036.85, 2792.00},
        {36, 1037.96, 2792.00}, {36, 1039.13, 2792.00}, {36, 1040.26, 2792.00}, {36, 1041.51, 2792.00},
        {36, 1042.64, 2792.00}, {36, 1043.84, 2792.00}, {36, 1045.03, 2792.00}, {36, 1046.17, 2792.00},
        {36, 1047.39, 2792.00}, {36, 1048.52, 2792.00}, {36, 1049.75, 2792.00}, {36, 1050.71, 2792.00},
        {36, 1051.95, 2792.00}, {36, 1053.10, 2792.00}, {36, 1054.27, 2792.00}, {36, 1055.49, 2792.00},
        {36, 1056.68, 2792.00}, {36, 1057.92, 2792.00}, {36, 1059.14, 2792.00}, {36, 1060.37, 2792.00},
        {36, 1061.59, 2792.00}, {36, 1062.59, 2792.00}, {36, 1063.81, 2792.00}, {36, 1065.05, 2792.00},
        {36, 1066.28, 2792.00}, {36, 1067.39, 2792.00}, {36, 1068.61, 2792.00}, {36, 1069.54, 2792.00},
        {37, 1070.52, 2792.00}, {37, 1071.72, 2792.00}, {37, 1072.92, 2792.00}, {37, 1074.11, 2792.00},
        {37, 1075.33, 2792.00}, {37, 1076.55, 2792.00}, {37, 1077.77, 2792.00}, {37, 1078.97, 2792.00},
        {37, 1080.15, 2792.00}, {37, 1081.32, 2792.00}, {37, 1082.48, 2792.00}, {37, 1083.75, 2792.00},
        {37, 1084.92, 2792.00}, {37, 1086.19, 2792.00}, {37, 1087.27, 2792.00}, {37, 1088.43, 2792.00},
        {37, 1089.72, 2792.00}, {37, 1090.93, 2792.00}, {37, 1092.13, 2792.00}, {37, 1093.28, 2792.00},
        {37, 1094.47, 2792.00}, {37, 1095.66, 2792.00}, {37, 1096.87, 2792.00}, {37, 1098.05, 2792.00},
        {37, 1099.19, 2792.00}, {37, 1100.37, 2792.00}, {37, 1101.62, 2792.00}, {37, 1102.74, 2792.00},
        {37, 1103.90, 2792.00}, {37, 1105.02, 2792.00}, {37, 1106.20, 2792.00}, {37, 1107.34, 2792.00},
        {37, 1108.55, 2792.00}, {37, 1109.81, 2792.00}, {37, 1110.98, 2792.00}, {37, 1112.21, 2792.00},
        {37, 1113.35, 2792.00}, {37, 1114.52, 2792.00}, {37, 1115.70, 2792.00}, {37, 1116.66, 2792.00},
        {37, 1117.83, 2792.00}, {37, 1119.04, 2792.00}, {37, 1120.20, 2792.00}, {37, 1121.32, 2792.00},
        {37, 1122.51, 2792.00}, {37, 1123.60, 2792.00}, {37, 1124.76, 2792.00}, {37, 1125.88, 2792.00},
        {37, 1127.12, 2792.00}, {37, 1128.33, 2792.00}, {37, 1129.55, 2792.00}, {37, 1130.80, 2792.00},
        {37, 1131.94, 2792.00}, {37, 1133.15, 2792.00}, {37, 1134.34, 2792.00}, {37, 1135.30, 2792.00},
        {37, 1136.50, 2792.00}, {37, 1137.49, 2792.00}, {37, 1138.72, 2792.00}, {37, 1139.94, 2792.00},
        {37, 1141.14, 2792.00}, {37, 1142.35, 2792.00}, {37, 1143.60, 2792.00}, {37, 1144.76, 2792.00},
        {37, 1145.96, 2792.00}, {37, 1147.16, 2792.00}, {37, 1148.13, 2792.00}, {37, 1149.38, 2792.00},
        {37, 1150.63, 2792.00}, {37, 1151.60, 2792.00}, {37, 1152.65, 2792.00}, {37, 1153.82, 2792.00},
        {37, 1154.79, 2792.00}, {37, 1156.03, 2792.00}, {37, 1157.04, 2792.00}, {37, 1158.19, 2792.00},
        {37, 1159.45, 2792.00}, {37, 1160.69, 2792.00}, {37, 1161.88, 2792.00}, {37, 1163.09, 2792.00},
        {37, 1164.20, 2792.00}, {37, 1165.38, 2792.00}, {37, 1166.53, 2792.00}, {37, 1167.70, 2792.00},
        {37, 1168.90, 2792.00}, {37, 1170.05, 2792.00}, {37, 1171.24, 2792.00}, {37, 1172.40, 2792.00},
        {37, 1173.62, 2792.00}, {38, 1174.74, 2792.00}, {38, 1175.91, 2792.00}, {38, 1177.08, 2792.00},
        {38, 1178.21, 2792.00}, {38, 1179.37, 2792.00}, {38, 1180.62, 2792.00}, {38, 1181.62, 2792.00},
        {38, 1182.82, 2792.00}, {38, 1184.02, 2792.00}, {38, 1185.20, 2792.00}, {38, 1186.31, 2792.00},
        {38, 1187.58, 2792.00}, {38, 1188.70, 2792.00}, {38, 1189.95, 2792.00}, {38, 1191.10, 2792.00},
        {38, 1192.28, 2792.00}, {38, 1193.47, 2792.00}, {38, 1194.66, 2792.00}, {38, 1195.85, 2792.00},
        {38, 1196.98, 2792.00}, {38, 1198.13, 2792.00}, {38, 1199.30, 2792.00}, {38, 1200.46, 2792.00},
        {38, 1201.65, 2792.00}, {38, 1202.76, 2792.00}, {38, 1203.97, 2792.00}, {38, 1205.11, 2792.00},
        {38, 1206.09, 2792.00}, {38, 1207.27, 2792.00}, {38, 1208.50, 2792.00}, {38, 1209.65, 2792.00},
        {38, 1210.81, 2792.00}, {38, 1211.98, 2792.00}, {38, 1213.13, 2792.00}, {38, 1214.34, 2792.00},
        {38, 1215.47, 2792.00}, {38, 1216.63, 2792.00}, {38, 1217.74, 2792.00}, {38, 1218.89, 2792.00},
        {38, 1220.13, 2792.00}, {38, 1221.35, 2792.00}, {38, 1222.33, 2792.00}, {38, 1223.44, 2792.00},
        {38, 1224.66, 2792.00}, {38, 1225.83, 2792.00}, {38, 1226.84, 2792.00}, {38, 1227.92, 2792.00},
        {38, 1229.06, 2792.00}, {38, 1230.14, 2792.00}, {38, 1231.31, 2792.00}, {38, 1232.47, 2792.00},
        {38, 1233.64, 2792.00}, {38, 1234.57, 2792.00}, {38, 1235.77, 2792.00}, {38, 1237.00, 2792.00},
        {38, 1238.17, 2792.00}, {38, 1239.39, 2792.00}, {38, 1240.55, 2792.00}, {38, 1241.75, 2792.00},
        {38, 1243.03, 2792.00}, {38, 1244.24, 2792.00}, {38, 1245.28, 2792.00}, {38, 1246.25, 2792.00},
        {38, 1247.49, 2792.00}, {38, 1248.69, 2792.00}, {38, 1249.92, 2792.00}, {38, 1251.09, 2792.00},
        {38, 1252.33, 2792.00}, {38, 1253.49, 2792.00}, {38, 1254.71, 2792.00}, {38, 1255.88, 2792.00},
        {38, 1257.04, 2792.00}, {38, 1258.22, 2792.00}, {38, 1259.40, 2792.00}, {38, 1260.41, 2792.00},
        {38, 1261.63, 2792.00}, {38, 1262.87, 2792.00}, {38, 1264.07, 2792.00}, {38, 1265.33, 2792.00},
        {38, 1266.33, 2792.00}, {38, 1267.55, 2792.00}, {38, 1268.51, 2792.00}, {38, 1269.74, 2792.00},
        {38, 1270.97, 2792.00}, {38, 1272.22, 2792.00}, {38, 1273.42, 2792.00}, {38, 1274.64, 2792.00},
        {38, 1275.84, 2792.00}, {38, 1276.84, 2792.00}, {38, 1278.03, 2792.00}, {39, 1316.93, 2792.00},
        {39, 1321.12, 2792.02}, {39, 1331.02, 2791.99}, {39, 1340.60, 2792.00}, {39, 1354.90, 2791.99},
        {39, 1356.24, 2791.99}, {39, 1357.21, 2791.99}, {39, 1358.20, 2791.99}, {39, 1359.16, 2791.99},
        {39, 1360.44, 2791.99}, {39, 1361.67, 2791.99}, {39, 1362.92, 2791.99}, {39, 1363.89, 2791.99},
        {39, 1364.90, 2791.99}, {39, 1366.13, 2791.99}, {39, 1367.36, 2791.99}, {39, 1368.61, 2791.99},
        {39, 1369.82, 2791.99}, {39, 1371.01, 2791.99}, {39, 1372.15, 2791.99}, {39, 1373.42, 2791.99},
        {39, 1374.61, 2791.99}, {39, 1375.79, 2791.99}, {39, 1377.04, 2791.99}, {39, 1378.15, 2791.99},
        {39, 1379.34, 2791.99}, {39, 1380.45, 2791.99}, {39, 1381.64, 2791.99}, {39, 1382.78, 2791.99},
        {39, 1383.78, 2791.99}, {40, 1384.94, 2791.99}, {40, 1386.19, 2791.99}, {40, 1387.35, 2791.99},
        {40, 1388.51, 2791.99}, {40, 1389.73, 2791.99}, {40, 1390.90, 2791.99}, {40, 1392.11, 2791.99},
        {40, 1393.27, 2791.99}, {40, 1394.26, 2791.99}, {40, 1395.22, 2791.99}, {40, 1396.45, 2791.99},
        {40, 1397.60, 2791.99}, {40, 1398.71, 2791.99}, {40, 1399.98, 2791.99}, {40, 1401.18, 2791.99},
        {40, 1402.35, 2791.99}, {40, 1403.56, 2791.99}, {40, 1404.77, 2791.99}, {40, 1406.00, 2791.99},
        {40, 1407.12, 2791.99}, {40, 1408.28, 2791.99}, {40, 1409.40, 2791.99}, {40, 1410.56, 2791.99},
        {40, 1411.73, 2791.99}, {40, 1412.87, 2791.99}, {40, 1413.95, 2791.99}, {40, 1415.15, 2791.99},
        {40, 1416.34, 2791.99}, {40, 1417.45, 2791.99}, {40, 1418.60, 2791.99}, {40, 1419.67, 2791.99},
        {40, 1420.85, 2791.99}, {40, 1421.96, 2791.99}, {40, 1423.16, 2791.99}, {40, 1424.36, 2791.99},
        {40, 1425.50, 2791.99}, {40, 1426.68, 2791.99}, {40, 1427.85, 2791.99}, {40, 1429.04, 2791.99},
        {40, 1430.15, 2791.99}, {40, 1431.35, 2791.99}, {40, 1432.53, 2791.99}, {40, 1433.62, 2791.99},
        {40, 1434.80, 2791.99}, {40, 1435.93, 2791.99}, {40, 1437.11, 2791.99}, {40, 1438.26, 2791.99},
        {40, 1439.43, 2791.99}, {40, 1440.62, 2791.99}, {40, 1441.87, 2791.99}, {40, 1443.10, 2791.99},
        {40, 1444.23, 2791.99}, {40, 1445.43, 2791.99}, {40, 1484.91, 2791.99}, {40, 1486.12, 2791.98},
        {40, 1524.91, 2791.98}, {41, 1547.68, 2791.97}, {41, 1548.88, 2791.96}, {41, 1576.17, 2791.95},
        {41, 1588.20, 2791.93}, {41, 1598.92, 2791.93}, {41, 1600.07, 2791.92}, {41, 1622.43, 2791.91},
        {41, 1634.87, 2791.89}, {41, 1667.72, 2791.87}, {41, 1668.84, 2791.86}, {41, 1680.14, 2791.86},
        {42, 1716.04, 2791.83}, {42, 1717.14, 2791.82}, {42, 1741.90, 2791.81}, {42, 1743.12, 2791.80},
        {42, 1760.76, 2792.00}, {42, 1777.09, 2791.80}, {42, 1789.57, 2792.04}, {42, 1810.93, 2791.90},
        {43, 1845.06, 2792.16}, {43, 1863.63, 2791.90}, {43, 1875.02, 2791.94}, {43, 1884.16, 2791.94},
        {43, 1885.35, 2791.95}, {43, 1925.04, 2791.99}, {43, 1934.10, 2791.99}, {43, 1935.23, 2792.00},
        {44, 1944.45, 2792.00}, {44, 1945.67, 2792.01}, {44, 1981.65, 2792.04}, {44, 1994.59, 2792.04},
        {44, 1995.65, 2792.05}, {44, 2009.39, 2792.05}, {44, 2010.50, 2792.06}, {44, 2023.06, 2792.06},
        {44, 2046.07, 2792.09}, {44, 2057.07, 2792.09}, {44, 2058.32, 2792.10}, {45, 2084.00, 2792.11},
        {45, 2096.94, 2792.13}, {45, 2109.55, 2792.13}, {45, 2110.76, 2792.14}, {45, 2121.89, 2792.14},
        {45, 2156.71, 2792.17}, {45, 2181.67, 2791.18}, {45, 2197.03, 2789.87}, {46, 2224.14, 2786.56},
        {46, 2247.83, 2782.60}, {46, 2263.28, 2779.07}, {46, 2299.72, 2768.95}, {46, 2316.71, 2763.46},
        {47, 2346.21, 2751.97}, {47, 2371.73, 2740.56}, {47, 2393.96, 2729.29}, {47, 2414.78, 2717.36},
        {47, 2433.47, 2705.52}, {47, 2452.52, 2692.37}, {48, 2466.33, 2681.74}, {48, 2492.88, 2660.08},
        {49, 2509.16, 2645.40}, {49, 2511.31, 2642.63}, {49, 2512.60, 2639.48}, {49, 2512.44, 2634.59},
        {49, 2510.23, 2630.59}, {49, 2500.66, 2621.95}, {49, 2491.88, 2615.04}, {49, 2480.06, 2607.02},
        {50, 2458.95, 2594.88}, {50, 2438.03, 2585.16}, {50, 2408.77, 2575.99}, {50, 2394.93, 2572.90},
        {51, 2374.73, 2570.46}, {51, 2350.63, 2569.19}, {51, 2323.21, 2566.31}, {51, 2299.52, 2562.71},
        {52, 2279.57, 2557.80}, {52, 2260.20, 2551.61}, {52, 2239.95, 2543.20}, {52, 2222.86, 2534.26},
        {53, 2209.32, 2525.91}, {53, 2205.18, 2522.73}, {53, 2201.65, 2518.16}, {53, 2199.99, 2512.82},
        {53, 2200.18, 2505.06}, {53, 2202.21, 2499.37}, {54, 2211.69, 2488.84}, {54, 2232.27, 2464.77},
        {54, 2254.35, 2434.74}, {54, 2267.61, 2417.69},
    },
}

local function buildPlan(route)
    plan.route, plan.items, plan.serverCount = route, {}, 0
    local points = routePoints()
    if not points or #points == 0 then return end
    local server = {}
    for i = 1, #points do
        local p = pointAt(i)
        if not p then return end
        server[i] = p
    end
    local n = #server
    plan.serverCount = n
    plan.server = server
    local marks = {}
    for index, point in ipairs(WAYPOINTS) do
        if not point[5] or point[5] == route then
            marks[#marks + 1] = {index = index, point = point}
        end
    end
    -- Петлю замыкаем через базу: машина выезжает с базы к S1 и после Sn возвращается
    -- туда же. Тогда метки выезда из депо и место сдачи лежат на настоящих отрезках,
    -- а не меряются «до ближайшей точки», что при далёком депо их бы отбросило.
    local base = homePoint(locationOfRoute(route), server[1])
    local function cost(point, slot)
        if slot == 0 then
            if base then
                return segmentDistance(point[1], point[2], base.x, base.y, server[1].x, server[1].y)
            end
            return math.sqrt((point[1] - server[1].x) ^ 2 + (point[2] - server[1].y) ^ 2)
        elseif slot == n then
            if base then
                return segmentDistance(point[1], point[2], server[n].x, server[n].y, base.x, base.y)
            end
            return math.sqrt((point[1] - server[n].x) ^ 2 + (point[2] - server[n].y) ^ 2)
        end
        return segmentDistance(point[1], point[2], server[slot].x, server[slot].y,
            server[slot + 1].x, server[slot + 1].y)
    end
    local best, from = {}, {}
    for j = 1, #marks do
        best[j], from[j] = {}, {}
        local runMin, runArg = math.huge, 0
        for slot = 0, n do
            local own = cost(marks[j].point, slot)
            if j == 1 then
                best[j][slot] = own
            else
                if best[j - 1][slot] < runMin then runMin, runArg = best[j - 1][slot], slot end
                best[j][slot] = runMin + own
                from[j][slot] = runArg
            end
        end
    end
    local slots = {}
    if #marks > 0 then
        local bestSlot, bestCost = 0, math.huge
        for slot = 0, n do
            if best[#marks][slot] < bestCost then bestSlot, bestCost = slot, best[#marks][slot] end
        end
        slots[#marks] = bestSlot
        for j = #marks, 2, -1 do slots[j - 1] = from[j][slots[j]] end
    end
    -- Последняя метка у конца петли — это место сдачи, куда едут ПОСЛЕ маршрута.
    -- Динамика может отнести её к последнему отрезку (он проходит рядом с базой), и
    -- тогда при закрытии маршрута она засчиталась бы «пройденной» сама собой.
    -- Отличаем по геометрии: обычные метки лежат на маршруте (на маршруте 1 — от 1 до
    -- 39 м от его линии), а место сдачи стоит в стороне (96 м). Порог 50 м.
    if #marks > 0 and slots[#marks] < n then
        local last = marks[#marks].point
        if cost(last, slots[#marks]) > PLAN_DESTINATION and cost(last, n) <= PLAN_EDGE then
            slots[#marks] = n
        end
    end
    local bySlot = {}
    for j, mark in ipairs(marks) do
        local slot = slots[j]
        -- У концов петли (выезд из депо, место сдачи) метки стоят в стороне от точек.
        local limit = (slot <= 1 or slot >= n - 1) and PLAN_EDGE or PLAN_MID
        if cost(mark.point, slot) <= limit then
            bySlot[slot] = bySlot[slot] or {}
            table.insert(bySlot[slot], mark)
        end
    end
    for slot = 0, n do
        if slot >= 1 then
            plan.items[#plan.items + 1] = {kind = "server", index = slot,
                x = server[slot].x, y = server[slot].y}
        end
        local guides = GUIDES[route]
        if guides and slot >= 1 and slot < n and not bySlot[slot] then
            for _, g in ipairs(guides) do
                if g[1] == slot then
                    plan.items[#plan.items + 1] = {kind = "guide", slot = slot, x = g[2], y = g[3]}
                end
            end
        end
        for _, mark in ipairs(bySlot[slot] or {}) do
            local p = mark.point
            plan.items[#plan.items + 1] = {kind = "mark", mark = mark.index, slot = slot,
                x = p[1], y = p[2], h = p[3], r = p[4] or K.WAYPOINT_MID}
        end
    end
    local last = plan.items[#plan.items]
    if last and last.kind == "mark" then last.final = true end
    local markCount = 0
    for _, item in ipairs(plan.items) do
        if item.kind == "mark" then markCount = markCount + 1 end
    end
    local guideCount = 0
    for _, item in ipairs(plan.items) do
        if item.kind == "guide" then guideCount = guideCount + 1 end
    end
    emit("plan_built", {route = route, server = n, marks = markCount, guides = guideCount,
        dropped = #marks - markCount}, true)
    Path.rebuild()
end

-- Место метки на маршруте: отрезок (0 — от базы к S1, s — S_s→S_s+1, n — от S_n к базе) и
-- путь вдоль него. Курс метки должен идти по отрезку: маршрут — петля, у депо выезд и
-- въезд рядом, и без курса метку въезда можно было бы принять за метку выезда.
local function routeSpot(route, x, y, h)
    local server = plan.server
    local n = #server
    local base = homePoint(locationOfRoute(route), server[1])
    local best, bestSlot, bestAlong
    for slot = 0, n do
        local a, b
        if slot == 0 then a, b = base or server[1], server[1]
        elseif slot == n then a, b = server[n], base or server[n]
        else a, b = server[slot], server[slot + 1] end
        local dx, dy = b.x - a.x, b.y - a.y
        local length = math.sqrt(dx * dx + dy * dy)
        local d, along = math.sqrt((x - a.x) ^ 2 + (y - a.y) ^ 2), 0
        if length > 0.5 then
            local k = clamp(((x - a.x) * dx + (y - a.y) * dy) / (length * length), 0, 1)
            d = math.sqrt((x - a.x - dx * k) ^ 2 + (y - a.y - dy * k) ^ 2)
            along = k * length
            if finite(h) and math.abs(angle(bearingTo(dx, dy) - h)) > 75 then d = d + 50 end
        end
        if not best or d < best then best, bestSlot, bestAlong = d, slot, along end
    end
    return bestSlot, bestAlong
end

placeInOrder = function(point)
    local route = point[5]
    if not route or plan.route ~= route or not plan.server or #plan.server < 2 then
        return #WAYPOINTS + 1
    end
    local slot, along = routeSpot(route, point[1], point[2], point[3])
    local last = 0
    for index, other in ipairs(WAYPOINTS) do
        if other[5] == route then
            local s, a = routeSpot(route, other[1], other[2], other[3])
            if s > slot or (s == slot and a > along) then return index end
            last = index
        end
    end
    return last > 0 and last + 1 or #WAYPOINTS + 1
end

-- Звено ещё впереди? Серверная точка — пока сервер её не засчитал; метка — пока её
-- не задели и сервер не ушёл дальше отрезка, на котором она стоит.
local function planPending(item, current)
    if item.kind == "server" then return item.index >= current end
    if item.done then return false end
    return item.slot + 1 >= current
end

local function planLabel(item)
    if item.kind ~= "mark" then return nil end
    return string.format("метка #%d/%d (r%g)%s", item.mark, #WAYPOINTS, item.r,
        item.final and " ФИНИШ" or "")
end

-- Метки выезда из депо — рельсы. Цель в центр каждой метки не годится: бот проходит
-- центр со старым курсом и доворачивает уже за ним, а в воротах депо (будка справа,
-- столбик слева, 4.7 м между ними) снос на доворот — это удар. Поэтому ведём по самим
-- отрезкам между центрами: цель — точка на ломаной в 1.5 с пути впереди (8–14 м), и
-- доворот начинается заранее, внутри угла.
-- Короче нельзя: на 6 м грузовик с запаздыванием руля перелетал курс рельса на 11°
-- и дёргался обратно — в будку.
local function railLookahead(speed, most)
    return clamp(toMs(speed or 0) * 1.5, 8, most or 14)
end

-- Отдаёт цель и отрезок рельса, на котором она стоит: от него считается сдвиг мимо помех.
local function railAim(position, speed, a, b, c, most)
    local abx, aby = b.x - a.x, b.y - a.y
    local ab = math.sqrt(abx * abx + aby * aby)
    local legAB = {a.x, a.y, b.x, b.y, b.r}
    if ab < 0.5 then return {x = b.x, y = b.y, z = position.z}, legAB end
    local along = ((position.x - a.x) * abx + (position.y - a.y) * aby) / ab
    local reach = math.max(along, 0) + railLookahead(speed, most)
    if reach <= ab then
        return {x = a.x + abx / ab * reach, y = a.y + aby / ab * reach, z = position.z}, legAB
    end
    if c then
        local bcx, bcy = c.x - b.x, c.y - b.y
        local bc = math.sqrt(bcx * bcx + bcy * bcy)
        if bc > 0.5 then
            local rest = math.min(reach - ab, bc)
            return {x = b.x + bcx / bc * rest, y = b.y + bcy / bc * rest, z = position.z},
                {b.x, b.y, c.x, c.y, c.r}
        end
    end
    return {x = b.x, y = b.y, z = position.z}, legAB
end

-- Рельсы — метки выезда из депо (до первой точки) и метки въезда (после последней).
local function isRail(item)
    return item ~= nil and item.kind == "mark" and not item.final
        and (item.slot == 0 or item.slot == plan.serverCount)
end

-- Где у серверной точки ехал водитель в эталонном ручном заезде: смещение вбок, плюс —
-- вправо по ходу. Точки сервера стоят на разметке между полосами, а водитель держался
-- в своей полосе, на 1.5–2.4 м правее. Бот ехал ровно по разметке, и его подрезали.
-- Маршрут 1, эталонный заезд 28.09.2026 по правой полосе (PlowBot.ride2.log), S1…S54.
local LANE_OFFSETS = {
    [1] = {
        0.8, 1.9, 1.6, 1.5, 1.9, 1.0, 1.1, 1.7, 1.7, 2.4, 1.8, 1.7, 1.3, 1.2, 0.5, 0.9,
        0.7, 0.7, 1.5, 1.7, 1.7, 2.1, 0.2, -1.6, -0.8, -0.3, 1.1, 2.1, 1.0, 1.9, 0.6, 1.7,
        0.7, 1.4, 2.0, 1.6, 1.6, 1.5, 1.5, 1.6, 2.3, 2.2, 1.7, 2.1, 2.0, 1.1, 1.4, 1.8,
        0.7, 1.5, 1.1, 0.9, 1.1, 0.1},
}

local function laneShifted(index, point, z)
    local offsets = LANE_OFFSETS[plan.route]
    local offset = offsets and offsets[index]
    local server = plan.server
    if not finite(offset) or offset == 0 or not server or not server[index] then
        return {x = point.x, y = point.y, z = z}
    end
    local a = server[math.max(1, index - 1)]
    local b = server[math.min(#server, index + 1)]
    local dx, dy = b.x - a.x, b.y - a.y
    local length = math.sqrt(dx * dx + dy * dy)
    if length < 1 then return {x = point.x, y = point.y, z = z} end
    dx, dy = dx / length, dy / length
    return {x = point.x + dy * offset, y = point.y - dx * offset, z = z}
end

-- Текущее звено цепочки и следующее за ним. current — CURRENT_POSITION_ID сервера,
-- nil — маршрут уже закрыт (тогда остались только метки после последней точки).
local function planStep(position, heading, current, live, speed)
    local items = plan.items
    if #items == 0 or not position or not finite(heading) then return nil end
    current = current or (plan.serverCount + 1)
    for _, item in ipairs(items) do
        if item.kind == "mark" and not item.done and item.slot + 1 < current then
            item.done = true   -- участок маршрута уже пройден
        end
    end
    local function nextAfter(i)
        for j = i + 1, #items do
            if planPending(items[j], current) then return items[j], j end
        end
        return nil
    end
    -- Прямую между точками больше не держим: на изгибах она идёт по тротуару и забору
    -- (у точек 26, 47, 48 водитель ехал на 1.5–4 м в стороне от неё). Едем прямо на цель,
    -- как раньше, а ленивую дугу в поворотах лечит расчёт кривизны в контроллере.
    -- Цель звена: центр метки или точка сервера со смещением водителя.
    local function goalOf(item)
        if not item then return nil end
        if item.kind ~= "server" then return {x = item.x, y = item.y, z = position.z} end
        return laneShifted(item.index, item, position.z)
    end
    -- Угол поворота в цели: между подходом к ней и отрезком к следующей.
    local function turnAt(target, follow)
        if not follow then return 0 end
        return math.abs(angle(bearingTo(follow.x - target.x, follow.y - target.y)
            - bearingTo(target.x - position.x, target.y - position.y)))
    end
    -- Заход в поворот. Бот ехал в точку и узнавал о следующей только на ней: на метках
    -- 6–7–8 ошибка курса прыгала на 13–19° и руль дёргался до 0.8–0.9 на скорости 51.
    -- Водитель начинает поворот заранее: когда до цели остаётся ~1.2 с пути, прицел
    -- переходит на следующий отрезок, и руль плавно перетекает в поворот. Только для
    -- поворотов до 90°: круче — доезжаем до точки и поворачиваем резко.
    -- allowed — насколько путь может пройти в стороне от цели: у метки это её зона вместе
    -- с бортом, у точки — сфера зачёта 7 м. Прицел уходит на следующий отрезок ровно
    -- настолько, чтобы прямая «машина → прицел» прошла от цели не дальше allowed: без этого
    -- бот срезал угол мимо маленьких меток и потом возвращался к ним.
    local function lookThrough(target, follow, allowed)
        if not follow then return target end
        local d = math.sqrt((target.x - position.x) ^ 2 + (target.y - position.y) ^ 2)
        local reach = clamp(toMs(speed or 0) * 1.2, 8, 25)
        if d >= reach or turnAt(target, follow) > 90 then return target end
        local dx, dy = follow.x - target.x, follow.y - target.y
        local length = math.sqrt(dx * dx + dy * dy)
        if length < 0.5 then return target end
        dx, dy = dx / length, dy / length
        local best = 0
        for k = 1, 12 do
            local carry = math.min(reach - d, length) * k / 12
            local ax, ay = target.x + dx * carry, target.y + dy * carry
            if segmentDistance(target.x, target.y, position.x, position.y, ax, ay) > allowed then
                break
            end
            best = carry
        end
        return {x = target.x + dx * best, y = target.y + dy * best, z = target.z}
    end
    -- Прицел у своей метки. Центр проходим не дальше PASS.markThrough, а в поворот за меткой
    -- входим плавно: руль ведём так, как просит следующая цель, но не больше, чем позволяет
    -- центр. Кривизна k вместо дуги «прямо в центр» (kM) уводит путь в центре на
    -- (k − kM)·d²/2, отсюда допуск 2·PASS.markThrough/d²: вдали он почти ноль — едем в центр,
    -- за 4–6 м до центра руль плавно входит в поворот. Прежний «заход в поворот» с допуском
    -- по прямой к прицелу для меток не годился: прицел уезжал на следующий отрезок и
    -- возвращался, и перед центром метки 4 руль перекладывался из −0.5 в +0.8.
    local function curvatureTo(point, most)
        local dx, dy = point.x - position.x, point.y - position.y
        local d = math.sqrt(dx * dx + dy * dy)
        if d < 0.5 then return 0, d, 0 end
        local err = angle(bearingTo(dx, dy) - heading)
        if math.abs(err) >= 90 then
            -- Как в контроллере: цель сбоку или сзади — руль в упор.
            return (err >= 0 and 1 or -1) / minRadius(toMs(speed or 0)), d, err
        end
        return 2 * math.sin(math.rad(err)) / math.max(1, math.min(d, most or d)), d, err
    end
    local function markAim(target, follow, tolerance)
        if not follow or turnAt(target, follow) > 90 then return target end
        local ms = toMs(speed or 0)
        local kM, d, errM = curvatureTo(target)
        -- Дальше 20 м допуск меньше сантиметра — там просто едем в центр, как контроллер.
        -- Метка сбоку (после поворота у точки 23 метка 5 была в 64 м под 90°) — тоже.
        if d < 0.5 or d > 20 or math.abs(errM) > 60 then return target end
        local kN = curvatureTo(follow, clamp(ms * 2.0, 15, 40))
        local slack = 2 * (tolerance or PASS.markThrough) / (d * d)
        local k = clamp(kN, kM - slack, kM + slack)
        if math.abs(k - kM) < 1e-4 then return target end
        -- Прицел на дуге кривизны k: контроллер считает дугу на расстояние до прицела
        -- (оно короче 15 м), и 2·sin(e)/L = k.
        local L = clamp(ms * 0.8, 6, 12)
        local r = math.rad(heading + math.deg(math.asin(clamp(k * L / 2, -1, 1))))
        return {x = position.x - math.sin(r) * L, y = position.y + math.cos(r) * L, z = target.z}
    end
    -- Звено сразу после крутого угла (больше 80°): там доворачиваем резко, пока не
    -- выровнялись. Водитель у метки 5 проходит угол «квадратом», а ленивая дуга к
    -- точке в 55 м уводила бота на 20 м в сторону.
    local function afterSharp(i)
        local a, b, c = items[i - 2], items[i - 1], items[i]
        if not a or not b then return false end
        return math.abs(angle(bearingTo(c.x - b.x, c.y - b.y) - bearingTo(b.x - a.x, b.y - a.y))) > 80
    end
    -- Прицел через точку сервера прямо перед этим звеном, пока она ещё впереди по дороге.
    local function ghostAim(i, target)
        local prev = items[i - 1]
        if not prev or prev.kind ~= "server" or isRail(items[i]) then return nil end
        -- Только там, где линия известна: к своей метке или на маршруте с линией водителя
        -- (смещения полосы). На маршрутах без них точка стоит на разметке между полосами, и
        -- проход ровно через неё добавил вдвое рывков руля (маршруты 2–9 в тесте).
        if items[i].kind ~= "mark" and not LANE_OFFSETS[plan.route] then return nil end
        local pass = laneShifted(prev.index, prev, position.z)
        local rad = math.rad(heading)
        local ahead = (pass.x - position.x) * -math.sin(rad) + (pass.y - position.y) * math.cos(rad)
        if ahead <= PASS.ghostAhead then return nil end
        local d = math.sqrt((pass.x - position.x) ^ 2 + (pass.y - position.y) ^ 2)
        if d > 20 then return nil end
        if math.abs(angle(bearingTo(pass.x - position.x, pass.y - position.y) - heading)) > 45 then
            return nil
        end
        if turnAt(pass, target) > PASS.ghostTurn then return nil end
        return markAim(pass, target, PASS.serverThrough), pass
    end
    local function build(i)
        local item = items[i]
        -- target — настоящая цель (по ней торможение и поворот), aim — куда рулить.
        local target, aim, rail
        if (isRail(item) and isRail(items[i - 1]))
            or (item.kind == "mark" and item.final and items[i - 1]) then
            -- По рельсу: на выезде — от метки к метке депо до первой точки; к месту сдачи —
            -- всегда, от прошлого звена (метки или последней точки) через ворота депо.
            -- Дальше следующей метки депо не смотрим.
            target = {x = item.x, y = item.y, z = position.z}
            aim, rail = railAim(position, speed, items[i - 1], item,
                isRail(items[i + 1]) and items[i + 1] or nil)
        elseif item.kind == "guide" then
            target = {x = item.x, y = item.y, z = position.z}
            aim = lookThrough(target, goalOf((nextAfter(i))), 3)
        elseif item.kind == "mark" then
            -- В центр метки: его и ставил водитель. От помех бот уходит только внутри зоны.
            target = {x = item.x, y = item.y, z = position.z}
            aim = markAim(target, goalOf((nextAfter(i))))
        elseif item.index == current and live then
            target = laneShifted(item.index, live, position.z)
            aim = lookThrough(target, goalOf((nextAfter(i))), CAPTURE - 3)
        else
            target = laneShifted(item.index, item, position.z)
            aim = lookThrough(target, goalOf((nextAfter(i))), CAPTURE - 3)
        end
        local ghost, ghostPass
        if not rail and item.kind ~= "guide" then ghost, ghostPass = ghostAim(i, target) end
        if ghost then aim = ghost end
        local follow, fj = nextAfter(i)
        local after = fj and nextAfter(fj) or nil
        return {item = item, aim = aim, target = target, rail = rail, final = item.final == true,
            ghost = ghostPass,
            sharp = rail == nil and afterSharp(i),
            label = planLabel(item),
            distance = math.sqrt((target.x - position.x) ^ 2 + (target.y - position.y) ^ 2),
            next = follow and {x = follow.x, y = follow.y, z = position.z} or nil,
            after = after and {x = after.x, y = after.y, z = position.z} or nil}
    end
    for i, item in ipairs(items) do
        if planPending(item, current) then
            local d = math.sqrt((item.x - position.x) ^ 2 + (item.y - position.y) ^ 2)
            if item.kind == "mark" then
                local err = math.abs(angle(bearingTo(item.x - position.x,
                    item.y - position.y) - heading))
                -- Метку только что поставили под машиной: пока стоим в ней, она не цель и
                -- не засчитывается. Выехали — дальше она обычная метка, в этом же кадре.
                local fresh = state.waypointSkip == item.mark
                if fresh and d > item.r + BODY_HALF_WIDTH then
                    state.waypointSkip, fresh = nil, false
                end
                -- Метку выезда засчитываем, только когда её центр под кабиной (не дальше
                -- 2 м впереди центра машины): иначе бот уходил к следующей за 2.6 м до
                -- центра и срезал угол к воротам.
                local rad = math.rad(heading)
                local ahead = (item.x - position.x) * -math.sin(rad)
                    + (item.y - position.y) * math.cos(rad)
                -- Любую свою метку засчитываем только у центра: когда он поравнялся с
                -- машиной (рельсы депо — за 2 м). Раньше хватало задеть зону, и бот уходил
                -- к следующей цели за 4–5 м до центра, срезая угол.
                local reach = item.r + BODY_HALF_WIDTH
                local reached = d <= reach and ahead <= (isRail(item) and 2 or PASS.markAhead)
                -- Центр уже сбоку или позади, а зона не задета — прошли мимо. Назад к ней не
                -- разворачиваемся: руль в упор к метке за спиной хуже промаха.
                local passedBy = not isRail(item) and ahead <= 0 and d <= reach + 3
                if fresh then
                    -- стоим внутри свежей метки — смотрим следующее звено
                elseif reached and not item.final then
                    item.done = true   -- задета
                    state.waypointPassed = item.mark
                elseif passedBy and not item.final then
                    item.done, item.missed = true, true
                    state.waypointMissed = item.mark
                elseif err > PLAN_BEHIND and d > math.max(2 * item.r, 10) and not item.final then
                    item.done, item.missed = true, true   -- позади: не разворачиваемся
                    state.waypointMissed = item.mark
                else
                    return build(i)
                end
            elseif item.kind == "guide" then
                -- Направляющую не засчитывают: проехали рядом или она уже позади — дальше.
                local err = math.abs(angle(bearingTo(item.x - position.x,
                    item.y - position.y) - heading))
                if d < 10 or (err > 100 and d > 3) then
                    item.done = true
                else
                    return build(i)
                end
            else
                -- Серверная точка под колёсами (сфера 7 м) засчитается сама — рулим
                -- уже на следующее звено, иначе машина доворачивает в неё впритык.
                local follow, j = nextAfter(i)
                if d < 9 and follow then return build(j) end
                return build(i)
            end
        end
    end
    return nil
end

-- === светофоры ===
-- getTrafficLightState() отдаёт одно число на весь город, а какое из них «зелёный»
-- зависит от направления улицы. Поэтому для каждого перекрёстка запоминаем своё
-- значение: игрок останавливается на стоп-линии, дожидается зелёного и сохраняет.
K.TRAFFIC_SEE = 60      -- дальше этого светофор нас не касается
K.TRAFFIC_CONE = 70     -- и он должен быть впереди, а не сбоку
K.TRAFFIC_SAME = 12     -- ближе этого считаем, что это тот же самый светофор

-- Обученные светофоры: {x, y, green, heading}. Курс обязателен: у одного и того же
-- перекрёстка «зелёное» значение своё для каждого направления, поэтому запись
-- применяется, только когда машина едет примерно так же, как при обучении.
K.TRAFFIC_HEADING = 60  -- допуск по курсу, чтобы не спутать направления
-- Обучено вручную на маршруте 1 (Вокзал – Заводское шоссе), 27.09.2026.
-- Разрешающих состояний на этом сервере два: 0 и 3 — для перпендикулярных улиц.
local TRAFFIC = {
    {2165.20, 2443.91, 0, 51.0},
    {1336.42, 2583.26, 3, 90.0},
    {1077.01, 2583.26, 3, 90.0},
    {752.41, 2583.56, 3, 89.8},
    {465.60, 2583.05, 3, 90.4},
    {-11.05, 2582.66, 3, 87.5},
    {426.00, 2792.02, 3, 270.4},
    {1280.30, 2792.24, 3, 269.9},
}

local function trafficState()
    local value = read("getTrafficLightState")
    value = tonumber(value)
    return finite(value) and value or nil
end

-- Ближайшая запись, сделанная примерно в том же направлении: встречный светофор
-- на том же перекрёстке — это отдельная запись, а не та же самая.
local function trafficNear(position, radius, heading)
    if not position then return nil end
    local best, bestDistance
    for index, light in ipairs(TRAFFIC) do
        local distance = math.sqrt((light[1] - position.x) ^ 2 + (light[2] - position.y) ^ 2)
        local sameWay = true
        if finite(heading) and finite(light[4]) then
            sameWay = math.abs(angle(light[4] - heading)) <= K.TRAFFIC_HEADING
        end
        if sameWay and distance <= (radius or K.TRAFFIC_SAME)
            and (not bestDistance or distance < bestDistance) then
            best, bestDistance = index, distance
        end
    end
    return best, bestDistance
end

-- Светофор, который реально стоит у нас на пути: близко и в конусе перед носом.
-- Светофоры могут быть отключены: сервер объявляет об этом в чат, а движок отдаёт
-- состояние 9 (мигающий/выключенный режим). В обоих случаях тормозить перед каждым
-- перекрёстком нельзя — бот встанет намертво, потому что ни одно «зелёное» не совпадёт.
local function trafficWorking()
    if state.ignoreTraffic then return false end
    local value = trafficState()
    if value == 9 then return false end
    return true
end

local function trafficAhead(position, heading)
    if not position or not finite(heading) then return nil end
    if not trafficWorking() then return nil end
    local best, bestDistance
    for _, light in ipairs(TRAFFIC) do
        local dx, dy = light[1] - position.x, light[2] - position.y
        local distance = math.sqrt(dx * dx + dy * dy)
        if distance <= K.TRAFFIC_SEE then
            local err = math.abs(angle(bearingTo(dx, dy) - heading))
            -- Едем ли мы так же, как при обучении: иначе это светофор для другого
            -- направления, и его «зелёное» значение к нам не относится.
            local sameWay = not finite(light[4])
                or math.abs(angle(light[4] - heading)) <= K.TRAFFIC_HEADING
            if sameWay and err <= K.TRAFFIC_CONE
                and (not bestDistance or distance < bestDistance) then
                best, bestDistance = light, distance
            end
        end
    end
    if not best then return nil end
    local state = trafficState()
    return {distance = bestDistance, green = state ~= nil and state == best[3],
        state = state, green_state = best[3]}
end

-- Когда светофоры выключены, их метки всё равно полезны: они стоят ровно на
-- стоп-линии по центру полосы. Используем их как промежуточную точку проезда,
-- чтобы бот входил в перекрёсток правильно, а не срезал угол.
local function trafficWaypoint(position, heading, targetDistance)
    if not position or not finite(heading) then return nil end
    local best, bestDistance
    for _, light in ipairs(TRAFFIC) do
        local dx, dy = light[1] - position.x, light[2] - position.y
        local distance = math.sqrt(dx * dx + dy * dy)
        if distance >= 6 and distance <= 45
            and (not finite(targetDistance) or distance < targetDistance - 4) then
            local err = math.abs(angle(bearingTo(dx, dy) - heading))
            local sameWay = not finite(light[4])
                or math.abs(angle(light[4] - heading)) <= K.TRAFFIC_HEADING
            if sameWay and err <= 35 and (not bestDistance or distance < bestDistance) then
                best, bestDistance = light, distance
            end
        end
    end
    if not best then return nil end
    return {x = best[1], y = best[2], z = position.z}, bestDistance
end

local function trafficDump()
    local parts = {}
    for _, light in ipairs(TRAFFIC) do
        parts[#parts + 1] = string.format("    {%.2f, %.2f, %d, %.1f},",
            light[1], light[2], light[3], finite(light[4]) and light[4] or 0)
    end
    local text = "local TRAFFIC = {\n" .. table.concat(parts, "\n") .. "\n}"
    emit("traffic_table", {count = #TRAFFIC, lua = text}, true)
    return text
end

local function trafficSave(position, heading)
    if not position then
        note("Светофор: не вижу машину")
        return
    end
    local state = trafficState()
    if not state then
        note("Светофор: состояние недоступно")
        return
    end
    local existing = trafficNear(position, K.TRAFFIC_SAME, heading)
    if existing then
        TRAFFIC[existing][3] = state
        TRAFFIC[existing][4] = finite(heading) and heading or TRAFFIC[existing][4]
        note(string.format("Светофор обновлён: зелёный = %d (всего %d)", state, #TRAFFIC))
    else
        TRAFFIC[#TRAFFIC + 1] = {position.x, position.y, state,
            finite(heading) and heading or 0}
        note(string.format("Светофор сохранён: зелёный = %d, курс %d (всего %d)",
            state, math.floor(finite(heading) and heading or 0), #TRAFFIC))
    end
    emit("traffic_saved", {x = position.x, y = position.y, heading = heading,
        green = state, total = #TRAFFIC}, true)
    trafficDump()
end

local function trafficForget(position, heading)
    local index = trafficNear(position, K.TRAFFIC_SEE, heading)
    if not index then
        note("Светофор: рядом ничего не сохранено")
        return
    end
    table.remove(TRAFFIC, index)
    note("Светофор забыт, осталось " .. #TRAFFIC)
    trafficDump()
end

-- === управление ===

-- Три луча вперёд (центр и борта). Точки маршрута соединены прямой, а дорога — нет,
-- поэтому впереди бывает и трафик, и столб. Дальность — тормозной путь плюс запас.
local NOSE = 4.2   -- полудлина машины 18761: луч стартует перед бампером, не в корпусе
-- Лучи по ширине: два по бортам и четыре внутренних с шагом 0.625 м, которые каждый кадр
-- сдвигаются на треть шага. За три кадра сетка — 0.21 м, тоньше любого столба. С
-- неподвижными лучами через 0.6 м столб 0.24 м, стоящий ровно на линии маршрута,
-- проскакивал между ними при каждом проезде, а память не могла его запомнить.
local RAY_EDGE = 1.25
local RAY_STEP = 0.625
local rayPhase = 1
local function rayOffsets()
    local list = {-RAY_EDGE, RAY_EDGE}
    for k = 0, 3 do list[#list + 1] = -RAY_EDGE + (k + rayPhase / 3) * RAY_STEP end
    return list
end

-- === дуга движения ===
-- Помехи ищем не по прямой курса, а по дуге, которую машина реально опишет: кривизна —
-- та, что просит руль, но не круче физически возможной на этой скорости. Прямой луч в
-- повороте видел то, что стоит по курсу, но куда машина уже не едет (угол будки в
-- воротах депо, бот вставал перед ним), и не видел того, что стоит на самой дуге.
local ARC_STRAIGHT = 1 / 250   -- положе этого — прямая
local ARC_SEGMENTS = 3

-- Точка и курс на дуге через s метров пути центра машины (кривизна: плюс — влево).
local function arcPose(position, heading, curvature, s)
    local rad = math.rad(heading)
    local fx, fy = -math.sin(rad), math.cos(rad)
    if math.abs(curvature) < ARC_STRAIGHT then
        return position.x + fx * s, position.y + fy * s, heading
    end
    local turn = curvature * s
    local along, aside = math.sin(turn) / curvature, (1 - math.cos(turn)) / curvature
    return position.x + fx * along - fy * aside, position.y + fy * along + fx * aside,
        heading + math.deg(turn)
end

-- Где точка относительно дуги: путь вдоль неё (s, от центра машины) и сдвиг вбок
-- (side, плюс — справа). На прямой — обычные координаты машины.
local function arcLocal(position, heading, curvature, x, y)
    local rad = math.rad(heading)
    local fx, fy = -math.sin(rad), math.cos(rad)
    local dx, dy = x - position.x, y - position.y
    if math.abs(curvature) < ARC_STRAIGHT then
        return dx * fx + dy * fy, dx * fy - dy * fx
    end
    local radius, sign = 1 / math.abs(curvature), curvature > 0 and 1 or -1
    -- Центр поворота: слева для левого, справа для правого.
    local cx, cy = position.x - fy * radius * sign, position.y + fx * radius * sign
    local ux, uy = position.x - cx, position.y - cy
    local wx, wy = x - cx, y - cy
    local phi = sign * math.atan2(ux * wy - uy * wx, ux * wx + uy * wy)
    local rho = math.sqrt(wx * wx + wy * wy)
    return phi * radius, sign > 0 and (rho - radius) or (radius - rho)
end

-- Нос машины: от него начинаются дуги лучей. Дуга от центра уводила начало лучей вбок,
-- хотя стоящая машина ещё никуда не повернула: руль влево — и столб у правого угла
-- переставал быть помехой, руль прямо — снова помеха (метка 4, 20 с качелей).
local function noseOf(position, heading)
    local rad = math.rad(heading)
    return {x = position.x - math.sin(rad) * NOSE, y = position.y + math.cos(rad) * NOSE,
        z = position.z}
end

-- === путь по меткам — как у самолёта: весь путь известен заранее ===
-- Раньше бот рулил от цели к цели: доехал до метки — прицел прыгал на следующую, ошибка курса
-- скакала на 19–24° (метки 9, 10, 11 в заезде 2.10.3), руль уходил в 0.8 и машину несло на
-- бордюр; у метки 8 прицел «за метку / в метку» перещёлкивался — руль пилой. Теперь при выдаче
-- маршрута строится гладкая линия через центры всех меток (кубические кривые Эрмита), а между
-- метками — по линии водителя из эталонного заезда (ride2: правая полоса, без тротуаров),
-- плавно притянутой к меткам. Бот едет по ней, как самолёт по построенной траектории: прицел —
-- точка на линии в ~1 с пути впереди, лучи помех идут вдоль этой же линии, а не по дуге руля
-- (дуга руля в поворотах упиралась в стены и бордюры снаружи, и бот уходил «в объезд» на
-- соседнюю полосу — 25 раз за заезд 2.10.3).
Path.STEP, Path.MAXM, Path.SHARP, Path.SHARPM, Path.DEDUPE, Path.PHASE = 1.0, 30, 45, 18, 5, 0.5
Path.TAPER, Path.NEAR = 35, 4          -- притяжение линии водителя к метке: ±25 м, у метки — сама метка
Path.LEAD, Path.LEAD_MIN, Path.LEAD_MAX = 1.0, 6, 18   -- прицел: 1 с пути, 6–18 м
Path.TOL, Path.TOL_MARK = 1.0, 0.6     -- насколько прямая к прицелу может срезать линию
Path.OFF = 8                           -- дальше этого от линии — едем по-старому, к цели
Path.CUT = 5                           -- без линии водителя: угол у точки сервера срезаем до 5 м
Path.n = 0

do
    local function unit(x, y)
        local l = math.sqrt(x * x + y * y)
        if l < 1e-9 then return 0, 0, 0 end
        return x / l, y / l, l
    end
    local function rotate(x, y, deg)
        local r = math.rad(deg)
        return x * math.cos(r) - y * math.sin(r), x * math.sin(r) + y * math.cos(r)
    end

    -- Линия водителя, притянутая к меткам: метка в стороне от линии на delta — линию рядом с
    -- ней плавно сдвигаем на столько же (косинусом на ±TAPER м вдоль линии), точки линии у самой
    -- метки убираем. Без этого путь шёл «линия — метка — линия» зигзагом.
    local function bend(line, marks)
        local n = #line
        local s, nx, ny = {}, {}, {}
        for i = 1, n do
            s[i] = i == 1 and 0 or s[i - 1] + math.sqrt((line[i][2] - line[i - 1][2]) ^ 2
                + (line[i][3] - line[i - 1][3]) ^ 2)
            local a, b = line[math.max(1, i - 1)], line[math.min(n, i + 1)]
            local ux, uy = unit(b[2] - a[2], b[3] - a[3])
            nx[i], ny[i] = uy, -ux   -- вправо по ходу
        end
        local placed = {}
        for _, mk in ipairs(marks) do
            local best, bestAlong, bestDelta
            for i = 1, n - 1 do
                local a, b = line[i], line[i + 1]
                if math.abs(a[1] - mk.slot) <= 1 then
                    local dx, dy = b[2] - a[2], b[3] - a[3]
                    local L2 = dx * dx + dy * dy
                    local t = L2 > 0 and clamp(((mk.x - a[2]) * dx + (mk.y - a[3]) * dy) / L2, 0, 1) or 0
                    local px, py = a[2] + dx * t, a[3] + dy * t
                    local d = math.sqrt((mk.x - px) ^ 2 + (mk.y - py) ^ 2)
                    if not best or d < best then
                        local L = math.sqrt(L2)
                        best, bestAlong = d, s[i] + t * L
                        bestDelta = L > 0 and ((mk.x - px) * dy - (mk.y - py) * dx) / L or 0
                    end
                end
            end
            mk.along = best and best <= 40 and bestAlong or nil
            -- Метка дальше 15 м от линии — линию к ней не тянем.
            if best and best <= 15 then placed[#placed + 1] = {along = bestAlong, delta = bestDelta} end
        end
        -- Сдвиг линии вдоль пути: между соседними метками (до 80 м) — плавный переход от
        -- сдвига одной к сдвигу другой; до первой и после последней в группе — сходит на нет за
        -- TAPER. Горб у каждой метки по отдельности давал волну: метка 9 левее линии на 0.7 м,
        -- метка 10 — на 2 м, и путь между ними возвращался на линию и снова уходил.
        table.sort(placed, function(p, q) return p.along < q.along end)
        local function taper(d) return d < Path.TAPER and 0.5 * (1 + math.cos(math.pi * d / Path.TAPER)) or 0 end
        local out = {}
        local k = 1
        for i = 1, n do
            while k <= #placed and placed[k].along <= s[i] do k = k + 1 end
            local prev, nxt = placed[k - 1], placed[k]
            local shift, drop = 0, false
            if (prev and s[i] - prev.along < Path.NEAR) or (nxt and nxt.along - s[i] < Path.NEAR) then drop = true end
            if prev and nxt and nxt.along - prev.along <= 80 then
                local u = (s[i] - prev.along) / math.max(0.01, nxt.along - prev.along)
                u = u * u * (3 - 2 * u)
                shift = prev.delta + (nxt.delta - prev.delta) * u
            else
                if prev then shift = shift + prev.delta * taper(s[i] - prev.along) end
                if nxt then shift = shift + nxt.delta * taper(nxt.along - s[i]) end
            end
            if not drop then
                out[#out + 1] = {kind = "line", slot = line[i][1], x = line[i][2] + nx[i] * shift,
                    y = line[i][3] + ny[i] * shift, along = s[i], order = 0}
            end
        end
        return out
    end

    -- Метки и линия водителя в одном порядке — по пути вдоль линии. Метка не на линии
    -- (выезд из депо, место сдачи) встаёт после последней точки линии своего отрезка.
    local function merge(points, marks)
        local list = {}
        for _, p in ipairs(points) do list[#list + 1] = p end
        for _, mk in ipairs(marks) do
            local along = mk.along
            if not along then
                along = -1000
                for _, p in ipairs(points) do
                    if p.slot <= mk.slot and p.along >= along then along = p.along + 0.01 end
                end
            end
            list[#list + 1] = {kind = "mark", x = mk.x, y = mk.y, h = mk.h, slot = mk.slot,
                along = along, order = mk.order}
        end
        table.sort(list, function(a, b)
            if a.along ~= b.along then return a.along < b.along end
            return a.order < b.order
        end)
        return list
    end

    -- Контрольные точки: без повторов (метка главнее) и без точек линии или сервера у самой
    -- метки — две точки в 2 м друг от друга дают излом (у метки 6 точка 25 в 2 м — поворот
    -- −113° и +87°). Отдаёт точки и направление пути в каждой.
    local function prepare(ctrl)
        local P = {}
        for _, c in ipairs(ctrl) do
            local last = P[#P]
            if last and (c.x - last.x) ^ 2 + (c.y - last.y) ^ 2 < 1 then
                if c.kind == "mark" then P[#P] = c end
            else
                P[#P + 1] = c
            end
        end
        local Q = {}
        for i, c in ipairs(P) do
            local drop = false
            if c.kind ~= "mark" then
                for _, j in ipairs({i - 1, i + 1}) do
                    local o = P[j]
                    if o and o.kind == "mark" and (o.x - c.x) ^ 2 + (o.y - c.y) ^ 2 < Path.DEDUPE ^ 2 then
                        drop = true
                    end
                end
            end
            if not drop then Q[#Q + 1] = c end
        end
        P = Q
        local m = #P
        local tx, ty, turn = {}, {}, {}
        for j = 1, m do
            local c = P[j]
            local ix, iy, ox, oy = 0, 0, 0, 0
            if j > 1 then ix, iy = unit(c.x - P[j - 1].x, c.y - P[j - 1].y) end
            if j < m then ox, oy = unit(P[j + 1].x - c.x, P[j + 1].y - c.y) end
            if j == 1 then ix, iy = ox, oy end
            if j == m then ox, oy = ix, iy end
            local th = angle(bearingTo(ox, oy) - bearingTo(ix, iy))
            turn[j] = th
            local bx, by, bl = unit(ix + ox, iy + oy)
            if bl < 0.2 then bx, by = ox, oy end
            -- Соседи ближе 12 м (метка на линии водителя) — направление задаёт сама линия: курс
            -- метки и доворот на её углу давали горб кривизны у каждой метки и руль волной.
            local lonely = (j == 1 or (c.x - P[j - 1].x) ^ 2 + (c.y - P[j - 1].y) ^ 2 > 144)
                and (j == m or (c.x - P[j + 1].x) ^ 2 + (c.y - P[j + 1].y) ^ 2 > 144)
            if c.dir then
                bx, by = c.dir[1], c.dir[2]   -- край дуги скругления: по отрезку
            elseif not lonely then
                -- по линии
            elseif c.kind == "mark" and finite(c.h) and math.abs(th) <= Path.SHARP then
                -- Пологий поворот у метки: курс, с которым её ставил водитель, если он в
                -- пределах поворота (с запасом 20°).
                local hx, hy = -math.sin(math.rad(c.h)), math.cos(math.rad(c.h))
                if math.abs(angle(bearingTo(hx, hy) - bearingTo(bx, by))) <= math.abs(th) / 2 + 20 then
                    bx, by = hx, hy
                end
            elseif c.kind == "mark" and math.abs(th) > Path.SHARP then
                -- Крутой угол у метки: центр проходим, уже повернув на половину угла.
                bx, by = rotate(ix, iy, th * Path.PHASE)
            end
            tx[j], ty[j] = bx, by
        end
        return P, tx, ty, turn
    end

    -- Кубические Эрмита через точки, выборка через step по длине дуги: put(x, y, s, j) на
    -- каждую точку (j — номер контрольной точки в начале куска). Отдаёт путь у каждой точки.
    local function sample(P, tx, ty, turn, step, put)
        local m = #P
        local S, nextAt, px, py = 0, 0, nil, nil
        local ctrlS = {}
        for j = 1, m - 1 do
            local a, b = P[j], P[j + 1]
            local _, _, d = unit(b.x - a.x, b.y - a.y)
            local m0 = math.min(d, math.abs(turn[j]) > 30 and Path.SHARPM or Path.MAXM)
            local m1 = math.min(d, math.abs(turn[j + 1]) > 30 and Path.SHARPM or Path.MAXM)
            if a.arc and a.arc == b.arc then m0, m1 = a.mag, b.mag end
            local ax, ay = tx[j] * m0, ty[j] * m0
            local bx, by = tx[j + 1] * m1, ty[j + 1] * m1
            local steps = math.max(4, math.ceil(d / 0.25))
            ctrlS[j] = S
            for k = (j == 1 and 0 or 1), steps do
                local u = k / steps
                local u2, u3 = u * u, u * u * u
                local h00, h10, h01, h11 = 2 * u3 - 3 * u2 + 1, u3 - 2 * u2 + u, -2 * u3 + 3 * u2, u3 - u2
                local x = h00 * a.x + h10 * ax + h01 * b.x + h11 * bx
                local y = h00 * a.y + h10 * ay + h01 * b.y + h11 * by
                if px then S = S + math.sqrt((x - px) ^ 2 + (y - py) ^ 2) end
                if not px or S >= nextAt then
                    put(x, y, S, j)
                    nextAt = (px and nextAt or 0) + step
                end
                px, py = x, y
            end
        end
        ctrlS[m] = S
        return ctrlS
    end

    -- Линия водителя — плотной гладкой кривой через 4 м: к метке её тянут по этим точкам, и
    -- притяжение ложится плавно (по редким точкам упрощённой линии путь у метки 5 шёл пилой).
    local function smoothLine(line)
        local ctrl = {}
        for _, g in ipairs(line) do ctrl[#ctrl + 1] = {kind = "line", slot = g[1], x = g[2], y = g[3]} end
        local P, tx, ty, turn = prepare(ctrl)
        local out = {}
        if #P < 2 then return out end
        sample(P, tx, ty, turn, 4, function(x, y, _, j) out[#out + 1] = {P[j].slot, x, y} end)
        local last = P[#P]
        out[#out + 1] = {last.slot, last.x, last.y}
        return out
    end

    -- Путь: кубические Эрмита через контрольные точки, выборка через STEP.
    local function build(ctrl)
        local P, tx, ty, turn = prepare(ctrl)
        local m = #P
        Path.ctrl = P
        Path.x, Path.y, Path.s, Path.k, Path.slot, Path.tol, Path.z = {}, {}, {}, {}, {}, {}, {}
        Path.n, Path.cursor, Path.marksS = 0, nil, {}
        if m < 2 then return end
        local n = 0
        local ctrlS = sample(P, tx, ty, turn, Path.STEP, function(x, y, S, j)
            n = n + 1
            Path.x[n], Path.y[n], Path.s[n], Path.slot[n] = x, y, S, P[j].slot or 0
        end)
        Path.n = n
        -- Кривизна по окружности через точки ±3 м (плюс — влево), потом сглаживание ±3 м.
        local raw = {}
        for i = 1, n do
            local a, b = math.max(1, i - 3), math.min(n, i + 3)
            local k = 0
            if b - a >= 2 then
                local x1, y1, x2, y2, x3, y3 = Path.x[a], Path.y[a], Path.x[i], Path.y[i], Path.x[b], Path.y[b]
                local cross = (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1)
                local d12 = math.sqrt((x2 - x1) ^ 2 + (y2 - y1) ^ 2)
                local d23 = math.sqrt((x3 - x2) ^ 2 + (y3 - y2) ^ 2)
                local d13 = math.sqrt((x3 - x1) ^ 2 + (y3 - y1) ^ 2)
                if d12 * d23 * d13 > 1e-6 then k = 2 * cross / (d12 * d23 * d13) end
            end
            raw[i] = k
        end
        for i = 1, n do
            local sum, count = 0, 0
            for j = math.max(1, i - 3), math.min(n, i + 3) do sum, count = sum + raw[j], count + 1 end
            Path.k[i] = sum / count
            Path.tol[i] = Path.TOL
        end
        -- У меток прямая к прицелу почти не срезает линию: центр метки — закон.
        local i = 1
        for j = 1, m do
            if P[j].kind == "mark" then
                local sm = ctrlS[j]
                Path.marksS[#Path.marksS + 1] = sm
                while i > 1 and Path.s[i] > sm - 6 do i = i - 1 end
                while i <= n and Path.s[i] < sm - 6 do i = i + 1 end
                local k = i
                while k <= n and Path.s[k] <= sm + 6 do Path.tol[k] = Path.TOL_MARK; k = k + 1 end
            end
        end
    end

    -- Путь под текущую цепочку: для маршрута с линией водителя — линия + метки, иначе — точки
    -- сервера (со смещением полосы), направляющие и метки цепочки.
    function Path.rebuild()
        Path.n, Path.cursor, Path.frame, Path.searchAt = 0, nil, nil, nil
        local items, count = plan.items, plan.serverCount
        if #items < 2 then return end
        local line = Path.LINE[plan.route]
        local ctrl = {}
        if line then
            local marks, inner, core = {}, {}, {}
            for order, item in ipairs(items) do
                if item.kind == "mark" then
                    local mk = {x = item.x, y = item.y, h = item.h, slot = item.slot, order = order}
                    marks[#marks + 1] = mk
                    if item.slot >= 1 and item.slot < count then inner[#inner + 1] = mk end
                end
            end
            -- Выезд из депо и въезд — по одним меткам: там рельсы, линию водителя не берём.
            for _, g in ipairs(line) do
                if g[1] >= 1 and g[1] < count then core[#core + 1] = g end
            end
            ctrl = merge(bend(smoothLine(core), inner), marks)
            -- Рельсы депо (метки до первой точки и после последней) — прямыми отрезками, как их и
            -- ведёт бот: точки через 2 м между соседними метками рельса.
            local railed = {}
            for i, c in ipairs(ctrl) do
                local prev = ctrl[i - 1]
                if prev and c.kind == "mark" and prev.kind == "mark"
                    and (c.slot == 0 or c.slot >= count) and (prev.slot == 0 or prev.slot >= count) then
                    local d = math.sqrt((c.x - prev.x) ^ 2 + (c.y - prev.y) ^ 2)
                    for q = 1, math.floor(d / 2) - 1 do
                        local f = q * 2 / d
                        railed[#railed + 1] = {kind = "rail", slot = c.slot, x = prev.x + (c.x - prev.x) * f,
                            y = prev.y + (c.y - prev.y) * f, along = 0, order = 0}
                    end
                end
                railed[#railed + 1] = c
            end
            ctrl = railed
        else
            local raw = {}
            for _, item in ipairs(items) do
                if item.kind == "server" then
                    local p = laneShifted(item.index, item, 0)
                    raw[#raw + 1] = {kind = "server", x = p.x, y = p.y, slot = item.index}
                else
                    raw[#raw + 1] = {kind = item.kind, x = item.x, y = item.y, h = item.h, slot = item.slot}
                end
            end
            -- Точку сервера на углу скругляем внутрь сферы зачёта, как самолёт — дугой, вписанной
            -- в угол (не дальше CUT от точки): через саму точку угол шёл бы радиусом 4–8 м, и бот
            -- полз бы на каждом перекрёстке. Край дуги — две точки на отрезках, курс — по отрезку.
            for i, c in ipairs(raw) do
                local a, b = raw[i - 1], raw[i + 1]
                local filleted = false
                if c.kind == "server" and a and b then
                    local ix, iy, lin = unit(c.x - a.x, c.y - a.y)
                    local ox, oy, lout = unit(b.x - c.x, b.y - c.y)
                    local th = math.rad(math.abs(angle(bearingTo(ox, oy) - bearingTo(ix, iy))))
                    if th > math.rad(4) and th < math.rad(150) and lin > 2 and lout > 2 then
                        local half = th / 2
                        local R = math.min(Path.CUT / (1 / math.cos(half) - 1), 200)
                        local T = math.min(R * math.tan(half), 0.45 * math.min(lin, lout))
                        -- Касательные точной дуги (4R·tg(θ/4)): с длиной хорды кривая выходила
                        -- площе дуги и срезала угол до 7.4 м — дальше сферы, точку не засчитывали.
                        local mag = 4 * (T / math.tan(half)) * math.tan(th / 4)
                        ctrl[#ctrl + 1] = {kind = "fillet", x = c.x - ix * T, y = c.y - iy * T,
                            slot = c.slot - 1, dir = {ix, iy}, arc = i, mag = mag}
                        ctrl[#ctrl + 1] = {kind = "fillet", x = c.x + ox * T, y = c.y + oy * T,
                            slot = c.slot, dir = {ox, oy}, arc = i, mag = mag}
                        filleted = true
                    end
                end
                if not filleted then ctrl[#ctrl + 1] = c end
            end
        end
        build(ctrl)
        emit("path_built", {route = plan.route, points = Path.n, control = #(Path.ctrl or {}),
            length = Path.n > 0 and math.floor(Path.s[Path.n]) or 0, line = line ~= nil}, true)
    end

    -- Точка пути через s метров (и номер точки перед ней).
    function Path.at(s, from)
        local i = clamp(from or 1, 1, math.max(1, Path.n - 1))
        while i < Path.n - 1 and Path.s[i + 1] < s do i = i + 1 end
        while i > 1 and Path.s[i] > s do i = i - 1 end
        local b = math.min(Path.n, i + 1)
        local span = Path.s[b] - Path.s[i]
        local t = span > 0 and clamp((s - Path.s[i]) / span, 0, 1) or 0
        return Path.x[i] + (Path.x[b] - Path.x[i]) * t, Path.y[i] + (Path.y[b] - Path.y[i]) * t, i
    end

    -- Где машина на пути: s, сдвиг вбок (плюс — вправо), курс пути. Ищем рядом с прошлым
    -- местом (путь не прыгает), а потерялись — по всему пути, но только по ходу и у своего
    -- отрезка маршрута: в депо путь выезда и въезда идут рядом.
    function Path.locate(position, heading, slotHint)
        if Path.n < 2 then return nil end
        local rad = math.rad(heading or 0)
        local fx, fy = -math.sin(rad), math.cos(rad)
        local function nearest(from, to, check)
            local best, bi
            for i = math.max(1, from), math.min(Path.n - 1, to) do
                local d = (Path.x[i] - position.x) ^ 2 + (Path.y[i] - position.y) ^ 2
                if not best or d < best then
                    local ok = true
                    if check then
                        local dx, dy = Path.x[i + 1] - Path.x[i], Path.y[i + 1] - Path.y[i]
                        ok = dx * fx + dy * fy > 0.3 * math.sqrt(dx * dx + dy * dy)
                            and (not slotHint or math.abs(Path.slot[i] - slotHint) <= 1)
                    end
                    if ok then best, bi = d, i end
                end
            end
            return bi, best
        end
        local i, d2
        if Path.cursor then i, d2 = nearest(Path.cursor - 15, Path.cursor + 120, false) end
        local now = getTickCount()
        if (not i or d2 > 15 * 15) and (not Path.searchAt or now - Path.searchAt >= 400) then
            Path.searchAt = now
            local j, e2 = nearest(1, Path.n - 1, true)
            if j and (not i or e2 < d2) then i, d2 = j, e2 end
        end
        if not i then return nil end
        Path.cursor = i
        local ax, ay = Path.x[i], Path.y[i]
        local dx, dy = Path.x[i + 1] - ax, Path.y[i + 1] - ay
        local L = math.sqrt(dx * dx + dy * dy)
        if L < 1e-6 then return Path.s[i], 0, heading, i end
        local t = clamp(((position.x - ax) * dx + (position.y - ay) * dy) / (L * L), 0, 1)
        local lateral = ((position.x - ax) * dy - (position.y - ay) * dx) / L
        return Path.s[i] + t * L, lateral, bearingTo(dx, dy), i
    end

    -- Прицел: точка пути в LEAD с пути впереди. Прямая к ней не должна срезать путь дальше
    -- допуска (у меток — 0.4 м): иначе укорачиваем, до LEAD_MIN.
    function Path.aim(s, speed)
        local L = clamp(toMs(speed or 0) * Path.LEAD, Path.LEAD_MIN, Path.LEAD_MAX)
        local x0, y0, i0 = Path.at(s, Path.cursor)
        local length = L
        while true do
            local x1, y1, i1 = Path.at(s + length, i0)
            if length <= Path.LEAD_MIN then return x1, y1, length end
            local dx, dy = x1 - x0, y1 - y0
            local chord = math.sqrt(dx * dx + dy * dy)
            local ok = true
            if chord > 0.5 then
                for j = i0 + 1, i1 do
                    if math.abs((Path.x[j] - x0) * dy - (Path.y[j] - y0) * dx) / chord > Path.tol[j] then
                        ok = false
                        break
                    end
                end
            end
            if ok then return x1, y1, length end
            length = math.max(Path.LEAD_MIN, length - 1)
        end
    end

    -- Скорость, с которой ещё можно пройти все изгибы пути впереди (с тормозным путём до них).
    function Path.speedAhead(s, speed)
        local reach = brakeDistance(speed or 0, 0) * 1.3 + 20
        local goal
        local j = Path.cursor or 1
        while j <= Path.n and Path.s[j] <= s + reach do
            local k = math.abs(Path.k[j])
            if k > 1 / 400 and Path.s[j] >= s - 2 then
                local fit = speedForRadius(0.85 / k)
                local room = math.max(0, Path.s[j] - s - 3)
                local allowed = toUnits(math.sqrt(fit * fit + 2 * DECEL * room / 1.25))
                if not goal or allowed < goal then goal = allowed end
            end
            j = j + 1
        end
        return goal
    end

    -- Кадр пути: где машина и куда пойдут лучи. Лучи помех — вдоль пути от носа, со сдвигом
    -- машины от линии, который сходит на нет за 20 м (машина вернётся на путь). Отрезки
    -- хребта сливаются, пока путь прямой: на прямой — один луч на полосу, как раньше.
    -- near — машина у пути (прицел по пути); onPath — ещё и носом по ходу (лучи вдоль пути).
    function Path.update(position, heading, slotHint, speed, shift)
        Path.frame = nil
        local s, lateral, pathHeading, i = Path.locate(position, heading, slotHint)
        if not s then return nil end
        local frame = {s = s, lateral = lateral, heading = pathHeading, i = i, shift = shift or 0}
        Path.frame = frame
        frame.near = math.abs(lateral) <= Path.OFF
        if not frame.near or math.abs(angle(pathHeading - heading)) > 60 then return frame end
        frame.onPath = true
        -- Где машина окажется вбок от пути через d метров: от нынешнего сдвига и курса (нос
        -- уже повёрнут — машина уходит вбок сразу) к сдвигу объезда за 1.5 с пути (8–20 м).
        -- Прежнее «сходит на нет за 20 м» не знало курса: при объезде стоящей машины лучи
        -- ещё видели её правым бортом, хотя нос уже увёл машину в сторону, и бот тормозил.
        local span = clamp(toMs(speed or 0) * 1.5, 8, 20)
        local slope = -math.tan(math.rad(clamp(angle(heading - pathHeading), -60, 60)))
        local goal = shift or 0
        function frame.expect(d)
            if d >= span then return goal end
            local u = math.max(0, d) / span
            local u2, u3 = u * u, u * u * u
            return (2 * u3 - 3 * u2 + 1) * lateral + (u3 - 2 * u2 + u) * slope * span
                + (-2 * u3 + 3 * u2) * goal
        end
        local reach = clamp(brakeDistance(speed or 0, 0) + 22, 18, 85)
        local nose = s + NOSE
        local points = {}
        for d = 0, reach, 5 do
            local x, y, k = Path.at(nose + d, i)
            local x2, y2 = Path.at(nose + d + 1, k)
            local ux, uy = unit(x2 - x, y2 - y)
            local off = frame.expect(d + NOSE)
            -- Вправо от пути — (uy, -ux).
            points[#points + 1] = {x + uy * off, y - ux * off, uy, -ux, d}
        end
        -- Первая точка — сам нос: лучи начинаются от бампера, как раньше.
        local r = math.rad(heading)
        local nx, ny = position.x - math.sin(r) * NOSE, position.y + math.cos(r) * NOSE
        points[1] = {nx, ny, math.cos(r), math.sin(r), 0}
        local kept = {points[1]}
        for j = 2, #points - 1 do
            local a, b, c = kept[#kept], points[j], points[j + 1]
            local h1 = bearingTo(b[1] - a[1], b[2] - a[2])
            local h2 = bearingTo(c[1] - b[1], c[2] - b[2])
            if math.abs(angle(h2 - h1)) > 2.5 then kept[#kept + 1] = b end
        end
        if #points > 1 then kept[#kept + 1] = points[#points] end
        frame.spine = kept
        frame.reach = reach
        return frame
    end

    -- Где точка относительно будущего пути машины: сколько метров вперёд от носа и сколько
    -- вбок (плюс — вправо). Нужна памяти помех и «протиснуться»: по дуге руля в повороте туда
    -- попадали столбы и стены снаружи, мимо которых путь не идёт.
    function Path.localOf(x, y)
        local f = Path.frame
        if not f or not f.onPath then return nil end
        local best, bi
        for j = math.max(1, f.i - 8), math.min(Path.n - 1, f.i + 100) do
            local d = (Path.x[j] - x) ^ 2 + (Path.y[j] - y) ^ 2
            if not best or d < best then best, bi = d, j end
        end
        if not bi or best > 30 * 30 then return nil end
        local ax, ay = Path.x[bi], Path.y[bi]
        local dx, dy = Path.x[bi + 1] - ax, Path.y[bi + 1] - ay
        local L = math.sqrt(dx * dx + dy * dy)
        if L < 1e-6 then return nil end
        local t = ((x - ax) * dx + (y - ay) * dy) / (L * L)
        local ahead = Path.s[bi] + t * L - (f.s + NOSE)
        local side = ((x - ax) * dy - (y - ay) * dx) / L
        return ahead, side - f.expect(math.max(0, ahead) + NOSE)
    end

    -- Место помехи для памяти и «протиснуться»: по пути, если едем по нему, иначе — по дуге руля.
    -- Помеха в стороне от пути дальше 30 м — не наша.
    function Path.place(nose, heading, curvature, x, y)
        local f = Path.frame
        if f and f.onPath then
            local ahead, side = Path.localOf(x, y)
            if ahead then return ahead, side end
            return -1e9, 1e9
        end
        return arcLocal(nose, heading, curvature, x, y)
    end

    -- Годится ли поза для выхода из тупика: на дороге (не дальше 2.5 м вправо от пути — там
    -- тротуар, не дальше 5 м влево — это уже за встречной) и носом по ходу. Отдаёт штраф.
    function Path.judge(x, y, h)
        if Path.n < 2 or not Path.frame then return true, 0 end
        local f = Path.frame
        local best, bi
        for j = math.max(1, f.i - 30), math.min(Path.n - 1, f.i + 60) do
            local d = (Path.x[j] - x) ^ 2 + (Path.y[j] - y) ^ 2
            if not best or d < best then best, bi = d, j end
        end
        if not bi then return true, 0 end
        local dx, dy = Path.x[bi + 1] - Path.x[bi], Path.y[bi + 1] - Path.y[bi]
        local L = math.sqrt(dx * dx + dy * dy)
        if L < 1e-6 then return true, 0 end
        local side = ((x - Path.x[bi]) * dy - (y - Path.y[bi]) * dx) / L
        local turn = math.abs(angle(h - bearingTo(dx, dy)))
        local ok = side <= 2.5 and side >= -5 and turn <= 75
        return ok, math.abs(side) * 3 + turn / 3
    end
end

local function forwardGap(vehicle, position, heading, speed, shift, curvature, spine)
    -- Видеть надо дальше тормозного пути: на 90 км/ч он 62 м, а прежние 55 м обзора
    -- означали, что остановиться перед увиденным уже физически невозможно.
    local reach = clamp(brakeDistance(speed, 0) + 22, 18, 85)
    curvature = finite(curvature) and curvature or 0
    local segments = math.abs(curvature) < ARC_STRAIGHT and 1 or ARC_SEGMENTS
    local sz = position.z - 0.3
    local nose = noseOf(position, heading)
    local best, bestVehicle, bestX, bestY, bestZ, bestLane, bestElement
    local central   -- ближайшее попадание центральных лучей (по оси машины ±0.7 м)
    -- Вдоль пути: хребет от носа (Path.update), на каждой точке — своя нормаль.
    for _, side in ipairs(spine and #spine >= 2 and rayOffsets() or {}) do
        local lane = side + (finite(shift) and shift or 0)
        local walked = 0
        for k = 2, #spine do
            local a, b = spine[k - 1], spine[k]
            local ax, ay = a[1] + a[3] * lane, a[2] + a[4] * lane
            local bx, by = b[1] + b[3] * lane, b[2] + b[4] * lane
            local hit, hx, hy, hz, element = read("processLineOfSight", ax, ay, sz, bx, by, sz,
                true, true, true, true, false, false, false, false, vehicle)
            if hit == true and finite(hx) and finite(hy) then
                local gap = walked + math.sqrt((hx - ax) ^ 2 + (hy - ay) ^ 2)
                if math.abs(side) <= 0.7 and (not central or gap < central) then central = gap end
                if not best or gap < best then
                    best, bestX, bestY, bestZ, bestLane = gap, hx, hy, finite(hz) and hz or sz, lane
                    bestElement = element
                end
                break
            end
            walked = walked + math.sqrt((bx - ax) ^ 2 + (by - ay) ^ 2)
        end
    end
    for _, side in ipairs(not (spine and #spine >= 2) and rayOffsets() or {}) do
        local lane = side + (finite(shift) and shift or 0)
        for k = 1, segments do
            local s0 = reach * (k - 1) / segments
            local s1 = reach * k / segments
            local x0, y0, h0 = arcPose(nose, heading, curvature, s0)
            local x1, y1, h1 = arcPose(nose, heading, curvature, s1)
            local r0, r1 = math.rad(h0), math.rad(h1)
            local ax, ay = x0 + math.cos(r0) * lane, y0 + math.sin(r0) * lane
            local bx, by = x1 + math.cos(r1) * lane, y1 + math.sin(r1) * lane
            local hit, hx, hy, hz, element = read("processLineOfSight", ax, ay, sz, bx, by, sz,
                true, true, true, true, false, false, false, false, vehicle)
            if hit == true and finite(hx) and finite(hy) then
                local gap = s0 + math.sqrt((hx - ax) ^ 2 + (hy - ay) ^ 2)
                if math.abs(side) <= 0.7 and (not central or gap < central) then central = gap end
                if not best or gap < best then
                    best, bestX, bestY, bestZ, bestLane = gap, hx, hy, finite(hz) and hz or sz, lane
                    bestElement = element
                end
                break
            end
        end
    end
    -- Что за помеха: машина, человек или всё остальное (столб, стена, забор).
    local kind = "static"
    if best and bestElement ~= nil then
        local elementType = read("getElementType", bestElement)
        if elementType == "vehicle" then kind = "vehicle"
        elseif elementType == "ped" or elementType == "player" then kind = "ped" end
    end
    bestVehicle = kind == "vehicle"
    return best, bestVehicle, bestX, bestY, bestZ, bestLane, kind, bestElement, central
end

-- Машины у самого бампера. Основные лучи начинаются у носа, и то, что уже вплотную, они
-- не видят: у ворот депо друг встал перед носом, а бот упирался и толкал его. Низкие
-- лучи из кабины (свою машину луч пропускает) и только по машинам и людям, чтобы бордюр
-- и асфальт их не задевали.
local function nearVehicleGap(vehicle, position, heading)
    local rad = math.rad(heading)
    local fx, fy = -math.sin(rad), math.cos(rad)
    local rx, ry = fy, -fx
    local sz = position.z - 0.75
    local best, bestLane, bestElement
    for _, lane in ipairs({-1.0, 0, 1.0}) do
        local ax = position.x + fx * (NOSE - 2) + rx * lane
        local ay = position.y + fy * (NOSE - 2) + ry * lane
        local hit, hx, hy, _, element = read("processLineOfSight", ax, ay, sz, ax + fx * 14,
            ay + fy * 14, sz, false, true, true, false, false, false, false, false, vehicle)
        if hit == true and finite(hx) and finite(hy) then
            local gap = math.max(0, math.sqrt((hx - ax) ^ 2 + (hy - ay) ^ 2) - 2)
            if not best or gap < best then best, bestLane, bestElement = gap, lane, element end
        end
    end
    local elementType = bestElement ~= nil and read("getElementType", bestElement) or nil
    local kind = (elementType == "ped" or elementType == "player") and "ped" or "vehicle"
    return best, bestLane, kind, bestElement
end

-- === память помех ===
-- Тонкий столб луч ловит через раз: в заезде перед меткой 4 помеха мигала 9.9 → чисто →
-- 12.9 → чисто → 7.3 → чисто, и между попаданиями бот снова разгонялся — удар силой 822.
-- Поэтому неподвижные помехи запоминаются на всю смену, а каждый кадр проверяется, не
-- лежит ли какая-то из них в коридоре машины впереди.
K.OBSTACLE_SAME = 1.5   -- ближе этого — та же помеха
K.OBSTACLE_HEIGHT = 0.5 -- помеха должна стоять над землёй: на подъёме луч бьёт в дорогу
local CORRIDOR = 1.7        -- полуширина коридора: кузов 1.25 м + запас
local OBSTACLES = {}
-- Известные помехи: пойманы лучом бота в заезде 28.09.2026 и вшиты, как светофоры и
-- метки, — иначе бот узнаёт о них, только подъехав. Ворота выезда из депо маршрута 1:
-- будка справа по ходу и столбик слева, 4.7 м между ними.
local KNOWN_OBSTACLES = {
    {2190.10, 2437.96}, {2192.63, 2436.05}, {2191.85, 2432.82},   -- будка
    {2195.14, 2429.47},                                           -- столбик
}
for _, o in ipairs(KNOWN_OBSTACLES) do OBSTACLES[#OBSTACLES + 1] = {o[1], o[2], 0} end

local function rememberObstacle(x, y, z, now)
    local ground = read("getGroundPosition", x, y, z + 1.5)
    if finite(ground) and z - ground < K.OBSTACLE_HEIGHT then return false end
    for _, o in ipairs(OBSTACLES) do
        if (o[1] - x) ^ 2 + (o[2] - y) ^ 2 < K.OBSTACLE_SAME * K.OBSTACLE_SAME then
            o[3] = now
            return false
        end
    end
    -- Вытесняем самую старую выученную, вшитые не трогаем.
    if #OBSTACLES >= 800 then table.remove(OBSTACLES, #KNOWN_OBSTACLES + 1) end
    OBSTACLES[#OBSTACLES + 1] = {x, y, now}
    return true
end

local function rememberedGap(position, heading, reach, curvature, skip)
    local best, bestSide, bestX, bestY
    local nose = noseOf(position, heading)
    local far = (reach + 10) ^ 2
    for _, o in ipairs(OBSTACLES) do
        local ahead, side = -1, 99
        if (o[1] - nose.x) ^ 2 + (o[2] - nose.y) ^ 2 < far then
            ahead, side = Path.place(nose, heading, curvature, o[1], o[2])
        end
        if ahead > 0 and ahead < reach and math.abs(side) < CORRIDOR
            and not (skip and skip(o[1], o[2])) then
            local gap = ahead
            if not best or gap < best then best, bestSide, bestX, bestY = gap, side, o[1], o[2] end
        end
    end
    return best, bestSide, bestX, bestY
end

-- Протиснуться мимо помех: сдвиг прицела (плюс — вправо), чтобы кузов прошёл мимо
-- запомненных помех впереди с запасом. Это поправка внутри полосы, а не перестроение,
-- поэтому работает и к своим меткам: в воротах депо линия между метками 2 и 3 идёт
-- в 1.3 м от угла будки, а водитель проходит там на 1–1.5 м левее.
local SQUEEZE_MAX = 1.5     -- больше не сдвигаемся: дальше уже объезд
local SQUEEZE_CLEAR = 1.75  -- полуширина кузова 1.25 + запас 0.5
K.SQUEEZE_REACH = 20    -- на столько метров впереди смотрим
-- most — сколько можно сдвигаться: 1.5 м в полосе, а к своей метке — в пределах её зоны.
local function squeezeShift(position, heading, curvature, most, skip)
    most = finite(most) and most or SQUEEZE_MAX
    local lo, hi, seen = -most, most, false
    local nose = noseOf(position, heading)
    local far = (K.SQUEEZE_REACH + 10) ^ 2
    for _, o in ipairs(OBSTACLES) do
        local ahead, side = -9, 99
        if (o[1] - nose.x) ^ 2 + (o[2] - nose.y) ^ 2 < far then
            ahead, side = Path.place(nose, heading, curvature, o[1], o[2])
        end
        if ahead > -1 and ahead < K.SQUEEZE_REACH
            and math.abs(side) < SQUEEZE_CLEAR + most
            and not (skip and skip(o[1], o[2])) then
            seen = true
            if side >= 0 then hi = math.min(hi, side - SQUEEZE_CLEAR)
            else lo = math.max(lo, side + SQUEEZE_CLEAR) end
        end
    end
    if not seen then return 0 end
    -- Помехи с обеих сторон и щель уже машины — держимся посередине.
    if lo > hi then return clamp((lo + hi) / 2, -most, most) end
    return clamp(0, lo, hi)
end

-- То же для рельса меток выезда, но сдвиг считается от самого отрезка между метками, а
-- не от машины: отрезок неподвижен, поэтому сдвиг не качается вслед за машиной. Отрезок
-- метка 2 → метка 3 идёт в 1.3 м от угла будки; кузову нужно 1.75.
K.RAIL_SHIFT_RAMP = 8   -- м от начала отрезка до полного сдвига
local function railShift(leg, position)
    local lx, ly = leg[3] - leg[1], leg[4] - leg[2]
    local length = math.sqrt(lx * lx + ly * ly)
    if length < 0.5 then return 0 end
    lx, ly = lx / length, ly / length
    local carAlong = (position.x - leg[1]) * lx + (position.y - leg[2]) * ly
    -- Сдвигаться можно в пределах зоны метки, куда ведёт отрезок: сделал зону больше —
    -- у бота больше места выбрать, ехать по центру или у края.
    local most = math.max(SQUEEZE_MAX, finite(leg[5]) and leg[5] or 0)
    local lo, hi, left, right = -most, most, false, false
    for _, o in ipairs(OBSTACLES) do
        local dx, dy = o[1] - leg[1], o[2] - leg[2]
        local along = dx * lx + dy * ly
        local side = dx * ly - dy * lx
        if along > carAlong - 3 and along < length + 5
            and math.abs(side) < SQUEEZE_CLEAR + most then
            if side >= 0 then hi, right = math.min(hi, side - SQUEEZE_CLEAR), true
            else lo, left = math.max(lo, side + SQUEEZE_CLEAR), true end
        end
    end
    -- Помехи с обеих сторон (ворота) — рельс ставим посередине щели: машина выходит на
    -- сдвиг не сразу, и минимальный сдвиг оставлял у будки 0.15 м.
    if left and right then return clamp((lo + hi) / 2, -most, most) end
    if not (left or right) then return 0 end
    return clamp(0, lo, hi)
end

-- Попадание луча ниже полуметра над землёй — бордюр или подъём дороги, не стена.
local function lowHit(x, y, z)
    local ground = read("getGroundPosition", x, y, z + 1.5)
    return finite(ground) and z - ground < K.OBSTACLE_HEIGHT
end

-- === умные помехи и выход из тупика ===
local Smart = {STEERS = {-1, -0.5, 0, 0.5, 1}}
-- round объявлен ниже по файлу — здесь свой.
function Smart.round(v, places)
    local m = 10 ^ (places or 2)
    return math.floor(v * m + 0.5) / m
end

-- Машина впереди: едет (поток) — едем за ней; стоит с водителем (очередь, светофор) — ждём;
-- пустая и не двигается — брошена, объезжаем как столб. С водителем, но 20 с ни с места
-- (игрок отошёл от клавиатуры посреди дороги) — тоже брошена. Постоять 5 с и посмотреть,
-- сдвинется ли, — мало: пустую видно сразу по водителю, а очередь на светофоре стоит и 60 с.
function Smart.vehicleKind(element, now)
    local vx, vy = read("getElementVelocity", element)
    local moving = finite(vx) and finite(vy) and math.sqrt(vx * vx + vy * vy) * 50 > 1.0
    local x, y = read("getElementPosition", element)
    local watch = state.vehicleWatch
    if moving or not watch or watch.element ~= element or not finite(x)
        or (x - watch.x) ^ 2 + (y - watch.y) ^ 2 > 1 then
        state.vehicleWatch = {element = element, since = now, x = x or 0, y = y or 0}
        if moving then return "traffic" end
        watch = state.vehicleWatch
    end
    local still = now - watch.since
    local driver = read("getVehicleController", element)
    local occupied = driver ~= nil and driver ~= false
    -- Пустая сама не поедет — брошена сразу, объезд начинается издалека. С водителем —
    -- через 20 с без движения.
    if not occupied then return "parked" end
    -- С водителем и 15 с ни с места (светофоры работают — 35 с: полный цикл) — «застряла».
    -- 5 с было мало: в заезде 2.10.3 очередь на красный у точки 39 через 5 с стала «брошенной»,
    -- бот пошёл в объезд и полторы минуты катался по траве, встречке и остановке.
    if still >= (trafficWorking() and 35000 or 15000) then return "stalled" end
    return "queue"
end

-- Кривизна дуги при руле sigma на малой скорости (обратная к steerForCurvature).
function Smart.curvature(sigma)
    return (sigma >= 0 and 1 or -1) * sigma * sigma / minRadius(2)
end

function Smart.rearOf(position, heading)
    local rad = math.rad(heading)
    return {x = position.x + math.sin(rad) * NOSE, y = position.y - math.cos(rad) * NOSE,
        z = position.z}
end

-- Сколько метров свободно по дуге от точки start (нос или задний бампер), три луча по
-- ширине кузова. Бордюр (низкое попадание) не помеха.
function Smart.arcFree(vehicle, start, heading, curvature, length, backward)
    local sz = start.z - 0.3
    local free = length
    for _, lane in ipairs({-1.25, 0, 1.25}) do
        for k = 1, 3 do
            local s0, s1 = length * (k - 1) / 3, length * k / 3
            if backward then s0, s1 = -s0, -s1 end
            local x0, y0, h0 = arcPose(start, heading, curvature, s0)
            local x1, y1, h1 = arcPose(start, heading, curvature, s1)
            local r0, r1 = math.rad(h0), math.rad(h1)
            local ax, ay = x0 + math.cos(r0) * lane, y0 + math.sin(r0) * lane
            local bx, by = x1 + math.cos(r1) * lane, y1 + math.sin(r1) * lane
            local hit, hx, hy, hz = read("processLineOfSight", ax, ay, sz, bx, by, sz,
                true, true, true, true, false, false, false, false, vehicle)
            if hit == true and finite(hx) and finite(hy)
                and not lowHit(hx, hy, finite(hz) and hz or sz) then
                free = math.min(free, math.abs(s0) + math.sqrt((hx - ax) ^ 2 + (hy - ay) ^ 2))
                break
            end
        end
    end
    return free
end

-- Сколько метров машина проедет по дуге кривизны k (назад — backward), пока какой-нибудь
-- угол кузова не упрётся. Поза машины считается по шагам 2 м, и лучи идут по пути каждого
-- из четырёх углов: при откате с вывернутым рулём задний угол заносит шире, чем путь
-- центра бампера, — в тесте так правый задний угол задел ряд машин.
-- Ведущий край — пять лучей через 0.6 м (машина уже кузова: по одним углам луч проходил
-- мимо неё), отстающие углы — по лучу: их заносит внутрь поворота. Тонкий столб между
-- лучами ловит память помех.
-- С запасом 25 см по ширине и 20 см по длине: впритык скан однажды выбрал проезд в 27 см
-- от чужого бампера.
Smart.FRONT = {{4.4, -1.5}, {4.4, -0.75}, {4.4, 0}, {4.4, 0.75}, {4.4, 1.5}, {-4.2, -1.5}, {-4.2, 1.5}}
Smart.BACK = {{-4.4, -1.5}, {-4.4, -0.75}, {-4.4, 0}, {-4.4, 0.75}, {-4.4, 1.5}, {4.2, -1.5}, {4.2, 1.5}}
function Smart.remembered(x, y, h)
    local r = math.rad(h)
    local fx, fy = -math.sin(r), math.cos(r)
    for _, o in ipairs(OBSTACLES) do
        local dx, dy = o[1] - x, o[2] - y
        -- Запас больше, чем у лучей: у запомненной помехи неизвестна толщина, а в тесте столб
        -- в 1.57 м от оси проходил проверку «мимо», хотя кузов цеплял бы его; в 1.87 м — впритык,
        -- и после выхода бот доворачивал в него.
        if dx * dx + dy * dy < 49 and math.abs(dx * fx + dy * fy) < 4.6
            and math.abs(dx * fy - dy * fx) < 1.9 then
            return true
        end
    end
    return false
end
function Smart.sweepFree(vehicle, position, heading, k, length, backward)
    local sz = position.z - 0.3
    local px, py, ph, done = position.x, position.y, heading, 0
    while done < length do
        local s = math.min(length, done + 2)
        local x, y, h = arcPose(position, heading, k, backward and -s or s)
        if Smart.remembered(x, y, h) then return done end
        for _, c in ipairs(backward and Smart.BACK or Smart.FRONT) do
            local r0, r1 = math.rad(ph), math.rad(h)
            local ax = px - math.sin(r0) * c[1] + math.cos(r0) * c[2]
            local ay = py + math.cos(r0) * c[1] + math.sin(r0) * c[2]
            local bx = x - math.sin(r1) * c[1] + math.cos(r1) * c[2]
            local by = y + math.cos(r1) * c[1] + math.sin(r1) * c[2]
            local hit, hx, hy, hz = read("processLineOfSight", ax, ay, sz, bx, by, sz,
                true, true, true, true, false, false, false, false, vehicle)
            if hit == true and finite(hx) and finite(hy)
                and not lowHit(hx, hy, finite(hz) and hz or sz) then
                local whole = math.sqrt((bx - ax) ^ 2 + (by - ay) ^ 2)
                local part = whole > 0.01 and math.sqrt((hx - ax) ^ 2 + (hy - ay) ^ 2) / whole or 0
                return done + (s - done) * part
            end
        end
        px, py, ph, done = x, y, h, s
    end
    return length
end

-- Скан выхода. Выход — дуга, свободная на 14 м: короче мало, в тесте «прямо на 10 м
-- свободно» вело обратно в ту же машину, и бот катался туда-сюда. Сначала пять дуг вперёд
-- от носа: есть выход — объезжаем без отката. Нет — пять дуг назад на две глубины, и из
-- каждого конца три дуги вперёд; берём самый короткий откат, после которого есть выход и курс
-- ближе к цели. Выхода нигде нет — откатываемся туда, где впереди свободнее всего, и в
-- следующий раз на том же месте смотрим глубже (до 14 м). Около 300 лучей один раз на тупик.
function Smart.scan(err)
    local vehicle, position, heading = Smart.vehicle, Smart.position, Smart.heading
    if not position or not finite(heading) then return nil end
    local want = heading + (finite(err) and err or 0)
    local last = Smart.lastScan
    local tries = (last and (last.x - position.x) ^ 2 + (last.y - position.y) ^ 2 < 25) and last.tries + 1 or 0
    Smart.lastScan = {x = position.x, y = position.y, tries = tries}
    local function turnLeft(h, k) return math.abs(angle(want - h - math.deg(k * 10))) end
    -- Выход должен остаться на дороге: конец дуги не дальше 2.5 м вправо от пути (тротуар) и
    -- 5 м влево (за встречной) и носом по ходу (Path.judge). В заезде 2.10.3 скан выбирал дуги
    -- с рулём в упор — в траву, забор, на встречку и в остановку. Дуга не на дороге — запасная.
    local best, loose
    for _, sigma in ipairs(Smart.STEERS) do
        local k = Smart.curvature(sigma)
        if Smart.sweepFree(vehicle, position, heading, k, 14, false) >= 13 then
            -- Поза — через 7 м: столько длится выезд, дальше машину ведёт путь.
            local ex, ey, eh = arcPose(position, heading, k, 7)
            local ok, penalty = Path.judge(ex, ey, eh)
            local score = -turnLeft(heading, k) - penalty
            local candidate = {mode = "forward", steer = sigma, score = score}
            if ok and (not best or score > best.score) then best = candidate end
            if not loose or score > loose.score then loose = candidate end
        end
    end
    if best then
        best.side = best.steer > 0.2 and -1 or (best.steer < -0.2 and 1 or 0)
        emit("escape_plan", {mode = best.mode, steer = best.steer, tries = tries,
            x = Smart.round(position.x), y = Smart.round(position.y), heading = Smart.round(heading, 1)}, true)
        return best
    end
    local most = math.min(6 + 4 * tries, 14)
    for _, sigma in ipairs(Smart.STEERS) do
        local k = Smart.curvature(sigma)
        local back = Smart.sweepFree(vehicle, position, heading, k, most + 1, true)
        if back >= 2.5 then
            local deepest = math.min(back - 1, most)
            for _, s in ipairs({deepest / 2, deepest}) do
                local px, py, ph = arcPose(position, heading, k, -s)
                local pose = {x = px, y = py, z = position.z}
                for _, forward in ipairs({-1, 0, 1}) do
                    local fk = Smart.curvature(forward)
                    local free = Smart.sweepFree(vehicle, pose, ph, fk, 14, false)
                    local score = free >= 13 and (100 - turnLeft(ph, fk) / 3 - s)
                        or (free + s * 0.3 - turnLeft(ph, fk) / 10)
                    -- И поза после отката, и поза через 7 м выезда — на дороге и носом по ходу:
                    -- откат на 10 м с рулём в упор разворачивал машину поперёк улицы.
                    local ex, ey, eh = arcPose(pose, ph, fk, 7)
                    local ok, penalty = Path.judge(ex, ey, eh)
                    local backOk, backPenalty = Path.judge(px, py, ph)
                    ok = ok and backOk
                    score = score - (penalty + backPenalty) / 3 - (ok and 0 or 200)
                    if not best or score > best.score then
                        best = {mode = "reverse", steer = sigma, back = s, forward = forward, score = score,
                            offroad = not ok}
                    end
                end
            end
        end
    end
    -- Назад выхода на дорогу нет, а вперёд есть хоть какой-то — лучше вперёд.
    if loose and (not best or best.offroad) then
        best = loose
        best.side = best.steer > 0.2 and -1 or (best.steer < -0.2 and 1 or 0)
        emit("escape_plan", {mode = best.mode, steer = best.steer, tries = tries, offroad = true,
            x = Smart.round(position.x), y = Smart.round(position.y), heading = Smart.round(heading, 1)}, true)
        return best
    end
    if not best then
        emit("escape_plan", {mode = "boxed", tries = tries, x = Smart.round(position.x),
            y = Smart.round(position.y)}, true)
        return {mode = "boxed", tries = tries}
    end
    best.side = best.forward > 0.2 and -1 or (best.forward < -0.2 and 1 or 0)
    best.ms = clamp(best.back / 2 * 1000 + 800, 1000, 6000)
    best.tries = tries
    emit("escape_plan", {mode = best.mode, steer = best.steer, back = Smart.round(best.back, 1),
        forward = best.forward, score = Smart.round(best.score, 1), tries = tries,
        x = Smart.round(position.x), y = Smart.round(position.y), heading = Smart.round(heading, 1)}, true)
    return best
end

-- Боковые лучи. Все три удара за заезд были боковыми: руль вывернут, впереди чисто,
-- а корпус на повороте заметает вбок и цепляет столб. Передние лучи такое не видят.
local HALF_WIDTH = 1.25
local function sideGap(vehicle, position, heading, side)
    local rad = math.rad(heading)
    local fx, fy = -math.sin(rad), math.cos(rad)
    local rx, ry = fy, -fx
    local sx = position.x + rx * HALF_WIDTH * side
    local sy = position.y + ry * HALF_WIDTH * side
    local sz = position.z - 0.3
    local best
    -- Один луч поперёк борта, второй вперёд-вбок: туда уходит нос в повороте.
    for _, probe in ipairs({{0, 3.2}, {45, 7.0}, {20, 9.0}}) do
        local angleRad = rad + math.rad(probe[1] * side * -1)
        local dx, dy = -math.sin(angleRad), math.cos(angleRad)
        if probe[1] == 0 then dx, dy = rx * side, ry * side end
        local ex, ey = sx + dx * probe[2], sy + dy * probe[2]
        local hit, hx, hy = read("processLineOfSight", sx, sy, sz, ex, ey, sz,
            true, true, true, true, false, false, false, false, vehicle)
        if hit == true and finite(hx) and finite(hy) then
            local gap = math.sqrt((hx - sx) ^ 2 + (hy - sy) ^ 2)
            if not best or gap < best then best = gap end
        end
    end
    return best
end

local analogNames = {"accelerate", "brake_reverse", "vehicle_left", "vehicle_right"}

local function releaseControls()
    local failed = {}
    for _, name in ipairs(analogNames) do
        if read("setAnalogControlState", name) ~= true then
            failed[#failed + 1] = name
        end
    end
    read("setPedControlState", localPlayer, "handbrake", false)
    state.owned = false
    return failed
end

local function applyControls(out)
    -- Знак руля: курс в MTA растёт ПРОТИВ часовой (0 — север, 90 — запад), поэтому
    -- положительная ошибка курса означает поворот ВЛЕВО. Положительный steer идёт
    -- в vehicle_left. Ошибка в этом месте уводила бота вправо от первой же точки.
    local values = {out.throttle, out.brake,
        math.max(0, out.steer), math.max(0, -out.steer)}
    -- MTA гасит противоположную сторону даже при записи нуля: активную пишем последней.
    local order = {1, 2, out.steer > 0 and 4 or 3, out.steer > 0 and 3 or 4}
    for _, i in ipairs(order) do
        if read("setAnalogControlState", analogNames[i], values[i], true) ~= true then
            return false, analogNames[i]
        end
    end
    if read("setPedControlState", localPlayer, "handbrake", out.handbrake == true) ~= true then
        return false, "handbrake"
    end
    state.owned = true
    return true
end

-- === установка полива ===

local SPRAY_KEY = "1"

local function pressSprayKey(now)
    if type(native.key) ~= "function" then return false end
    if read("dfEmulateKey", SPRAY_KEY, true) ~= true then return false end
    state.sprayKeyDown = now
    return true
end

local function updateSprayKey(now)
    if state.sprayKeyDown and elapsed(now, state.sprayKeyDown) >= 60 then
        if read("dfEmulateKey", SPRAY_KEY, false) == true then state.sprayKeyDown = nil end
    end
end

-- Приводит установку к нужному состоянию: клавиша «1» переключает, поэтому жмём её
-- только когда наблюдаемое состояние расходится с желаемым, и не чаще раза в 1.2 с.
local function applySpray(vehicle, want, now)
    if not vehicle or want == nil then return end
    local current = sprayOn(vehicle)
    if current == want then state.sprayWanted = nil; return end
    if state.sprayWanted and elapsed(now, state.sprayWanted) < 1200 then return end
    state.sprayWanted = now
    if pressSprayKey(now) then
        emit("spray_request", {want = want, observed = current}, true)
    end
end

-- База локации: место, где выдают машину и где у НПС берут маршрут. Именно туда бот
-- возвращается после финиша — новый маршрут (и полный бак) дают только там.
locationOfRoute = function(id)
    if type(_G.tCities) ~= "table" or not id then return nil end
    for location, routes in pairs(_G.tCities) do
        if type(routes) == "table" then
            for _, routeId in ipairs(routes) do
                if routeId == id then return location end
            end
        end
    end
    return nil
end

homePoint = function(location, position)
    if type(_G.tCitySpawnPos) ~= "table" then return nil end
    local lists = {}
    if location and type(_G.tCitySpawnPos[location]) == "table" then
        lists[1] = _G.tCitySpawnPos[location]
    else
        -- Локация неизвестна: берём ближайшую площадку из всех, какие есть.
        for _, list in pairs(_G.tCitySpawnPos) do
            if type(list) == "table" then lists[#lists + 1] = list end
        end
    end
    local best, bestDistance
    for _, list in ipairs(lists) do
        for _, spot in ipairs(list) do
            if type(spot) == "table" and finite(spot[1]) and finite(spot[2]) then
                local point = {x = spot[1], y = spot[2], z = finite(spot[3]) and spot[3] or 0}
                if not position then return point end
                local distance = math.sqrt((point.x - position.x) ^ 2
                    + (point.y - position.y) ^ 2)
                if not bestDistance or distance < bestDistance then
                    best, bestDistance = point, distance
                end
            end
        end
    end
    return best, bestDistance
end

-- Суффиксные суммы по точкам маршрута: сколько метров от точки i до финиша.
-- Считаются один раз на маршрут, иначе это было бы 54 корня каждый кадр.
local function buildSuffix()
    state.suffix = nil
    local points = routePoints()
    if not points or #points == 0 then return end
    local suffix = {[#points] = 0}
    for i = #points - 1, 1, -1 do
        local a, b = pointAt(i), pointAt(i + 1)
        if not a or not b then return end
        suffix[i] = suffix[i + 1] + math.sqrt((b.x - a.x) ^ 2 + (b.y - a.y) ^ 2)
    end
    state.suffix = suffix
end

-- === команды панели ===

local function trigger(event, ...)
    local ok = read("triggerServerEvent", event, resourceRoot, ...)
    emit("server_event", {event = event, sent = ok == true}, true)
    return ok == true
end

local function stopBot(reason)
    if state.bot then
        state.bot = false
        releaseControls()
        controller:reset()
        note("Бот остановлен: " .. reason)
        -- Запись НЕ трогаем: она должна писать и ручную езду тоже, пока водитель
        -- сам её не выключит. Перехват управления — часть той же записи.
        if state.recording then
            emit("bot_stopped", {reason = reason, samples = state.samples or 0}, true)
        end
    end
end

local function startBot()
    if state.bot then return end
    controller:reset()
    state.bot = true
    state.tripStart = getTickCount()
    -- Скорость выезда из депо — случайная в 15–20 на каждый рейс.
    state.depotSpeed = 15 + math.random() * 5
    -- Проезд начинаем с чистого листа: отметки «задета/пропущена» от прошлых поездок и
    -- от разметки не должны закрывать метки. Прошедшие участки цепочка закроет сама.
    state.finished = nil
    for _, item in ipairs(plan.items) do item.done, item.missed = nil, nil end
    local vehicle = jobVehicle()
    local sx, sy
    -- Через "and" нельзя: множественный возврат усечётся до одного значения.
    if vehicle then sx, sy = read("getElementPosition", vehicle) end
    state.waypointCursor = waypointStart(finite(sx) and finite(sy) and {x = sx, y = sy} or nil)
    -- Запись включается сама: разбирать потом нечего, если бот ехал без телеметрии.
    if not state.recording then
        state.recording = true
        state.autoRecording = true
        state.recordedRoute = nil
        state.samples, state.checkpointsSeen = 0, 0
        emit("record_begin", {version = VERSION, route = routeId() or NULL,
            auto = true, in_vehicle = occupied() ~= nil}, true)
    end
    note("Бот запущен, запись включена")
end

local function selectRoute(id)
    if not finite(id) or id < 1 or id > 9 then return end
    local score, why = routeScore(id)
    if not score and why then note("Маршрут " .. id .. ": " .. why) end
    trigger("snowPlow:npc:route:select", id)
    note("Запрошен маршрут " .. id)
end

local function handleCommand(command, now)
    if type(command) ~= "string" then return end
    local name, argument = command:match("^([%w_]+):?(.*)$")
    name = name or command
    emit("command", {command = command}, true)
    if name == "bot_start" then startBot()
    elseif name == "bot_stop" then stopBot("команда панели")
    elseif name == "admin" then
        read("outputChatBox", "#00FF00[ТРЕВОГА] Админ крикнул: #FF6600" .. argument, 255, 255, 255, true)
        emit("safety_alert", {kind = "admin_caption", text = argument}, true)
        note("Админ крикнул: " .. argument)
    elseif name == "autonomy" then
        state.autonomy = argument == "1"
        note(state.autonomy and "Автономность включена" or "Автономность выключена")
    elseif name == "record_start" then
        state.recording = true
        -- Включённую руками запись бот сам не гасит.
        state.autoRecording = nil
        -- Запись включают уже сидя в машине с назначенным маршрутом, поэтому список
        -- точек нужно записать заново, иначе эталонный проезд останется без трассы.
        state.recordedRoute = nil
        state.samples, state.checkpointsSeen = 0, 0
        emit("record_begin", {version = VERSION, route = routeId() or NULL,
            in_vehicle = occupied() ~= nil, spray = jobVehicle()
                and sprayOn(jobVehicle()) or false}, true)
        note("Запись лога включена")
    elseif name == "record_stop" then
        emit("record_end", {samples = state.samples or 0,
            checkpoints = state.checkpointsSeen or 0, route = routeId() or NULL}, true)
        state.recording = false
        flush(true)
        note(string.format("Запись выключена: %d сэмплов, точек %d",
            state.samples or 0, state.checkpointsSeen or 0))
    elseif name == "accept_job" then
        trigger("snowPlow:npc:start")
        note("Запрошено трудоустройство")
    elseif name == "quit_job" then
        stopBot("увольнение")
        trigger("snowPlow:npc:stop")
        note("Запрошено увольнение")
    elseif name == "rent_start" then
        trigger("snowPlow:npc:rent:start")
        note("Запрошена машина")
    elseif name == "rent_stop" then
        -- Сдать машину можно только на базе, поэтому издалека сначала доезжаем.
        local vehicle = jobVehicle()
        local position
        if vehicle then
            local x, y = read("getElementPosition", vehicle)
            if finite(x) and finite(y) then position = {x = x, y = y} end
        end
        local _, distance = homePoint(state.homeLocation, position)
        if position and finite(distance) and distance > 12 then
            state.homeIntent, state.homeTick = "return", nil
            if not state.bot then startBot() end
            note(string.format("Еду сдавать машину: до базы %d м", math.floor(distance)))
        else
            stopBot("сдача машины")
            trigger("snowPlow:npc:rent:stop")
            note("Машина сдана")
        end
    elseif name == "route_exit" then
        stopBot("выход из маршрута")
        trigger("snowPlow:npc:route:exit")
        note("Выход из маршрута")
    elseif name == "route_best" then
        local id, score = bestRoute()
        if id then
            note(string.format("Лучший маршрут %d (%.2f/с)", id, score or 0))
            selectRoute(id)
        else
            note("Не удалось оценить маршруты")
        end
    elseif name == "route_select" then
        selectRoute(tonumber(argument))
    elseif name == "speed_limit" then
        -- Лимит приходит из панели в единицах спидометра сервера.
        local speedo = tonumber(argument)
        if finite(speedo) and speedo >= 10 then
            state.speedLimitSpeedo = speedo
            state.speedLimit = speedo
            note(string.format("Ограничитель: %d по спидометру", math.floor(speedo)))
        end
    elseif name == "ignore_traffic" then
        state.ignoreTraffic = argument == "1"
        note(state.ignoreTraffic and "Светофоры игнорируются"
            or "Светофоры снова учитываются")
    elseif name == "debug" then
        state.debug = argument == "1"
        note(state.debug and "Отладка включена" or "Отладка выключена")
    elseif name == "save_waypoint" or name == "forget_waypoint" then
        local vehicle = jobVehicle() or occupied()
        local position, heading
        if vehicle then
            local x, y = read("getElementPosition", vehicle)
            local _, _, rz = read("getElementRotation", vehicle)
            if finite(x) and finite(y) then position = {x = x, y = y} end
            heading = finite(rz) and rz or nil
        end
        if name == "save_waypoint" then
            local size = tonumber(argument)
            if argument == "small" then size = K.WAYPOINT_SMALL
            elseif argument == "mid" then size = K.WAYPOINT_MID
            elseif argument == "big" then size = K.WAYPOINT_BIG end
            waypointSave(position, heading, size)
        else waypointForget(position, heading) end
    elseif name == "waypoint_size" then
        waypointResize(tonumber(argument))
    elseif name == "editor" then
        state.editor = argument == "1"
        if state.editor and not (state.editSel and WAYPOINTS[state.editSel]) then
            local nearest = editorNearest()
            if nearest then editorSelect(nearest) end
        end
    elseif name == "wp_select" then
        editorSelect(argument == "near" and editorNearest() or tonumber(argument))
    elseif name == "wp_nudge" then
        local direction, step = argument:match("^(%a+):([%d%.]+)$")
        editorNudge(direction, step)
    elseif name == "wp_radius" then
        editorRadius(argument)
    elseif name == "wp_undo" then
        editorUndo()
    elseif name == "wp_order" then
        editorOrder(tonumber(argument))
    elseif name == "dump_waypoints" then
        waypointDump()
        note(string.format("Метки выгружены в лог: %d шт.", #WAYPOINTS))
    elseif name == "clear_waypoints" then
        WAYPOINTS = {}
        state.planDirty = true
        state.waypointCursor, state.lastWaypoint, state.finished = 1, nil, nil
        note("Метки проезда очищены")
    elseif name == "test_traffic" then
        local state = trafficState()
        local ahead = state and "--" or "нет данных"
        local vehicle = jobVehicle()
        if vehicle then
            local x, y = read("getElementPosition", vehicle)
            local _, _, rz = read("getElementRotation", vehicle)
            if finite(x) and finite(y) and finite(rz) then
                local light = trafficAhead({x = x, y = y}, rz)
                ahead = light and string.format("%s, %.0f м (зелёный при %d)",
                    light.green and "ЗЕЛЁНЫЙ" or "КРАСНЫЙ", light.distance,
                    light.green_state) or "рядом не сохранён"
            end
        end
        note(string.format("Светофор сейчас: %s | впереди: %s | сохранено: %d",
            state and string.format("%d", state) or "нет данных", ahead, #TRAFFIC))
    elseif name == "save_traffic" then
        local vehicle = jobVehicle() or occupied()
        local position, heading
        if vehicle then
            local x, y = read("getElementPosition", vehicle)
            local _, _, rz = read("getElementRotation", vehicle)
            if finite(x) and finite(y) then position = {x = x, y = y} end
            heading = finite(rz) and rz or nil
        end
        trafficSave(position, heading)
    elseif name == "forget_traffic" then
        local vehicle = jobVehicle() or occupied()
        local position, heading
        if vehicle then
            local x, y = read("getElementPosition", vehicle)
            local _, _, rz = read("getElementRotation", vehicle)
            if finite(x) and finite(y) then position = {x = x, y = y} end
            heading = finite(rz) and rz or nil
        end
        trafficForget(position, heading)
    elseif name == "dump_traffic" then
        trafficDump()
        note(string.format("Светофоры выгружены в лог: %d шт.", #TRAFFIC))
    elseif name == "spray_toggle" then
        state.sprayWanted = nil
        if pressSprayKey(now) then note("Переключение установки")
        else note("Не удалось нажать «" .. SPRAY_KEY .. "»") end
    end
end

-- === автономность ===

-- Приехали на базу: там и только там дают новый маршрут с полным баком.
-- Запрос повторяется раз в 5 с, пока сервер не ответит — сам признак успеха в том,
-- что появился CURRENT_ROUTE_ID (или исчезла машина, если мы её сдавали).
local function arriveHome(now)
    if elapsed(now, state.homeTick) < 5000 then return end
    state.homeTick = now
    if state.homeIntent == "return" then
        trigger("snowPlow:npc:rent:stop")
        note("На базе: сдаю машину")
        return
    end
    local id = bestRoute()
    if id then
        note("На базе: беру маршрут " .. id)
        selectRoute(id)
    else
        note("На базе, но маршрут оценить не удалось")
    end
end

-- === автономная работа ===
-- Полный цикл, как у водителя (28.09.2026, со слов пользователя): дойти до начальника
-- Руслана, «Огласите весь список» → маршрут 1, дойти до выданной машины, сесть (F), фары
-- (L, L), ремень (B), проехать маршрут, на СТО подтвердить «Сдачу транспорта» — и снова к
-- Руслану. Устройство взято из ресурса (дамп): у Руслана точка действия «E», её обработчик
-- шлёт серверу onNPCDialogStart, сервер открывает диалог (глобальная activeDialog); пункт
-- маршрута в списке — это selectClientRoute(id); машину сервер выдаёт событием
-- snowPlow:setPlayerJobVehicle, окно сдачи — snowPlow:openPassedWindow, кнопка
-- «Подтвердить» — snowPlow:playerCompleteTask. Ходьба — как у ЖБК-бота: камера персонажа
-- на цель и «вперёд». Каждый шаг проверяется по тому, что видно в игре, у каждого предел
-- времени и повторы; переходы пишутся в лог событием autonomy.
local Auto = {
    ROUTE = 1,
    -- Руслан на случай, если таблицы ресурса нет (data/npc-sh.lua).
    NPC = {[1] = {2230.41, 2450.86, 311.42}, [2] = {21.13, 502.15, 93.15}, [3] = {2110.84, -2822.5, 184.61}},
    TEXT = {to_npc = "иду к начальнику", talk = "говорю с начальником",
        choose = "беру маршрут", wait_vehicle = "жду машину", to_vehicle = "иду к машине",
        entering = "сажусь в машину", prepare = "фары и ремень", drive = "еду по маршруту",
        deliver = "сдаю транспорт", after = "машина сдана"},
    pressed = {}, release = {},
}

function Auto.route() return state.autoRoute or Auto.ROUTE end

-- Где стоит Руслан для локации маршрута и точка перед ним (в метре с небольшим, лицом к нему).
function Auto.npc()
    local location = locationOfRoute(Auto.route()) or 1
    local x, y, rot
    if type(_G.tPedsData) == "table" then
        for _, ped in pairs(_G.tPedsData) do
            if type(ped) == "table" and ped.location == location and type(ped.pos) == "table" then
                x, y, rot = ped.pos[1], ped.pos[2], ped.pos[4]
            end
        end
    end
    if not finite(x) then
        local fallback = Auto.NPC[location] or Auto.NPC[1]
        x, y, rot = fallback[1], fallback[2], fallback[3]
    end
    local r = math.rad(finite(rot) and rot or 0)
    return {x = x, y = y, standX = x - math.sin(r) * 1.3, standY = y + math.cos(r) * 1.3,
        id = "work:snowPlow:" .. location}
end

function Auto.set(stage, why)
    if Auto.stage == stage then return end
    Auto.stage, Auto.since, Auto.step = stage, getTickCount(), 0
    Auto.walk, Auto.dodge = nil, nil
    local x, y = read("getElementPosition", localPlayer)
    emit("autonomy", {stage = stage, why = why or NULL, x = finite(x) and Smart.round(x) or NULL,
        y = finite(y) and Smart.round(y) or NULL, route = Auto.route()}, true)
    note("Автономность: " .. (Auto.TEXT[stage] or stage) .. (why and (" — " .. why) or ""))
end

function Auto.control(name, on)
    if Auto.pressed[name] == on then return end
    read("setPedControlState", localPlayer, name, on)
    Auto.pressed[name] = on
end

function Auto.stopWalk()
    for name, on in pairs(Auto.pressed) do
        if on then read("setPedControlState", localPlayer, name, false) end
    end
    Auto.pressed, Auto.walk, Auto.dodge = {}, nil, nil
end

-- Нажать клавишу и отпустить через 80 мс (как живое нажатие).
function Auto.tap(key, now)
    local ok = read("dfEmulateKey", key, true) == true
    if ok then Auto.release[key] = now + 80 end
    emit("autonomy_key", {key = key, ok = ok, stage = Auto.stage or NULL}, true)
    return ok
end

function Auto.keys(now)
    for key, at in pairs(Auto.release) do
        if now >= at then
            read("dfEmulateKey", key, false)
            Auto.release[key] = nil
        end
    end
end

-- Идти к точке. Застряли (за 1.5 с ближе меньше чем на полметра) — шаг наискось в
-- сторону с прыжком, стороны чередуются. Возвращает true, когда дошли.
function Auto.walkTo(now, x, y, arrive)
    local px, py = read("getElementPosition", localPlayer)
    if not finite(px) or not finite(py) then return false end
    local d = math.sqrt((x - px) ^ 2 + (y - py) ^ 2)
    if d <= arrive then
        Auto.stopWalk()
        return true, d
    end
    if not Auto.walk or elapsed(now, Auto.walk.at) >= 1500 then
        if Auto.walk and Auto.walk.d - d < 0.5 then
            local side = -(Auto.dodgeSide or -1)
            Auto.dodgeSide = side
            Auto.dodge = {till = now + 800, side = side, jump = now + 120}
            emit("autonomy_walk", {stuck = true, x = Smart.round(px), y = Smart.round(py), left = Smart.round(d, 1)}, true)
        end
        Auto.walk = {at = now, d = d}
    end
    local dodging = Auto.dodge and now < Auto.dodge.till
    local bearing = (bearingTo(x - px, y - py) + (dodging and Auto.dodge.side * 70 or 0) + 360) % 360
    read("setPedCameraRotation", localPlayer, bearing)
    Auto.control("forwards", true)
    Auto.control("sprint", d > 8 and not dodging)
    Auto.control("jump", dodging and now < Auto.dodge.jump)
    return false, d
end

-- Выданная машина (есть, даже когда мы не в ней) и точка у водительской двери.
function Auto.vehicle()
    local vehicle = read("getVehicle")
    if valid(vehicle) then return vehicle end
    return jobVehicle()
end

function Auto.door(vehicle)
    local x, y = read("getElementPosition", vehicle)
    local _, _, rz = read("getElementRotation", vehicle)
    if not finite(x) or not finite(y) then return nil end
    local r = math.rad(finite(rz) and rz or 0)
    local fx, fy = -math.sin(r), math.cos(r)
    -- Слева по ходу, у кабины: +1.5 м вперёд, 2.6 м влево от оси.
    return {x = x + fx * 1.5 - fy * 2.6, y = y + fy * 1.5 + fx * 2.6}
end

function Auto.fail(why)
    state.autonomy = false
    Auto.stopWalk()
    Auto.stage = nil
    emit("autonomy", {stage = "stopped", why = why}, true)
    note("Автономность выключена: " .. why)
end

function Auto.update(now, context)
    Auto.keys(now)
    if not state.autonomy then
        if Auto.stage then
            Auto.stopWalk()
            Auto.stage = nil
        end
        return
    end
    if not context.ready then return end
    local vehicle = Auto.vehicle()
    local inside = vehicle ~= nil and occupied() == vehicle
    local stage = Auto.stage
    local age = elapsed(now, Auto.since)

    -- Откуда начинать: по тому, что сейчас видно в игре.
    if not stage then
        if Auto.window then Auto.set("deliver")
        elseif vehicle and inside and (context.route or state.bot) then
            -- Включили, когда водитель уже за рулём: фары и ремень он сделал сам. L и B
            -- переключают — нажать их ещё раз значило бы выключить фары и отстегнуться.
            Auto.prepared = vehicle
            Auto.set("drive")
        elseif vehicle and context.route then Auto.set("to_vehicle")
        else Auto.set("to_npc") end
        return
    end

    if stage == "to_npc" then
        -- Сидим в машине (не в той или уже без маршрута) — выходим: к Руслану пешком.
        local car = occupied()
        if car then
            if elapsed(now, Auto.exitTap) >= 2500 then Auto.exitTap = now; Auto.tap("f", now) end
            return
        end
        local npc = Auto.npc()
        local px, py = read("getElementPosition", localPlayer)
        if finite(px) and (px - npc.x) ^ 2 + (py - npc.y) ^ 2 > 300 * 300 then
            Auto.fail("до начальника дальше 300 м — довезите меня к базе")
            return
        end
        if Auto.walkTo(now, npc.standX, npc.standY, 0.6) then Auto.set("talk") return end
        if age > 120000 then Auto.set("to_npc", "не дошёл за 2 минуты, пробую снова") end
    elseif stage == "talk" then
        if _G.activeDialog then Auto.set("choose") return end
        if Auto.step == 0 then
            Auto.tap("e", now)
            Auto.step = 1
        elseif Auto.step == 1 and age > 2500 then
            -- «E» не сработала — запускаем ту же точку действия Руслана напрямую: её
            -- обработчик в ресурсе сам проверит условия и отправит запрос серверу.
            local npc = Auto.npc()
            local points = read("getElementsByType", "actionPoint", resourceRoot)
            local fired = false
            if type(points) == "table" then
                for _, point in ipairs(points) do
                    local x, y = read("getElementPosition", point)
                    if finite(x) and (x - npc.x) ^ 2 + (y - npc.y) ^ 2 < 4 then
                        fired = read("triggerEvent", "actionPoint:onClientTrigger", point) == true or fired
                    end
                end
            end
            emit("autonomy", {stage = "talk", why = "action_point", fired = fired}, true)
            Auto.step = 2
        elseif age > 7000 then
            Auto.talkTries = (Auto.talkTries or 0) + 1
            if Auto.talkTries >= 3 then Auto.fail("начальник не отвечает (3 попытки)") return end
            Auto.set("to_npc", "диалог не открылся, подхожу снова")
        end
    elseif stage == "choose" then
        if age < 800 then return end
        -- Сервер открыл диалог приёма («Ты готов к работе?») — сначала нанимаемся. Иначе
        -- мы уже на работе: лишний запрос приёма серверу не шлём.
        if Auto.dialogKey == "employment:start" and not Auto.hired then
            read("startClientWork")
            Auto.hired = now
            emit("autonomy", {stage = "choose", why = "hire"}, true)
            return
        end
        if Auto.hired and elapsed(now, Auto.hired) < 3000 then return end
        Auto.hired, Auto.dialogKey = nil, nil
        read("selectClientRoute", Auto.route())
        read("endDialog")
        Auto.talkTries = 0
        Auto.set("wait_vehicle", "маршрут " .. Auto.route())
    elseif stage == "wait_vehicle" then
        if vehicle then Auto.vehicleTries = 0; Auto.set("to_vehicle") return end
        if age > 12000 then
            Auto.vehicleTries = (Auto.vehicleTries or 0) + 1
            if Auto.vehicleTries >= 3 then
                Auto.fail("машину не выдали 3 раза — хватает ли денег на залог 10 000?")
                return
            end
            Auto.set("to_npc", "машину не выдали, прошу снова")
        end
    elseif stage == "to_vehicle" then
        if not vehicle then Auto.set("to_npc", "машины больше нет") return end
        if inside then Auto.set("prepare") return end
        local door = Auto.door(vehicle)
        local arrived, left = false, nil
        if door then arrived, left = Auto.walkTo(now, door.x, door.y, 1.2) end
        if arrived or (finite(left) and left < 3.5 and age > 20000) then
            Auto.stopWalk()
            Auto.tap("f", now)
            Auto.set("entering")
        elseif age > 60000 then
            Auto.set("to_vehicle", "не дошёл до машины за минуту, иду снова")
        end
    elseif stage == "entering" then
        if inside then Auto.set("prepare") return end
        if not vehicle then Auto.set("to_npc", "машины больше нет") return end
        if age > 6000 then
            Auto.enterTries = (Auto.enterTries or 0) + 1
            if Auto.enterTries >= 3 then
                Auto.enterTries = 0
                Auto.set("to_vehicle", "не сел за 3 попытки, подхожу ближе")
            else
                Auto.tap("f", now)
                Auto.since = now
            end
        end
    elseif stage == "prepare" then
        if not inside then Auto.set("to_vehicle", "оказался не в машине") return end
        Auto.enterTries = 0
        -- Фары: L дважды, потом ремень B — с паузами, как руками.
        local plan = {{1200, "l"}, {1900, "l"}, {2600, "b"}}
        local next = plan[Auto.step + 1]
        if next and age >= next[1] then
            -- Журнал: что L делает с фарами (0 — как игра решит, 1 — выкл, 2 — вкл).
            emit("autonomy", {stage = "prepare", key = next[2],
                lights = read("getVehicleOverrideLights", vehicle) or NULL}, true)
            Auto.tap(next[2], now)
            Auto.step = Auto.step + 1
            if next[2] == "b" then Auto.watchData = now + 5000 end
        elseif not next and age >= 3400 then
            Auto.prepared = vehicle
            emit("autonomy", {stage = "prepare", why = "done",
                lights = read("getVehicleOverrideLights", vehicle) or NULL}, true)
            Auto.set("drive")
        end
    elseif stage == "drive" then
        if not inside then Auto.set(vehicle and "to_vehicle" or "to_npc", "не в машине") return end
        if Auto.window or state.finished then Auto.set("deliver") return end
        if not state.bot then
            if context.route or #plan.items > 0 then startBot() end
        end
    elseif stage == "deliver" then
        if Auto.window and elapsed(now, Auto.window) >= 800 and (context.speed or 0) <= 1 then
            -- Кнопка «Подтвердить» окна сдачи шлёт ровно это событие.
            read("triggerServerEvent", "snowPlow:playerCompleteTask", resourceRoot)
            read("closeRouteWindow")
            emit("autonomy", {stage = "deliver", why = "confirmed"}, true)
            Auto.window = nil
            state.trips = (state.trips or 0)
            Auto.set("after")
        elseif not Auto.window and age > 30000 and not Auto.windowNoted then
            Auto.windowNoted = true
            note("Автономность: окно сдачи не открылось — стою у места сдачи")
        end
    elseif stage == "after" then
        -- Машину забрали (или мы уже снаружи) — снова к Руслану.
        if not vehicle or not inside or age > 8000 then
            Auto.prepared, Auto.windowNoted = nil, nil
            state.finished = nil
            Auto.set("to_npc", "рейс сдан")
        end
    end
end

-- === контр-админ ===
-- Как в самолёте (PilotTelemetry) и Трамботе. Пока работает бот или автономия:
-- • надпись администратора на экране «(ADMIN)» — её ловит DLL (монитор включаем здесь),
--   сама даёт сирену и передаёт текст боту: в чат «[ТРЕВОГА] Админ крикнул: …»;
-- • сообщение администратора в чате (список ников по серверу — из самолёта, плюс ники из
--   системных строк «Администратор Ник …») или системная строка администратора — сирена;
-- • сообщение игрока, стоящего ближе 50 м (формат «Ник[id]: текст»), — сирена, как в
--   Трамботе: администраторы проверяют, подъехав и написав.
-- Сирена по одному поводу — не чаще раза в 5 с; вход и выход администраторов не в счёт.
local Guard = {
    ADMINS = {
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
},
    PATTERNS = {
        "администратором%s+([A-Za-z]+_[A-Za-z0-9]+)",
        "администратор%s+([A-Za-z]+_[A-Za-z0-9]+)",
        "Администратор%s+([A-Za-z]+_[A-Za-z0-9]+)",
        "Admin%s+([A-Za-z]+_[A-Za-z0-9]+)",
        "заблокирован%s+([A-Za-z]+_[A-Za-z0-9]+)",
        "наказан%s+([A-Za-z]+_[A-Za-z0-9]+)",
        "предупреждён%s+([A-Za-z]+_[A-Za-z0-9]+)",
        "предупрежден%s+([A-Za-z]+_[A-Za-z0-9]+)",
    },
    PRESENCE = {"зашёл", "зашел", "подключился", "вышел", "покинул сервер", "отключился",
        "joined the server", "left the server", "connected", "disconnected"},
    NEAR = 50,
    known = {}, alerts = {},
}
for nick in tostring(Guard.ADMINS[read("getServerIp", true)] or ""):gmatch("%S+") do
    Guard.known[nick] = true
end

function Guard.active() return state.bot == true or state.autonomy == true end

-- Монитор экранных надписей в DLL — пока бот работает (как у самолёта — пока летит).
function Guard.sync()
    local active = Guard.active()
    if Guard.monitor == active then return end
    Guard.monitor = active
    read("dfSetAlertMonitorEnabled", active)
end

function Guard.alert(key, text, data, cooldown)
    local now = getTickCount()
    if elapsed(now, Guard.alerts[key]) < (cooldown or 5000) then return false end
    Guard.alerts[key] = now
    local played = read("dfPlayAlertSignal") == true
    read("outputChatBox", "#FF5555[PlowBot] #FFFFFF" .. text, 255, 255, 255, true)
    emit("safety_alert", {kind = key, text = text, played = played, data = data or NULL}, true)
    return played
end

function Guard.nearbyById(id)
    local px, py, pz = read("getElementPosition", localPlayer)
    if not finite(px) then return nil end
    local dimension = read("getElementDimension", localPlayer)
    local interior = read("getElementInterior", localPlayer)
    for _, player in ipairs(read("getElementsByType", "player", root, true) or {}) do
        if player ~= localPlayer and tostring(read("getElementData", player, "id") or "") == id then
            local x, y, z = read("getElementPosition", player)
            if finite(x) and (x - px) ^ 2 + (y - py) ^ 2 + (z - pz) ^ 2 <= Guard.NEAR * Guard.NEAR
                and read("getElementDimension", player) == dimension
                and read("getElementInterior", player) == interior then
                return player, math.sqrt((x - px) ^ 2 + (y - py) ^ 2 + (z - pz) ^ 2)
            end
            return nil
        end
    end
end

function Guard.chat(text, r, g, b, messageType)
    if not Guard.active() or type(text) ~= "string" or (messageType ~= nil and messageType ~= 0) then
        return
    end
    local plain = text:gsub("#%x%x%x%x%x%x", "")
    for _, pattern in ipairs(Guard.PATTERNS) do
        local nick = plain:match(pattern)
        if nick then Guard.known[nick] = true; break end
    end
    for _, phrase in ipairs(Guard.PRESENCE) do
        if plain:find(phrase, 1, true) then return end
    end
    local nick, id = plain:match("^([A-Za-z_][A-Za-z0-9_]*)%[([^%]]+)%]:")
    local admin = nick ~= nil and Guard.known[nick] == true
    local system = r == 255 and g == 164 and b == 104 and plain:find("Администратор", 1, true) ~= nil
    if admin or system then
        Guard.alert("admin_chat", "сообщение администратора: " .. plain, {sender = nick or NULL})
        return
    end
    if id then
        local player, distance = Guard.nearbyById(id)
        if player then
            Guard.alert("near_chat", string.format("игрок рядом (%.0f м) пишет: %s", distance, plain),
                {sender = nick or NULL, distance = Smart.round(distance, 1)})
        end
    end
end

-- Упёрлись и начинаем выбираться (или выхода нет) — сирена: водителю стоит взглянуть.
Guard.KINDS = {static = "в препятствие", ped = "в пешехода", parked = "в стоящую машину",
    queue = "в машину", traffic = "в машину"}
function Guard.stuck(out)
    if not state.bot or type(out) ~= "table" then return end
    local reason = out.reason
    if reason ~= "escape_blocked" and reason ~= "escape_start" and reason ~= "boxed" then return end
    local what = Guard.KINDS[(out.detail and out.detail.blocked_by) or ""] or "в помеху"
    Guard.alert("stuck", reason == "boxed" and ("упёрся " .. what .. ", выхода не вижу — нужна помощь")
        or ("упёрся " .. what .. ", выбираюсь"), {reason = reason}, 30000)
end

-- === панель ===

local function pushUi(context)
    bridge.update("heartbeat", getTickCount())
    bridge.update("bot", state.bot and "1" or "0")
    bridge.update("autonomy", state.autonomy and "1" or "0")
    bridge.update("recording", state.recording and "1" or "0")
    bridge.update("resource_ready", context.ready and "1" or "0")
    bridge.update("employed", context.employed and "1" or "0")
    bridge.update("vehicle", context.vehicle and "1" or "0")
    bridge.update("on_route", context.route and "1" or "0")
    bridge.update("spray", context.spray and "1" or "0")
    -- Во время записи статус показывает счётчик: видно, что телеметрия реально идёт.
    if state.recording then
        bridge.update("status", string.format("ЗАПИСЬ: %d сэмплов, точек %d — %s",
            state.samples or 0, state.checkpointsSeen or 0, state.status))
    else
        bridge.update("status", state.status)
    end
    bridge.update("report", table.concat(state.report, "\n"))
    local id = context.route
    bridge.update("route", id and routeDescription(id):sub(1, 120) or "")
    bridge.update("route_info", id and routeDescription(id) or
        (function()
            local best = bestRoute()
            return best and routeDescription(best) or "нет данных о маршрутах"
        end)())
    local best = bestRoute()
    bridge.update("best_route", best and tostring(best) or "--")
    bridge.update("speed_limit", string.format("%d", state.speedLimitSpeedo or 50))
    bridge.update("debug", state.debug and "1" or "0")
    bridge.update("waypoints", string.format("%d", #WAYPOINTS))
    bridge.update("waypoint_cursor", string.format("%d", math.min(state.waypointCursor or 1, #WAYPOINTS)))
    local lastIndex = state.lastWaypoint
    local lastPoint = lastIndex and WAYPOINTS[lastIndex]
    bridge.update("waypoint_size", string.format("%d",
        lastPoint and math.floor(lastPoint[4] or K.WAYPOINT_MID) or K.WAYPOINT_MID))
    bridge.update("waypoint_last", lastPoint and string.format("%d", lastIndex) or "--")
    bridge.update("editor", state.editor and "1" or "0")
    bridge.update("wp_file", state.marksFile or "")
    if state.editor then
        local position = carPosition()
        local rows = {}
        for index, point in ipairs(WAYPOINTS) do
            local d = position and math.sqrt((point[1] - position.x) ^ 2
                + (point[2] - position.y) ^ 2) or -1
            rows[#rows + 1] = string.format("%d,%g,%.0f,%s", index, point[4] or K.WAYPOINT_MID, d,
                point[5] and tostring(point[5]) or "-")
        end
        -- Строка ограничена 4 КБ; на 100 меток хватает с запасом.
        bridge.update("wp_list", table.concat(rows, ";"):sub(1, 4000))
        local index = state.editSel
        local point = index and WAYPOINTS[index]
        if point then
            local d = position and math.sqrt((point[1] - position.x) ^ 2
                + (point[2] - position.y) ^ 2) or -1
            bridge.update("wp_edit", string.format("%d|%.2f|%.2f|%.1f|%g|%s|%.0f|%d", index,
                point[1], point[2], point[3] or 0, point[4] or K.WAYPOINT_MID,
                point[5] and tostring(point[5]) or "-", d, #(state.editUndo or {})))
        else
            bridge.update("wp_edit", "")
        end
    end
    bridge.update("ignore_traffic", state.ignoreTraffic and "1" or "0")
    bridge.update("traffic_working", trafficWorking() and "1" or "0")
    local lightState = trafficState()
    bridge.update("traffic_state", lightState and string.format("%d", lightState) or "--")
    bridge.update("traffic_info", string.format("В памяти: %d. Сохраняй, стоя на стоп-линии, "
        .. "когда горит зелёный.", #TRAFFIC))

    local out = state.lastOut or {}
    local total = context.points or 0
    local dash = {
        context.speed and string.format("%d", math.floor(context.speed * SPEEDO + 0.5)) or "--",
        context.index and (context.index .. "/" .. total) or "--",
        context.water and string.format("%d%%", math.floor(context.water)) or "--",
        (function()
            local light = context.light
            if not light then return "--" end
            return string.format("%s %d м", light.green and "ЗЕЛЁНЫЙ" or "КРАСНЫЙ",
                math.floor(light.distance))
        end)(),
        context.distance and string.format("%d м", math.floor(context.distance)) or "--",
        (function()
            local d = out.detail or {}
            if not finite(d.water_seconds) or not finite(d.route_seconds) then return "--" end
            return string.format("%+d с", math.floor(d.water_seconds - d.route_seconds))
        end)(),
        out.steer and string.format("%.2f", out.steer) or "--",
        out.throttle and string.format("%.2f", out.throttle) or "--",
        context.obstacle and string.format("%d м", math.floor(context.obstacle)) or "чисто",
        context.liters and string.format("%d л", math.floor(context.liters)) or "--",
        out.detail and out.detail.stuck_ms and out.detail.stuck_ms > 0
            and string.format("%.1f с", out.detail.stuck_ms / 1000) or "нет",
        tostring(state.trips),
    }
    bridge.update("dashboard", table.concat(dash, "|"))
end

-- === кадр ===

local function collect(now)
    local context = {}
    context.ready = type(_G.tRoutesPoints) == "table" and type(_G.getJobVehicle) == "function"
    context.vehicle = jobVehicle()
    context.inVehicle = context.vehicle ~= nil and occupied() == context.vehicle
    context.employed = context.ready and (context.vehicle ~= nil
        or read("getElementData", localPlayer, "work") == "SnowPlow"
        or state.employedSeen == true)
    context.route = routeId()
    context.index = pointIndex()
    local points = routePoints()
    context.points = points and #points or 0
    context.spray = context.vehicle and sprayOn(context.vehicle) or false
    context.water = waterPercent()
    context.liters = (function()
        local value = read("getWaterLiters")
        return finite(value) and value or nil
    end)()
    context.progress = routeProgress()
    context.speed = context.vehicle and speedOf(context.vehicle) or 0
    if context.route then state.employedSeen = true end
    return context
end

-- Куда бот реально рулит: точка упреждения, сдвинутая на смещение контроллера (объезд,
-- обход после отката, протиснуться мимо помехи) — так же, как её сдвигает контроллер.
local function debugAim(position, point, offset)
    if not point then return nil end
    if not finite(offset) or math.abs(offset) <= 0.05 then return {x = point.x, y = point.y} end
    local ax, ay = point.x - position.x, point.y - position.y
    local len = math.sqrt(ax * ax + ay * ay)
    if len < 0.5 then return {x = point.x, y = point.y} end
    local nx, ny = ax / len, ay / len
    local lead = math.min(len, 30)
    return {x = position.x + nx * lead + ny * offset, y = position.y + ny * lead - nx * offset}
end

-- Помеха в стороне от пути через свою метку. Луч смотрит по курсу, и пока машина
-- подъезжает к метке и доворачивает, в него попадает то, мимо чего путь не идёт, — у новой
-- метки 7 это столб, который она как раз обходит. Объезд и «протиснуться» уводили машину
-- на 2.4–3.5 м к нему, и бот вставал носом в столб (тест 2.9.2). Путь — два отрезка:
-- машина → центр метки → следующая цель; помеха дальше кузова с запасом от обоих — не наша.
local function beyondMark(step, position, x, y)
    if not step or step.item.kind ~= "mark" or step.rail or step.final or not step.next then
        return false
    end
    local mx, my = step.target.x, step.target.y
    local clear = CORRIDOR + 1
    return segmentDistance(x, y, position.x, position.y, mx, my) > clear
        and segmentDistance(x, y, mx, my, step.next.x, step.next.y) > clear
end

-- Сдвиг цели мимо помех. На рельсе депо — от самого отрезка между метками; к обычной
-- метке — от машины, в пределах 1.5 м: метку проходим через центр, а помехи за ней, мимо
-- которых путь не идёт, не считаем.
local function applyStepShift(step, position, heading, context)
    if not step then return end
    if step.item.kind == "mark" and not step.rail then
        context.squeeze = squeezeShift(position, heading, context.curve or 0, SQUEEZE_MAX,
            function(x, y) return beyondMark(step, position, x, y) end)
        return
    end
    if not step.rail then return end
    local leg = step.rail
    local shift = railShift(leg, position)
    context.squeeze = 0
    local lx, ly = leg[3] - leg[1], leg[4] - leg[2]
    local length = math.sqrt(lx * lx + ly * ly)
    if shift ~= 0 and length > 0.5 then
        lx, ly = lx / length, ly / length
        -- Сдвиг нарастает от начала отрезка: центр метки проходим как есть, а к помехе
        -- дальше по отрезку уже выходим сдвинутыми.
        local along = (step.aim.x - leg[1]) * lx + (step.aim.y - leg[2]) * ly
        shift = shift * clamp(along / K.RAIL_SHIFT_RAMP, 0, 1)
        -- Вправо от отрезка — это (ly, -lx).
        step.aim = {x = step.aim.x + ly * shift, y = step.aim.y - lx * shift, z = step.aim.z}
    end
    context.railShift = shift
end

-- === телеметрия ===
-- Пишется независимо от бота: первый проезд делает человек, и именно он даёт эталон.

local function round(value, places)
    if not finite(value) then return nil end
    local factor = 10 ^ (places or 2)
    return math.floor(value * factor + 0.5) / factor
end

local inputNames = {"accelerate", "brake_reverse", "vehicle_left", "vehicle_right",
    "handbrake", "sub_mission"}

local function playerInput()
    local input = {}
    for _, name in ipairs(inputNames) do
        local value = read("getAnalogControlState", name)
        if finite(value) and value ~= 0 then input[name] = round(value, 3) end
    end
    return input
end

-- Полный список точек маршрута — основа, по которой потом строится трасса бота.
local function recordRoute(context)
    if state.recordedRoute == context.route then return end
    state.recordedRoute = context.route
    if not context.route then return end
    local points = routePoints()
    if not points then return end
    local list = {}
    for i = 1, #points do
        local point = pointAt(i)
        list[i] = point and {round(point.x), round(point.y), round(point.z)} or NULL
    end
    local info = type(_G.tRoutes) == "table" and _G.tRoutes[context.route] or nil
    emit("route", {id = context.route, count = #list,
        name = type(info) == "table" and tostring(info.name) or NULL,
        time_limit = type(info) == "table" and tonumber(info.time) or NULL,
        salary = type(info) == "table" and tonumber(info.salary) or NULL,
        dist = type(_G.tDist) == "table" and _G.tDist[context.route] or NULL,
        water_needed = type(_G.tWaterCapacity) == "table"
            and _G.tWaterCapacity[context.route] or NULL,
        ideal_speed = tonumber(_G.IDEAL_SPEED) or NULL,
        limit_distance = tonumber(_G.LIMIT_DISTANCE) or NULL,
        points = list}, true)
    note("Маршрут записан: " .. #list .. " точек")
end

-- Прохождение контрольной точки: с какой скоростью и насколько мимо центра сферы.
local function trackCheckpoints(now, context, position)
    if state.lastIndex == context.index then return end
    local previous = state.lastIndex
    state.lastIndex = context.index
    if not previous or not context.index or context.index <= previous then
        state.lastCheckpointTick = now
        return
    end
    local center = state.lastTarget
    local miss
    if center and position then
        miss = math.sqrt((center.x - position.x) ^ 2 + (center.y - position.y) ^ 2)
    end
    emit("checkpoint", {index = previous, next_index = context.index,
        total = context.points, speed = round(context.speed, 1),
        speedo = round(context.speed * SPEEDO, 1), miss = round(miss),
        position = position and {round(position.x), round(position.y), round(position.z)} or NULL,
        water = round(context.water, 1), liters = round(context.liters, 1),
        spray = context.spray, bot = state.bot,
        seconds = round(elapsed(now, state.lastCheckpointTick) / 1000, 2)}, true)
    state.lastCheckpointTick = now
    state.checkpointsSeen = (state.checkpointsSeen or 0) + 1
    note(string.format("Точка %d/%d пройдена%s", previous, context.points or 0,
        miss and string.format(" (мимо центра %.1f м)", miss) or ""))
end

local function recordSample(now, context, position, heading, frozen)
    -- 20 Гц: на 38 км/ч это точка каждые полметра — хватает, чтобы восстановить
    -- траекторию человека в повороте, а не только прямые между точками.
    if elapsed(now, state.sampleTick) < 50 then return end
    state.sampleTick = now
    state.samples = (state.samples or 0) + 1
    local vx, vy, vz
    if context.vehicle then vx, vy, vz = read("getElementVelocity", context.vehicle) end
    emit("sample", {
        pos = position and {round(position.x), round(position.y), round(position.z)} or NULL,
        heading = round(heading, 1), speed = round(context.speed, 1),
        speedo = round(context.speed * SPEEDO, 1),
        vel = finite(vx) and {round(vx, 3), round(vy, 3), round(vz, 3)} or NULL,
        index = context.index or NULL, total = context.points,
        distance = round(context.distance), water = round(context.water, 1),
        liters = round(context.liters, 1), spray = context.spray,
        obstacle = round(context.obstacle), input = playerInput(),
        obstacle_beyond = context.obstacleBeyond,
        obstacle_kind = context.obstacleKind or NULL, rear_gap = round(context.rearGap, 1),
        nudge_free = round(context.nudgeFree, 1),
        path = context.pathFrame and {s = round(context.pathFrame.s, 1),
            e = round(context.pathFrame.lateral, 2), on = context.pathFrame.onPath == true,
            lead = round(context.pathLead, 1), speed = round(context.pathSpeed, 1)} or NULL,
        bot = state.bot, in_vehicle = context.inVehicle, frozen = frozen == true,
        target = state.lastTarget
            and {round(state.lastTarget.x), round(state.lastTarget.y)} or NULL,
        target_source = context.targetSource or NULL,
        table_gap = round(context.tableGap),
        light = context.light and {distance = round(context.light.distance),
            green = context.light.green, state = context.light.state,
            green_state = context.light.green_state} or NULL,
        light_state = trafficState() or NULL,
        waypoint = context.waypointKind or NULL,
        waypoint_gap = round(context.waypointGap),
        waypoint_index = context.waypointIndex or NULL,
        waypoint_final = context.waypointFinal == true,
        side_left = round(context.sideLeft, 1), side_right = round(context.sideRight, 1),
        -- Где помеха относительно оси (плюс — справа), бордюр, соседние полосы.
        obstacle_side = round(context.obstacleSide, 2), curb = round(context.curb, 1),
        forward = round(context.forward, 2), curve = round(context.curve, 4),
        rail_shift = round(context.railShift, 2), squeeze = round(context.squeeze, 2),
        gap_left = round(context.gapLeft, 1), gap_right = round(context.gapRight, 1),
        ignore_traffic = state.ignoreTraffic == true,
        out = state.bot and state.lastOut and {throttle = round(state.lastOut.throttle, 3),
            brake = round(state.lastOut.brake, 3), steer = round(state.lastOut.steer, 3),
            reason = state.lastOut.reason,
            goal = round(state.lastOut.detail and state.lastOut.detail.goal, 1),
            -- Почему цель скорости именно такая: ограничение дугой, поворотом в точке, ошибкой курса.
            turn_limit = round(state.lastOut.detail and state.lastOut.detail.turn_limit, 1),
            corner = round(state.lastOut.detail and state.lastOut.detail.corner, 1),
            turn = round(state.lastOut.detail and state.lastOut.detail.turn, 1),
            -- Поворот за следующей целью и скорость под него (торможение заранее).
            turn_after = round(state.lastOut.detail and state.lastOut.detail.turn_after, 1),
            blocked_by = state.lastOut.detail and state.lastOut.detail.blocked_by or nil,
            escape = state.lastOut.detail and state.lastOut.detail.escape or nil,
            corner_after = round(state.lastOut.detail and state.lastOut.detail.corner_after, 1),
            err = round(state.lastOut.detail and state.lastOut.detail.err, 1),
            curvature = round(state.lastOut.detail and state.lastOut.detail.curvature, 4),
            -- Сдвиг прицела: итог, вынос в поворот, обход после отката.
            offset = round(state.lastOut.detail and state.lastOut.detail.offset, 2),
            apex = round(state.lastOut.detail and state.lastOut.detail.apex, 2),
            detour = round(state.lastOut.detail and state.lastOut.detail.detour, 2),
            squeeze = round(state.lastOut.detail and state.lastOut.detail.squeeze, 2)} or NULL,
    })
end

local function onFrame()
    local now = getTickCount()
    state.frame = state.frame + 1
    updateSprayKey(now)
    for _ = 1, 8 do
        local command = bridge.command()
        if not command then break end
        handleCommand(command, now)
        -- Нажатая кнопка должна отразиться в панели в этом же кадре.
        state.uiTick = nil
    end

    -- Дрейф крейсера меняем раз в 12 с, чтобы скорость не выглядела машинной.
    if elapsed(now, state.jitterTick) >= 12000 then
        state.jitterTick = now
        state.jitter = (math.random() * 2 - 1) * 5
    end
    local context = collect(now)
    Auto.update(now, context)
    Guard.sync()
    if state.lastOut ~= Guard.lastSeen then
        Guard.lastSeen = state.lastOut
        Guard.stuck(state.lastOut)
    end
    -- Стоим за помехой (машина с водителем, очередь без светофора) больше 30 с — взгляни.
    if state.bot and controller.blockedSince and elapsed(now, controller.blockedSince) > 30000 then
        Guard.alert("waiting", "стою за помехой больше 30 с — взгляни", nil, 60000)
    end

    -- Геометрия нужна и боту, и записи ручного проезда, поэтому считается всегда.
    local vehicle = context.vehicle
    local position, heading, frozen, target
    if vehicle then
        local x, y, z = read("getElementPosition", vehicle)
        local _, _, rz = read("getElementRotation", vehicle)
        if finite(x) and finite(y) then
            position = {x = x, y = y, z = finite(z) and z or 0}
            heading = finite(rz) and rz or 0
            -- Скорость со знаком, м/с вдоль курса: getVehicleSpeed отдаёт модуль, а после
            -- отката машина ещё катится назад, и руль «как вперёд» уводит нос не туда.
            local vx, vy = read("getElementVelocity", vehicle)
            if finite(vx) and finite(vy) then
                local r = math.rad(heading)
                context.forward = (vx * -math.sin(r) + vy * math.cos(r)) * 50
            end
        end
        frozen = read("isElementFrozen", vehicle) == true
    end
    if state.suffixRoute ~= context.route then
        state.suffixRoute = context.route
        buildSuffix()
    end
    -- Цепочку строим под каждый новый маршрут; после закрытия маршрута она живёт
    -- дальше — по ней бот доезжает оставшиеся метки до финиша.
    -- Новый рейс — всегда с нуля, даже с тем же номером маршрута. Раньше цепочка не
    -- перестраивалась: метки прошлого рейса оставались «пройденными», а место на пути — у точки
    -- сдачи (её прямая идёт в паре метров от места выдачи машины), и бот сразу ехал сдавать
    -- новую машину (заезд 29.09, второй рейс).
    if context.route and context.route ~= state.lastRoute then state.planDirty = true end
    if context.route and (plan.route ~= context.route or state.planDirty) then
        state.planDirty = nil
        buildPlan(context.route)
    elseif state.planDirty and not context.route then
        -- Метки поменяли без маршрута: старая цепочка больше не верна.
        state.planDirty = nil
        plan.route, plan.items, plan.serverCount = nil, {}, 0
    end
    if position and context.index then
        -- Сначала живая колсфера игры, таблица маршрута — только запасной вариант.
        local live = liveTarget()
        local fromTable = pointAt(context.index)
        target = live or fromTable
        context.targetSource = live and "colshape" or (fromTable and "table" or nil)
        if live and fromTable then
            context.tableGap = math.sqrt((live.x - fromTable.x) ^ 2
                + (live.y - fromTable.y) ^ 2)
        end
        if target then
            local dx, dy = target.x - position.x, target.y - position.y
            context.distance = math.sqrt(dx * dx + dy * dy)
            if state.suffix and state.suffix[context.index] then
                context.routeLeft = context.distance + state.suffix[context.index]
            end
        end
    end
    if position and not frozen and (state.bot or state.recording) then
        -- Дуга, по которой машина поедет: руль прошлого кадра, но не круче возможного
        -- на этой скорости. На откате и без бота — прямая по курсу.
        local curve = 0
        local last = state.bot and state.lastOut
        if last and last.detail and finite(last.detail.curvature)
            and last.reason ~= "escape" and last.reason ~= "settle" then
            local limit = 1 / minRadius(toMs(context.speed))
            curve = clamp(last.detail.curvature, -limit, limit)
        end
        context.curve = curve
        rayPhase = rayPhase % 3 + 1
        -- Где машина на пути по меткам. Лучи помех — вдоль пути; на откате и выходе из тупика —
        -- по дуге руля, как раньше.
        local frame = state.bot and Path.n > 1 and not state.pathRail
            and Path.update(position, heading, state.pathSlot,
            context.speed, last and last.detail and last.detail.offset or 0) or nil
        -- На рельсах депо — тоже по дуге: рельс ведёт прямыми отрезками меток, а лучи вдоль
        -- скруглённого пути у метки 3 цепляли угол будки, и бот вставал.
        if frame and (controller.escapeUntil or controller.nudgeUntil or state.pathRail) then
            frame.onPath = nil
        end
        context.pathFrame = frame
        local spine = frame and frame.onPath and frame.spine or nil
        local gap, isVehicle, hx, hy, hz, hitLane, gkind, gelement, gcentral = forwardGap(vehicle,
            position, heading, context.speed, nil, curve, spine)
        -- Неподвижную помеху запоминаем всегда; машины и людей — нет, они уйдут. Раньше
        -- человек у дороги попадал в память на всю смену как столб.
        local side = gap and hitLane or nil
        local gx, gy = gap and hx or nil, gap and hy or nil
        if gap and gkind == "static" and finite(hx) then
            if rememberObstacle(hx, hy, hz, now) then
                emit("obstacle_learned", {x = round(hx), y = round(hy), z = round(hz),
                    total = #OBSTACLES}, true)
            elseif lowHit(hx, hy, finite(hz) and hz or position.z - 0.3) then
                -- Бордюр: помнить его незачем, а стоять перед ним — тем более.
                context.curb = gap
                gap, isVehicle, side, gx, gy, gkind = nil, nil, nil, nil, nil, nil
            end
        end
        local memo, memoSide, mx, my = rememberedGap(position, heading,
            clamp(brakeDistance(context.speed, 0) + 22, 18, 85), curve)
        if memo and (not gap or memo < gap) then
            gap, isVehicle, side, gx, gy, gkind = memo, false, memoSide, mx, my, "static"
            -- Память помнит точку попадания (край коробки); если по той же помехе бьют и
            -- центральные лучи — она по оси.
            gcentral = (math.abs(memoSide) < 0.9 and memo) or (gcentral and gcentral <= memo + 1.5 and gcentral) or nil
            context.obstacleRemembered = true
        end
        local near, nearSide, nearKind, nearElement = nearVehicleGap(vehicle, position, heading)
        if near and (not gap or near < gap) then
            gap, side, gx, gy, gkind, gelement = near, nearSide, nil, nil, nearKind, nearElement
            gcentral = near
            isVehicle = nearKind == "vehicle"
        end
        -- Помеха мигает (край забора, тонкий столб — луч ловит через кадр): держим последнюю
        -- 0.3 с за вычетом пройденного, иначе бот в тот же кадр снова давал газ.
        if gap then
            state.lastGap = {gap = gap, side = side, vehicle = isVehicle, at = now, x = gx, y = gy,
                kind = gkind, element = gelement}
        elseif state.lastGap and elapsed(now, state.lastGap.at) < 300 then
            local travelled = toMs(context.speed) * elapsed(now, state.lastGap.at) / 1000
            gap = math.max(0, state.lastGap.gap - travelled)
            side, isVehicle = state.lastGap.side, state.lastGap.vehicle
            gx, gy = state.lastGap.x, state.lastGap.y
            gkind, gelement = state.lastGap.kind, state.lastGap.element
        end
        context.obstacle, context.obstacleVehicle, context.obstacleSide = gap, isVehicle, side
        context.obstacleCentral = gap and gcentral or nil
        -- Машину разбираем: едет, стоит с водителем или брошена.
        if gap and gkind == "vehicle" and gelement ~= nil and valid(gelement) then
            gkind = Smart.vehicleKind(gelement, now)
        elseif gkind == "vehicle" then
            gkind = "queue"
        end
        context.obstacleKind = gap and (gkind or "static") or nil
        context.obstacleX, context.obstacleY = gap and gx or nil, gap and gy or nil
        -- Для скана выхода из тупика — где машина сейчас.
        Smart.vehicle, Smart.position, Smart.heading = vehicle, position, heading
        -- Во время отката щупаем задний бампер по дуге отката.
        if controller.nudgeUntil and controller.nudgeSteer then
            context.nudgeFree = Smart.sweepFree(vehicle, position, heading,
                Smart.curvature(controller.nudgeSteer), 3, false)
        end
        if controller.escapeUntil then
            context.rearGap = Smart.sweepFree(vehicle, position, heading,
                Smart.curvature(controller.escapeSide or 0), 4, true)
        end
        context.squeeze = squeezeShift(position, heading, curve)
        -- Соседние полосы щупаем только когда впереди действительно мешают.
        if finite(context.obstacle) and context.obstacle < 40 then
            context.gapLeft = forwardGap(vehicle, position, heading, context.speed, -LANE, curve, spine)
            context.gapRight = forwardGap(vehicle, position, heading, context.speed, LANE, curve, spine)
        end
        -- Борта щупаем всегда: боковой зацеп случается именно там, где впереди чисто.
        context.sideLeft = sideGap(vehicle, position, heading, -1)
        context.sideRight = sideGap(vehicle, position, heading, 1)
        context.light = trafficAhead(position, heading)
        -- Машина с водителем стоит 5 с: перед красным это очередь, иначе — брошена.
        if context.obstacleKind == "stalled" then
            context.obstacleKind = (context.light and not context.light.green) and "queue" or "parked"
            -- С водителем, но стоит 15 с (при светофорах 35 с): объезд и выход сканом — как у
            -- брошенной (скан теперь держит машину на дороге). Раньше за 5 с такой становилась
            -- очередь на красный — у точки 39 бот полез через траву и встречку.
            context.obstacleOccupied = context.obstacleKind == "parked"
        end
        -- Звено цепочки. Считается только когда ведёт бот: пока водитель сам ездит и
        -- ставит метки, цепочка не должна помечать их пройденными.
        local step = state.bot and #plan.items > 0 and context.index
            and planStep(position, heading, context.index, target, context.speed) or nil
        context.step = step
        if step then
            state.pathSlot = step.item.kind == "server" and step.item.index - 1 or step.item.slot
        end
        state.pathRail = step ~= nil and step.rail ~= nil
        -- Руль — на точку пути в ~1 с впереди. Рельсы депо (выезд, въезд) — по-старому.
        if step and not step.rail and frame and frame.near then
            local ax, ay, lead = Path.aim(frame.s, context.speed)
            step.aim = {x = ax, y = ay, z = position.z}
            context.pathLead = lead
            context.pathSpeed = Path.speedAhead(frame.s, context.speed)
        end
        -- Неподвижная помеха за своей меткой, мимо которой путь не идёт, — не наша.
        if finite(context.obstacle) and not context.obstacleVehicle and finite(gx)
            and beyondMark(step, position, gx, gy) then
            context.obstacleBeyond = round(context.obstacle, 1)
            context.obstacle, context.obstacleSide = nil, nil
            context.gapLeft, context.gapRight = nil, nil
        end
        applyStepShift(step, position, heading, context)
        if step and step.item.kind == "mark" then
            context.waypoint = step.aim
            context.waypointGap = step.distance
            context.waypointIndex = step.item.mark
            context.waypointRadius = step.item.r
            context.waypointFinal = step.final
            context.waypointKind = step.label
        end
        -- Без цепочки (маршрут ещё не выдан) — старый поиск меток.
        local own, ownGap, ownIndex
        if not step then own, ownGap, ownIndex = waypointNext(position, heading, context.distance) end
        -- Ведём к тому, что ближе: своей метке или контрольной точке сервера. Метки
        -- расставлены между точками, поэтому получается естественный порядок
        -- «метка → метка → точка», и ни одна серверная точка не пропускается.
        -- Точка, до которой меньше 9 м, уже внутри зоны зачёта (сфера 7 м) и вот-вот
        -- будет взята — в соревновании за «кто ближе» она не участвует, иначе бот
        -- доворачивал бы в неё вместо того, чтобы ехать по разметке.
        local pointGap = context.distance
        if finite(pointGap) and pointGap < 9 then pointGap = nil end
        if own and finite(pointGap) and ownGap > pointGap + 3 then
            own = nil
            context.waypointWaiting = ownIndex
        end
        if own then
            context.waypoint = waypointAim(own, position, heading)
            context.waypointGap = ownGap
            context.waypointIndex = ownIndex
            context.waypointRadius = own[4] or K.WAYPOINT_MID
            context.waypointFinal = ownIndex == #WAYPOINTS
            context.waypointKind = string.format("метка #%d/%d (r%d)%s", ownIndex or 0,
                #WAYPOINTS, math.floor(context.waypointRadius),
                context.waypointFinal and " ФИНИШ" or "")
        elseif not step and not context.light and not trafficWorking() then
            -- Цепочки нет и своих меток нет — сгодятся метки выключенных светофоров.
            local waypoint, gap = trafficWaypoint(position, heading, context.distance)
            if waypoint then
                context.waypoint, context.waypointGap = waypoint, gap
                context.waypointKind = "светофор"
            end
        end
    end

    recordRoute(context)
    trackCheckpoints(now, context, position)
    if target then state.lastTarget = target end

    if state.bot then
        if not context.inVehicle then
            if state.owned then releaseControls() end
            state.status = "Бот ждёт: вы не за рулём рабочей машины"
        elseif not context.route or not context.index then
            -- Сервер закрыл маршрут — это ещё не конец пути. Пока остались свои метки,
            -- бот доезжает по ним до финишной: именно там место сдачи и сигнал.
            -- Цепочка живёт и после закрытия маршрута: в ней остались метки после
            -- последней серверной точки. Предела в 200 м здесь нет — метки и есть путь.
            local rest, restGap, restIndex, aim, followUp, restFinal, steer
            local step = position and #plan.items > 0 and planStep(position, heading, nil, nil, context.speed) or nil
            -- Въезд в депо после сдачи маршрута — тоже рельсы, со сдвигом мимо ворот.
            applyStepShift(step, position, heading, context)
            if step and step.item.kind == "mark" then
                rest, aim, restGap, steer = step.item, step.target, step.distance, step.aim
                restIndex, followUp, restFinal = step.item.mark, step.next, step.final
                context.waypointKind = step.label
            elseif not step and #plan.items == 0 then
                -- Цепочки нет (бот запущен без маршрута) — идём по меткам от ближайшей.
                -- Если цепочка была и кончилась, старый поиск не включаем: он увёл бы
                -- машину к первой метке маршрута.
                rest, restGap, restIndex = waypointNext(position, heading)
                if rest then
                    aim = waypointAim(rest, position, heading)
                    restFinal = restIndex == #WAYPOINTS
                    local after = WAYPOINTS[restIndex + 1]
                    followUp = after and {x = after[1], y = after[2]} or nil
                    context.waypointKind = string.format("метка #%d/%d%s", restIndex,
                        #WAYPOINTS, restFinal and " ФИНИШ" or "")
                end
            end
            if rest and position then
                context.distance = restGap
                context.waypointIndex = restIndex
                context.waypointFinal = restFinal == true
                local out = controller:update({
                    position = position, heading = heading, speed = context.speed,
                    target = aim, distance = restGap, frozen = frozen,
                    nextTarget = followUp, steerTarget = steer,
                    afterTarget = step and step.after or nil,
                    obstacle = context.obstacle, spray = context.spray,
                    obstacleVehicle = context.obstacleVehicle,
                    obstacleKind = context.obstacleKind, rearGap = context.rearGap,
                    scanEscape = Smart.scan, nudgeFree = context.nudgeFree,
                    obstacleX = context.obstacleX, obstacleY = context.obstacleY,
                    obstacleSide = context.obstacleSide, curb = context.curb,
                    squeeze = context.squeeze, reversing = finite(context.forward)
                        and context.forward < -0.8,
                    yield = elapsed(now, state.yieldAt) < 2500,
                    sharp = step ~= nil and step.sharp == true,
                    gapLeft = context.gapLeft, gapRight = context.gapRight,
                    sideLeft = context.sideLeft, sideRight = context.sideRight,
                    light = context.light, limit = state.speedLimit,
                    jitter = state.jitter, stopAt = context.waypointFinal == true, arrive = 2,
                    avoid = "toward",
                    -- Въезд в депо по рельсу — те же 15–20, что и на выезде: там узкие ворота.
                    depot = step ~= nil and step.final and step.distance < 80,
                    depotSpeed = state.depotSpeed,
                }, now)
                state.lastOut = out
                applySpray(vehicle, out.spray, now)
                if frozen then
                    if state.owned then releaseControls() end
                    state.status = "Машина заморожена сервером"
                else
                    local ok, failedName = applyControls(out)
                    if not ok then stopBot("ошибка управления: " .. tostring(failedName)) end
                end
                if out.reason == "arrived" and context.waypointFinal and not state.finished
                and (context.speed or 0) <= 0.5 then
                    state.finished = true
                    read("dfPlayAlertSignal")
                    note("Финиш: приехал в последнюю метку, рейс завершён")
                    emit("route_finished", {waypoint = restIndex, trips = state.trips}, true)
                    stopBot("финишная метка достигнута")
                elseif out.reason == "arrived" and context.waypointFinal then
                    -- Место сдачи: стоим на тормозе до полного нуля, потом сигнал.
                    state.status = string.format("Финиш: торможу до нуля (%d)",
                        math.floor(context.speed or 0))
                else
                    state.status = string.format("Маршрут сдан, иду по меткам: #%d/%d, %d м",
                        restIndex, #WAYPOINTS, math.floor(restGap))
                end
                if state.debug then
                    state.debugData = {position = position, target = aim, heading = heading,
                        aim = debugAim(position, steer or aim, (out.detail or {}).offset),
                        curve = context.curve, squeeze = context.squeeze,
                        detour = (out.detail or {}).detour,
                        speed = context.speed, goal = (out.detail or {}).goal,
                        steer = out.steer, offset = (out.detail or {}).offset,
                        avoidSide = 0, obstacle = context.obstacle,
                        gapLeft = context.gapLeft, gapRight = context.gapRight,
                        reason = out.reason, waypointKind = context.waypointKind,
                        waypointGap = restGap, sideLeft = context.sideLeft,
                        sideRight = context.sideRight}
                end
            else
                -- Метки кончились: либо стоим, либо едем на базу.
                local home = state.homeIntent and position
                    and homePoint(state.homeLocation, position) or nil
                if home then
                context.distance = math.sqrt((home.x - position.x) ^ 2
                    + (home.y - position.y) ^ 2)
                -- К базе тоже ведём через свои метки: прямая туда часто идёт по газону.
                local lead, leadGap, leadIndex = waypointNext(position, heading)
                local homeTarget, stopAt = home, true
                if lead then
                    homeTarget, stopAt = waypointAim(lead, position, heading), false
                    context.waypointKind = string.format("метка #%d", leadIndex or 0)
                    context.distance = leadGap
                    context.waypointKind = "своя"
                end
                local out = controller:update({
                    position = position, heading = heading, speed = context.speed,
                    target = homeTarget, distance = context.distance, frozen = frozen,
                    obstacle = context.obstacle, spray = context.spray, stopAt = stopAt,
                    obstacleSide = context.obstacleSide, curb = context.curb,
                    squeeze = context.squeeze, reversing = finite(context.forward)
                        and context.forward < -0.8,
                    sideLeft = context.sideLeft, sideRight = context.sideRight,
                    limit = state.speedLimit,
                }, now)
                state.lastOut = out
                applySpray(vehicle, out.spray, now)
                if frozen then
                    if state.owned then releaseControls() end
                    state.status = "Машина заморожена сервером"
                else
                    local ok, failedName = applyControls(out)
                    if not ok then stopBot("ошибка управления: " .. tostring(failedName)) end
                end
                if out.reason == "arrived" then
                    arriveHome(now)
                    state.status = state.homeIntent == "return"
                        and "На базе: сдаю машину" or "На базе: жду маршрут"
                else
                    state.status = string.format("Возвращаюсь на базу: %d м",
                        math.floor(context.distance))
                end
                else
                    if state.owned then releaseControls() end
                    state.status = "Бот ждёт: маршрут не назначен"
                end
            end
        elseif not position or not target then
            if state.owned then releaseControls() end
            state.status = "Бот ждёт: не читается точка маршрута"
        else
            -- Следующую точку берём из таблицы только когда она согласована с живой
            -- целью: иначе предсказанный поворот увёл бы скорость не туда.
            local nextTarget = nil
            if not context.tableGap or context.tableGap < 12 then
                nextTarget = pointAt(context.index + 1)
            end
            -- Точка уже под колёсами (сфера зачёта 7 м): доворачивать к ней нельзя —
            -- машина просто не впишется и обдерёт борт о столб на углу. Смотрим дальше.
            if context.distance and context.distance < 9 and nextTarget then
                target, nextTarget = nextTarget, pointAt(context.index + 2)
                local ndx, ndy = target.x - position.x, target.y - position.y
                context.distance = math.sqrt(ndx * ndx + ndy * ndy)
                context.lookedAhead = true
            end
            -- Метка светофора по курсу ближе цели — ведём машину через неё, а сама
            -- контрольная точка становится «следующей»: так бот не срезает перекрёсток.
            local aimTarget, aimNext, steerTarget = target, nextTarget, nil
            local stopAtTarget = false
            if context.step then
                -- Цепочка уже всё решила: текущее звено, следующее (для торможения
                -- заранее) и нужно ли в нём встать. Точку под колёсами она тоже учла.
                aimTarget = context.step.target
                steerTarget = context.step.aim
                aimNext = context.step.next
                context.distance = context.step.distance
                stopAtTarget = context.step.final
            elseif context.waypoint then
                aimTarget = context.waypoint
                context.distance = context.waypointGap
                -- Следующей целью даём СЛЕДУЮЩУЮ МЕТКУ, а не контрольную точку: только
                -- так бот заранее видит, куда поворачивать после текущей, успевает
                -- сбросить скорость и вписаться. Метки кончились — тогда точка сервера.
                local followUp = WAYPOINTS[(context.waypointIndex or 0) + 1]
                aimNext = followUp and {x = followUp[1], y = followUp[2], z = position.z}
                    or target
                -- В финишной метке надо встать, а не проехать её насквозь.
                stopAtTarget = context.waypointFinal == true
            end
            local out = controller:update({
                position = position, heading = heading, speed = context.speed,
                target = aimTarget, nextTarget = aimNext, steerTarget = steerTarget,
                afterTarget = context.step and context.step.after or nil,
                distance = context.distance, frozen = frozen,
                finishing = context.index >= context.points,
                obstacle = context.obstacle, spray = context.spray,
                obstacleVehicle = context.obstacleVehicle,
                obstacleKind = context.obstacleKind, rearGap = context.rearGap,
                scanEscape = Smart.scan, nudgeFree = context.nudgeFree,
                obstacleX = context.obstacleX, obstacleY = context.obstacleY,
                obstacleSide = context.obstacleSide, curb = context.curb,
                squeeze = context.squeeze, reversing = finite(context.forward)
                    and context.forward < -0.8,
                yield = elapsed(now, state.yieldAt) < 2500,
                sharp = context.step ~= nil and context.step.sharp == true,
                pathSpeed = context.pathSpeed, obstacleOccupied = context.obstacleOccupied,
                obstacleCentral = context.obstacleCentral,
                liters = context.liters, routeLeft = context.routeLeft,
                light = context.light, limit = state.speedLimit,
                jitter = state.jitter, stopAt = stopAtTarget, arrive = 2,
                -- Пока сервер не засчитал первую точку, мы выезжаем из депо.
                depot = context.index == 1, depotSpeed = state.depotSpeed,
                avoid = context.index == 1 and "none" or ((context.step ~= nil
                    and context.step.item.kind == "mark") and "toward" or nil),
                gapLeft = context.gapLeft, gapRight = context.gapRight,
                sideLeft = context.sideLeft, sideRight = context.sideRight,
            }, now)
            state.lastOut = out
            applySpray(vehicle, out.spray, now)
            -- Финиш: встали в последней метке — сигнал и стоп, рейс отработан.
            if out.reason == "arrived" and context.waypointFinal and not state.finished
                and (context.speed or 0) <= 0.5 then
                state.finished = true
                read("dfPlayAlertSignal")
                note("Финиш: приехал в последнюю метку, рейс завершён")
                emit("route_finished", {waypoint = context.waypointIndex,
                    trips = state.trips}, true)
                stopBot("финишная метка достигнута")
            end
            if state.debug then
                local detail = out.detail or {}
                state.debugData = {position = position, target = aimTarget,
                    aim = debugAim(position, steerTarget or aimTarget, detail.offset),
                    curve = context.curve, squeeze = context.squeeze,
                    railShift = context.railShift, detour = detail.detour,
                    heading = heading, speed = context.speed, goal = detail.goal,
                    steer = out.steer, offset = detail.offset,
                    avoidSide = detail.avoid_side or 0, obstacle = context.obstacle,
                    gapLeft = context.gapLeft, gapRight = context.gapRight,
                    reason = out.reason,
                    waypointKind = context.waypointKind, waypointGap = context.waypointGap,
                    waypointCount = #WAYPOINTS, pathFrame = context.pathFrame,
                    pathLead = context.pathLead, pathSpeed = context.pathSpeed,
                    sideLeft = context.sideLeft, sideRight = context.sideRight}
            end
            if frozen then
                if state.owned then releaseControls() end
                state.status = "Машина заморожена сервером"
            else
                local ok, failedName = applyControls(out)
                if not ok then stopBot("ошибка управления: " .. tostring(failedName)) end
            end
        end
    elseif state.owned then
        releaseControls()
    end

    if state.recording then
        recordSample(now, context, position, heading, frozen)
        -- Сброс на диск раз в 2 с: краш игры не должен съедать эталонный проезд.
        if elapsed(now, state.flushTick) >= 2000 then
            state.flushTick = now
            flush(false)
        end
    end

    -- Пока маршрут активен, помним его локацию: именно к ней возвращаться за следующим.
    if context.route and context.route ~= state.lastRoute then
        -- Новый маршрут — начинаем с ближайшей метки.
        state.finished = nil
        state.waypointCursor = waypointStart(position)
    end
    if context.route then
        state.homeLocation = locationOfRoute(context.route) or state.homeLocation
        if state.homeIntent == "route" then state.homeIntent = nil end
    end
    if not context.vehicle and state.homeIntent == "return" then state.homeIntent = nil end
    if not state.homeIntent then state.homeNoted = nil end

    if not context.route and state.lastRoute then
        state.trips = state.trips + 1
        state.lastIndex, state.lastTarget = nil, nil
        note("Маршрут завершён или сброшен")
        flush(true)
    end
    state.lastRoute = context.route

    if elapsed(now, state.uiTick) >= 200 then
        state.uiTick = now
        if state.autonomy and Auto.stage and Auto.stage ~= "drive" and not state.bot then
            state.status = "Автономность: " .. (Auto.TEXT[Auto.stage] or Auto.stage)
        end
        pushUi(context)
    end
end

-- === установка обработчиков ===

-- === отладка на экране ===
-- Рисуем то, по чему бот принимает решения: цель со смещением, лучи и числа.
-- Цвет через обёртку: если tocolor почему-то недоступен, отладка не должна падать
-- целиком и молча уносить с собой отрисовку меток.
local function rgba(r, g, b, a)
    local fn = _G.tocolor
    if type(fn) ~= "function" then return 4294967295 end
    local ok, value = pcall(fn, r, g, b, a)
    return ok and value or 4294967295
end

-- Метки проезда: столбик в центре, кольцо по радиусу зоны и номер над меткой. Рисуются
-- и без бота. Высоту берём от земли под самой меткой, а не от машины: издалека кольцо
-- иначе висело в воздухе или уходило под асфальт.
local function markGround(point, origin)
    local ground = read("getGroundPosition", point[1], point[2], origin.z + 30)
    if finite(ground) and math.abs(ground - origin.z) < 40 then return ground end
    return origin.z - 1
end

local function drawWaypoints(origin)
    local selected = state.editor and state.editSel or state.lastWaypoint
    for index, point in ipairs(WAYPOINTS) do
        local distance = math.sqrt((point[1] - origin.x) ^ 2 + (point[2] - origin.y) ^ 2)
        if distance < 150 then
            local radius = point[4] or K.WAYPOINT_MID
            local active = index == selected
            local colour = active and rgba(255, 210, 90, 235) or rgba(120, 255, 140, 200)
            local z = markGround(point, origin)
            read("dxDrawLine3D", point[1], point[2], z,
                point[1], point[2], z + 4, colour, active and 7 or 5)
            local steps = active and 36 or 20
            local px, py
            for step = 0, steps do
                local a = step / steps * math.pi * 2
                local x = point[1] + math.cos(a) * radius
                local y = point[2] + math.sin(a) * radius
                if px then
                    read("dxDrawLine3D", px, py, z + 0.2, x, y, z + 0.2, colour, active and 4 or 2)
                end
                px, py = x, y
            end
            if active then
                -- Стрелка — куда ехал водитель, когда ставил метку.
                local r = math.rad(point[3] or 0)
                local fx, fy = -math.sin(r), math.cos(r)
                local tip = math.max(radius, 3)
                read("dxDrawLine3D", point[1], point[2], z + 0.3,
                    point[1] + fx * tip, point[2] + fy * tip, z + 0.3, colour, 5)
            end
            local sx, sy = read("getScreenFromWorldPosition", point[1], point[2], z + 4.4)
            if finite(sx) and finite(sy) then
                read("dxDrawText", string.format("#%d  %g м", index, radius), sx, sy, sx, sy,
                    colour, active and 1.6 or 1.2, "default-bold", "center", "bottom")
            end
        end
    end
end

local function onDebugRender()
    if not state.debug and not state.editor then return end
    -- Позицию берём прямо из машины: метки надо видеть и с выключенным ботом,
    -- иначе размечать маршрут приходится вслепую — именно так и терялись метки.
    local vehicle = jobVehicle() or occupied() or localPlayer
    local px, py, pz
    if vehicle then px, py, pz = read("getElementPosition", vehicle) end
    if not finite(px) or not finite(py) then return end
    local here = {x = px, y = py, z = finite(pz) and pz or 0}
    local snapshot = state.debugData
    if not state.debug or not state.bot or not snapshot then
        -- Бот не ведёт машину или открыт только эдитор: метки и короткий статус.
        drawWaypoints(here)
        local point = state.editor and state.editSel and WAYPOINTS[state.editSel]
        local line = state.editor
            and (point and string.format("ЭДИТОР МЕТОК | #%d, зона %g м | меток: %d",
                state.editSel, point[4] or K.WAYPOINT_MID, #WAYPOINTS)
                or string.format("ЭДИТОР МЕТОК | метка не выбрана | меток: %d", #WAYPOINTS))
            or string.format("ОТЛАДКА | меток: %d | бот выключен", #WAYPOINTS)
        read("dxDrawText", line, 22, 240, 0, 0, rgba(255, 255, 255, 235), 1.15, "default-bold")
        return
    end
    local position, target = snapshot.position, snapshot.target
    if not position then return end
    if target then
        read("dxDrawLine3D", position.x, position.y, position.z + 0.5,
            target.x, target.y, position.z + 0.5, rgba(120, 220, 255, 220), 3)
    end
    if snapshot.aim then
        read("dxDrawLine3D", position.x, position.y, position.z + 0.8,
            snapshot.aim.x, snapshot.aim.y, position.z + 0.8, rgba(255, 190, 60, 230), 4)
    end
    -- Дуга, по которой машина поедет и по которой бот ищет помехи: середина и оба борта.
    -- Зелёная — чисто; красная — обрывается на помехе. Прямые лучи по курсу больше не
    -- рисуем: в повороте они смотрят туда, куда машина уже не едет.
    local curve = finite(snapshot.curve) and snapshot.curve or 0
    local reach = clamp(brakeDistance(snapshot.speed or 0, 0) + 22, 18, 85)
    local blocked = finite(snapshot.obstacle)
    local length = blocked and math.min(snapshot.obstacle, reach) or reach
    local colour = blocked and rgba(255, 90, 110, 220) or rgba(90, 230, 160, 170)
    -- Путь по меткам на 150 м вперёд — голубым, как у самолёта линия полёта; лучи помех идут
    -- вдоль него (зелёные — чисто, красные — до помехи).
    local frame = snapshot.pathFrame
    if frame and Path.n > 1 then
        local lx, ly, lz
        -- Высота земли под точкой пути запоминается (путь неподвижен): 51 запрос земли за кадр
        -- съедал ФПС (заезд 29.09 с включённой отладкой).
        Path.z = Path.z or {}
        for d = 0, 150, 4 do
            local x, y, k = Path.at(frame.s + d, frame.i)
            local z = Path.z[k]
            if not z then
                local ground = read("getGroundPosition", Path.x[k], Path.y[k], position.z + 15)
                if finite(ground) and math.abs(ground - position.z) < 20 then
                    z = ground
                    Path.z[k] = ground
                end
            end
            z = (z or position.z - 1) + 0.25
            if lx then read("dxDrawLine3D", lx, ly, lz, x, y, z, rgba(80, 200, 255, 230), 5) end
            lx, ly, lz = x, y, z
        end
    end
    local spine = frame and frame.onPath and frame.spine
    for _, lane in ipairs(spine and {0, -RAY_EDGE, RAY_EDGE} or {}) do
        local walked, lx, ly = 0, nil, nil
        for k = 1, #spine do
            local p = spine[k]
            local x, y = p[1] + p[3] * lane, p[2] + p[4] * lane
            if lx then
                local piece = math.sqrt((x - lx) ^ 2 + (y - ly) ^ 2)
                local stop = walked + piece > length
                if stop and piece > 0 then
                    local f = (length - walked) / piece
                    x, y = lx + (x - lx) * f, ly + (y - ly) * f
                end
                read("dxDrawLine3D", lx, ly, position.z - 0.3, x, y, position.z - 0.3, colour, lane == 0 and 3 or 1)
                walked = walked + piece
                if stop then break end
            end
            lx, ly = x, y
        end
    end
    for _, lane in ipairs(spine and {} or {0, -RAY_EDGE, RAY_EDGE}) do
        local lx, ly
        for k = 0, 10 do
            local x, y, h = arcPose(noseOf(position, snapshot.heading or 0),
                snapshot.heading or 0, curve, length * k / 10)
            local r = math.rad(h)
            x, y = x + math.cos(r) * lane, y + math.sin(r) * lane
            if lx then
                read("dxDrawLine3D", lx, ly, position.z - 0.3, x, y, position.z - 0.3,
                    colour, lane == 0 and 3 or 1)
            end
            lx, ly = x, y
        end
    end
    drawWaypoints(position)
    local lines = {
        string.format("цель %.0f, скорость %.0f, лимит %d", snapshot.goal or 0,
            snapshot.speed or 0, state.speedLimitSpeedo or 0),
        string.format("ведёт через: %s%s", snapshot.waypointKind or "контрольную точку",
            finite(snapshot.waypointGap)
                and string.format(" (%.0f м)", snapshot.waypointGap) or ""),
        string.format("руль %.2f, сдвиг прицела %+.1f м%s", snapshot.steer or 0,
            snapshot.offset or 0,
            snapshot.avoidSide ~= 0 and " (ОБЪЕЗД по полосе)"
                or (finite(snapshot.detour) and snapshot.detour ~= 0) and " (обход после отката)"
                or (finite(snapshot.squeeze) and snapshot.squeeze ~= 0) and " (мимо помехи)"
                or (finite(snapshot.railShift) and snapshot.railShift ~= 0)
                    and string.format(" (рельс %+.1f м)", snapshot.railShift) or ""),
        string.format("дуга: %s", math.abs(finite(snapshot.curve) and snapshot.curve or 0) < 1 / 250
            and "прямо" or string.format("радиус %.0f м %s", 1 / math.abs(snapshot.curve),
            snapshot.curve > 0 and "влево" or "вправо")),
        string.format("помеха %s | слева %s | справа %s",
            finite(snapshot.obstacle) and string.format("%.0f м", snapshot.obstacle) or "чисто",
            finite(snapshot.gapLeft) and string.format("%.0f м", snapshot.gapLeft) or "чисто",
            finite(snapshot.gapRight) and string.format("%.0f м", snapshot.gapRight) or "чисто"),
        string.format("борта: слева %s | справа %s",
            finite(snapshot.sideLeft) and string.format("%.1f м", snapshot.sideLeft) or "чисто",
            finite(snapshot.sideRight) and string.format("%.1f м", snapshot.sideRight) or "чисто"),
        string.format("режим: %s", snapshot.reason or "?"),
        frame and string.format("путь: %s, в стороне %+.1f м, прицел %.0f м, изгибы пускают %s",
            frame.onPath and "по пути" or (frame.near and "возвращаюсь" or "далеко — к цели"),
            frame.lateral or 0, snapshot.pathLead or 0,
            finite(snapshot.pathSpeed) and string.format("%.0f", snapshot.pathSpeed) or "без ограничения")
            or "путь: не построен",
    }
    local y = 240
    for _, line in ipairs(lines) do
        read("dxDrawText", line, 22, y, 0, 0, rgba(255, 255, 255, 235), 1.15, "default-bold")
        y = y + 20
    end
end

local function guard(fn, name)
    return function(...)
        local ok, err = pcall(fn, ...)
        if not ok then
            state.failures[name] = tostring(err):sub(1, 200)
            emit("handler_error", {handler = name, error = tostring(err)}, true)
        end
    end
end

local function hook(name, element, fn)
    local callback = guard(fn, name)
    local ok = read("addEventHandler", name, element, callback)
    state.hookStatus[name] = ok == true
    if ok then state.hooks[#state.hooks + 1] = {name, element, callback} end
end

hook("onClientPreRender", root, onFrame)
hook("onClientRender", root, onDebugRender)
hook("onClientKey", root, function(key, pressed)
    if not pressed or not (state.bot or (state.autonomy and Auto.stage)) then return end
    if read("isChatBoxInputActive") == true or read("isConsoleActive") == true then return end
    if read("dfMenuOpen") == true then return end
    -- Руль или газ от игрока — значит он забирает управление себе. Пешком то же самое:
    -- иначе автономия и игрок тянули бы персонажа в разные стороны.
    if key == "w" or key == "s" or key == "a" or key == "d"
        or key == "arrow_u" or key == "arrow_d" or key == "arrow_l" or key == "arrow_r" then
        if state.bot then stopBot("ручной перехват: " .. key) end
        if state.autonomy then
            state.autonomy = false
            Auto.stopWalk()
            note("Автономность выключена: управление у игрока")
        end
    end
end)
hook("onClientVehicleExit", root, function(player)
    if player == localPlayer and state.bot then stopBot("выход из машины") end
end)

-- Удары: место, где человек задел бордюр или машину, — это узкое место маршрута.
hook("onClientVehicleCollision", root, function(collider, force)
    local vehicle = jobVehicle()
    if not vehicle or source ~= vehicle then return end
    -- ДТП — как у самолёта (PilotTelemetry): пока машину ведёт бот, удар о машину, игрока
    -- или пешехода — сирена и красная строка в чат. Столб и бордюр — не ДТП. Одну и ту же
    -- машину считаем раз в 1.5 с (трение даёт событие каждый кадр), сирену — не чаще раза
    -- в 30 с.
    if state.bot and valid(collider) then
        local kind = read("getElementType", collider)
        local who = ({vehicle = "машиной", player = "игроком", ped = "пешеходом"})[kind]
        local now = getTickCount()
        state.crashContacts = state.crashContacts or setmetatable({}, {__mode = "k"})
        if who and elapsed(now, state.crashContacts[collider]) >= 1500 then
            state.crashContacts[collider] = now
            if elapsed(now, state.crashAlertAt) >= 30000 then
                state.crashAlertAt = now
                local played = read("dfPlayAlertSignal") == true
                read("outputChatBox", "#FF5555[PlowBot] #FFFFFFДТП: столкновение с " .. who,
                    255, 255, 255, true)
                emit("crash_alert", {kind = kind, played = played,
                    force = finite(force) and round(force, 1) or NULL}, true)
            end
        end
    end
    -- Упёрлись в машину спереди: 2.5 с стоим, а не толкаем. С пингом водитель нас мог не
    -- видеть, а тот, кто сдаёт назад, уедет сам. Сбоку и сзади — не наше дело тормозить.
    if state.bot and collider ~= nil and read("getElementType", collider) == "vehicle" then
        local cx, cy = read("getElementPosition", collider)
        local x, y = read("getElementPosition", vehicle)
        local _, _, rz = read("getElementRotation", vehicle)
        if finite(cx) and finite(cy) and finite(x) and finite(y) and finite(rz) then
            local r = math.rad(rz)
            if (cx - x) * -math.sin(r) + (cy - y) * math.cos(r) > 1 then
                state.yieldAt = getTickCount()
            end
        end
    end
    if not state.recording then return end
    -- Трение о столб даёт событие каждый кадр: в одном заезде 140 записей подряд, каждая
    -- с принудительным сбросом лога на диск. Пишем сильные удары всегда, мелкие — не
    -- чаще двух раз в секунду, остальные только считаем.
    local now = getTickCount()
    local strong = finite(force) and force >= 60
    if not strong and elapsed(now, state.collisionTick) < 500 then
        state.collisionMuted = (state.collisionMuted or 0) + 1
        return
    end
    state.collisionTick = now
    local x, y, z = read("getElementPosition", vehicle)
    emit("collision", {force = finite(force) and round(force, 2) or NULL,
        with = collider ~= nil and tostring(read("getElementType", collider) or "?") or NULL,
        model = collider ~= nil and (read("getElementModel", collider) or NULL) or NULL,
        position = finite(x) and {round(x), round(y), round(z)} or NULL,
        index = _G.CURRENT_POSITION_ID or NULL, muted = state.collisionMuted or 0}, true)
    state.collisionMuted = 0
end)

-- Чат сервера: предупреждения «слишком далеко от точки», «вода закончилась» и прочее.
hook("onClientChatMessage", root, function(text, r, g, b, messageType)
    if type(text) ~= "string" then return end
    Guard.chat(text, r, g, b, messageType)
    local plain = text:gsub("#%x%x%x%x%x%x", "")
    -- «Вам вернули залог 10 000» — машину сдали, рейс закрыт (списание залога при выдаче — нет).
    local lower = plain:lower()
    if lower:find("залог", 1, true) and (lower:find("верн", 1, true) or lower:find("возвра", 1, true)) then
        Auto.tripOver("вернули залог")
    end
    -- Сервер сам объявляет режим светофоров — ловим это и переключаемся автоматически.
    if plain:find("ночной режим", 1, true) then
        if not state.ignoreTraffic then
            state.ignoreTraffic = true
            note("Светофоры в ночном режиме — игнорирую")
        end
    elseif plain:find("дневной режим", 1, true) then
        if state.ignoreTraffic then
            state.ignoreTraffic = false
            note("Светофоры в дневном режиме — снова учитываю")
        end
    end
    if not state.recording then return end
    for _, word in ipairs({"точк", "Точк", "вод", "Вод", "маршрут", "Маршрут",
        "установк", "Установк", "далеко", "Далеко", "врем", "Врем", "смен", "Смен"}) do
        if plain:find(word, 1, true) then
            emit("chat", {text = plain:sub(1, 400), index = _G.CURRENT_POSITION_ID or NULL}, true)
            return
        end
    end
end)

-- Входящие события работы: по ним видно, что именно присылает сервер за проезд.
for _, name in ipairs({"snowPlow:initRoute", "snowPlow:resetRoute",
    "snowPlow:setPlayerJobVehicle", "snowPlow:syncWorkData", "showPlow:syncSprayAmmount"}) do
    local event = name
    hook(event, resourceRoot, function(a, b)
        emit("work_event", {event = event,
            a = (finite(a) or type(a) == "string") and a or tostring(a),
            b = (finite(b) or type(b) == "string") and b or (b ~= nil and tostring(b) or NULL)},
            true)
    end)
end

-- Рейс закрыт: машину сдали — сервер сбросил маршрут (snowPlow:resetRoute) или написал, что
-- вернул залог. Цепочку и путь очищаем: иначе бот, запущенный до выдачи нового маршрута, ехал по
-- оставшимся меткам прямо к месту сдачи.
function Auto.tripOver(why)
    if #plan.items == 0 and Path.n == 0 then return end
    plan.route, plan.items, plan.serverCount = nil, {}, 0
    Path.n, Path.cursor, Path.frame = 0, nil, nil
    state.planDirty, state.pathRail, state.pathSlot = nil, nil, nil
    note("Рейс закрыт: " .. why .. " — метки и путь сброшены до нового маршрута")
    emit("trip_over", {why = why}, true)
end
hook("snowPlow:resetRoute", resourceRoot, function()
    Auto.tripOver("маршрут сброшен сервером")
end)

-- Цикл автономии: выдача машины, окно сдачи, закрытие маршрута, диалог Руслана.
hook("snowPlow:setPlayerJobVehicle", resourceRoot, function()
    Auto.given = getTickCount()
end)
hook("snowPlow:openPassedWindow", localPlayer, function()
    Auto.window = getTickCount()
    emit("work_event", {event = "snowPlow:openPassedWindow"}, true)
end)
hook("snowPlow:closePassedWindow", localPlayer, function()
    Auto.window = nil
    emit("work_event", {event = "snowPlow:closePassedWindow"}, true)
end)
hook("snowPlow:completeRoute", resourceRoot, function(a)
    local x, y = nil, nil
    if type(a) == "table" then x, y = a.x or a[1], a.y or a[2] end
    emit("work_event", {event = "snowPlow:completeRoute", x = finite(x) and round(x) or NULL,
        y = finite(y) and round(y) or NULL, a = type(a) == "string" and a or tostring(a)}, true)
end)
hook("snowPlow:npc:start", resourceRoot, function(a, b, c)
    -- b — Руслан, c — страница диалога: «employment:start» — приём на работу.
    Auto.dialogKey = type(c) == "string" and c or nil
    emit("work_event", {event = "snowPlow:npc:start", a = tostring(a), b = tostring(b),
        c = tostring(c)}, true)
end)
-- После «B» пять секунд пишем, какие данные меняются у игрока и машины: так узнаем, где
-- сервер держит ремень (в дампе ресурса этого нет).
hook("onClientElementDataChange", root, function(key, old, new)
    if not Auto.watchData or getTickCount() > Auto.watchData then return end
    if source ~= localPlayer and source ~= Auto.prepared and source ~= Auto.vehicle() then return end
    emit("autonomy_data", {key = tostring(key), old = tostring(old), new = tostring(new),
        who = source == localPlayer and "player" or "vehicle"}, true)
end)

local cleanup
cleanup = function()
    if state.closed then return end
    state.closed = true
    stopBot("выгрузка скрипта")
    releaseControls()
    Auto.stopWalk()
    read("dfSetAlertMonitorEnabled", false)
    flush(true)
    for _, entry in ipairs(state.hooks) do
        read("removeEventHandler", entry[1], entry[2], entry[3])
    end
    bridge.update("loaded", "0")
    _G.__DarkFlamePlowCleanup = nil
end
hook("onClientResourceStop", resourceRoot, cleanup)
_G.__DarkFlamePlowCleanup = cleanup
-- DLL выгружает поток бота и сама: при перезагрузке скрипта (новая версия PlowBot.lua) и
-- по «Unload» во вкладке Lua Threads. Тогда тоже отпускаем руль и газ и дописываем лог —
-- иначе машина ехала бы дальше с последним нажатым газом.
if type(onUnload) == "function" then onUnload(cleanup) end

marksLoad()
bridge.update("loaded", "1")
note("PlowBot " .. VERSION .. " подключён")
note(state.marksFile)
if not state.hookStatus.onClientPreRender then
    state.status = "Ошибка: onClientPreRender не зарегистрирован"
    bridge.update("loaded", "0")
end

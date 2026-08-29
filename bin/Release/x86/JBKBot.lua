local api = {
    triggerEvent = dfTriggerEvent,
    alert = dfPlayAlertSignal,
    key = dfEmulateKey,
    mouse = dfEmulateMouseButton,
    alertMonitor = dfSetAlertMonitorEnabled,
    takeCommand = dfJbkTakeCommand,
    updateState = dfJbkUpdate,
}

for name, fn in pairs(api) do
    if type(fn) ~= "function" then
        error("DarkFlame API is missing: " .. name, 0)
    end
end

local _STATE = false
local syncOptionsBotState = function() end
local syncNativeJbkState = function() end
local cancelAutoResume = function() end
local WALK
local ADMINHIT
local startWalk = function() end
local stopWalk = function() end

local CONTROL_KEYS = {
    forwards = "w",
    sprint = "lshift",
    left = "a",
    right = "d",
    jump = "space",
}
local controlStates = {}

local function setBotControl(control, pressed)
    pressed = pressed and true or false
    if controlStates[control] == pressed then return true end
    local result
    if control == "fire" then
        result = api.mouse("left", pressed)
    elseif CONTROL_KEYS[control] then
        result = api.key(CONTROL_KEYS[control], pressed)
    else
        return false
    end
    if result then controlStates[control] = pressed end
    return result
end

local function releaseBotControls()
    for control, key in pairs(CONTROL_KEYS) do
        api.key(key, false)
        controlStates[control] = false
    end
    api.mouse("left", false)
    controlStates.fire = false
end

local Settings = {
    AllowBunnyHop = true,
    AntiAFK = true,
    BunnyHopDelay = 1000,
    BunnyHopMinDistance = 30,
    RotationSpeed = 5,
    AutoDisable = true,
    PlaySiren = true,
}

local alertTicks = {}
local function playAlert(key, cooldown)
    if not Settings.PlaySiren then return false end
    local now = getTickCount()
    cooldown = cooldown or 5000
    if alertTicks[key] and now - alertTicks[key] < cooldown then return false end
    alertTicks[key] = now
    return api.alert()
end

local STORAGE = {
    MARKERS = {},
    PATH_ID = 0,
    STORAGE_ID = 1,
    FINISH = {element = nil, type = "storage"},
    TARGET = {element = nil, index = 0},
}

local storagePoints = {
    {50.362, -242.500, 391.2578125},
    {50.362, -232.000, 391.2578125},
    {50.362, -223.500, 391.2578125},
}

local positions = {
    [1] = {14.654296875, -227.111328125, 390.20001220703},
    [2] = {10.4052734375, -223.4736328125, 390.20001220703},
    [3] = {13.275390625, -223.7451171875, 390.20001220703},
    [4] = {8.68359375, -226.546875, 390.20001220703},
    [5] = {28.2998046875, -308.9453125, 390.20001220703},
    [6] = {33.759765625, -243.962890625, 390.20001220703},
    [7] = {32.7939453125, -328.5419921875, 390.20001220703},
    [8] = {28.203125, -331.34375, 390.20001220703},
    [9] = {34.1728515625, -331.908203125, 390.20001220703},
    [10] = {29.9248046875, -328.2705078125, 390.20001220703},
    [11] = {13.4345703125, -284.1181640625, 390.20001220703},
    [12] = {10.5654296875, -283.8466796875, 390.20001220703},
    [13] = {14.814453125, -287.484375, 390.20001220703},
    [14] = {8.84375, -286.919921875, 390.20001220703},
    [15] = {32.7431640625, -273.302734375, 390.20001220703},
    [16] = {28.15234375, -276.1044921875, 390.20001220703},
    [17] = {29.8740234375, -273.0322265625, 390.20001220703},
    [18] = {34.1220703125, -276.6689453125, 390.20001220703}
}

-- Новые пути из первого скрипта
local new_traectories = {
    [1] = {
        [1] = {
            {41.087001800537, -239.83308410645, 391.2578125},
            {28.356245040894, -227.12426757812, 391.2578125}
        },
        [2] = {
            {40.597484588623, -239.85438537598, 391.2578125},
            {28.145015716553, -226.95195007324, 391.2578125},
            {13.18240737915, -222.11276245117, 391.4140625}
        },
        [3] = {
            {41.358383178711, -240.04502868652, 391.2578125},
            {27.794250488281, -226.60823059082, 391.2578125}
        },
        [4] = {
            {41.356792449951, -239.96162414551, 391.2578125},
            {28.044075012207, -226.23249816895, 391.2578125},
            {12.724277496338, -221.69270324707, 391.2578125},
            {9.0413026809692, -222.18544006348, 391.2578125}
        },
        [5] = {
            {42.344375610352, -239.94961547852, 391.2578125},
            {34.595687866211, -249.24639892578, 391.2578125},
            {27.659336090088, -272.94598388672, 391.2578125},
            {27.819087982178, -306.01553344727, 391.2578125}
        },
        [6] = {
            {41.832035064697, -239.9423828125, 391.2578125}
        },
        [7] = {
            {42.294086456299, -240.18312072754, 391.2578125},
            {37.779136657715, -245.66888427734, 391.2578125},
            {35.267971038818, -263.27154541016, 391.2578125}
        },
        [8] = {
            {40.833667755127, -239.98506164551, 391.2578125},
            {36.0339012146, -245.85079956055, 391.2578125},
            {28.006986618042, -271.71008300781, 391.2578125},
            {27.75291633606, -327.4436340332, 391.2578125}
        },
        [9] = {
            {40.901363372803, -239.91513061523, 391.2578125},
            {37.682144165039, -244.25148010254, 391.2578125},
            {35.447765350342, -258.91177368164, 391.2578125},
            {34.039691925049, -329.43478393555, 391.2578125}
        },
        [10] = {
            {41.149597167969, -239.80958557129, 391.2578125},
            {35.396938323975, -247.96961975098, 391.2578125},
            {34.146724700928, -282.25100708008, 391.2578125},
            {27.865116119385, -295.47686767578, 391.2578125},
            {28.156101226807, -324.99197387695, 391.2578125}
        },
        [11] = {
            {41.374877929688, -240.08079528809, 391.2578125},
            {33.54899597168, -250.56532287598, 391.2578125},
            {25.79610824585, -261.35153198242, 391.2578125},
            {23.031658172607, -265.24633789062, 391.2578125}
        },
        [12] = {
            {41.048309326172, -240.06695556641, 391.2578125},
            {31.465339660645, -255.38941955566, 391.2578125},
            {25.363389968872, -262.34872436523, 391.2578125},
            {23.05361366272, -265.06982421875, 391.2578125}
        },
        [13] = {
            {40.966579437256, -240.03254699707, 391.2578125},
            {31.657535552979, -251.05856323242, 391.2578125},
            {25.065023422241, -261.4801940918, 391.2578125},
            {22.986492156982, -265.03588867188, 391.2578125}
        },
        [14] = {
            {41.117622375488, -240.12075805664, 391.2578125},
            {31.23738861084, -252.97479248047, 391.2578125},
            {25.301797866821, -261.4807434082, 391.2578125},
            {23.005262374878, -265.03469848633, 391.2578125},
            {9.4350576400757, -281.26391601562, 391.2578125}
        },
        [15] = {
            {41.289798736572, -240.02139282227, 391.2578125},
            {36.945949554443, -246.38380432129, 391.2578125}
        },
        [16] = {
            {41.414657592773, -240.09637451172, 391.2578125},
            {36.637401580811, -246.10354614258, 391.2578125},
            {28.853342056274, -270.07870483398, 391.2578125}
        },
        [17] = {
            {41.378032684326, -239.94631958008, 391.2578125},
            {37.166751861572, -243.88194274902, 391.2578125}
        },
        [18] = {
            {41.537826538086, -239.8013458252, 391.2578125},
            {38.753368377686, -243.62083435059, 391.2578125}
        }
    },
    [2] = {
        [1] = {
            {41.499664306641, -231.99174499512, 391.2578125},
            {27.945171356201, -226.08892822266, 391.2578125}
        },
        [2] = {
            {41.342517852783, -231.8399810791, 391.2578125},
            {15.192231178284, -222.07646179199, 391.2578125}
        },
        [3] = {
            {41.076457977295, -231.52578735352, 391.2578125},
            {27.724369049072, -226.65208435059, 391.2578125}
        },
        [4] = {
            {41.750946044922, -231.72080993652, 391.2578125},
            {27.079736709595, -226.04844665527, 391.2578125},
            {9.1334257125854, -221.39865112305, 391.2578125}
        },
        [5] = {
            {41.634418487549, -231.68551635742, 391.2578125},
            {39.023105621338, -240.05274963379, 391.2578125},
            {35.064212799072, -281.91116333008, 391.2578125},
            {27.491138458252, -296.51458740234, 391.2578125}
        },
        [6] = {
            {41.226596832275, -231.64555358887, 391.2578125},
            {36.28296661377, -232.88952636719, 391.2578125},
        },
        [7] = {
            {42.328765869141, -231.7585144043, 391.2578125},
            {40.524208068848, -237.22622680664, 391.2578125},
            {36.033706665039, -258.79446411133, 391.2578125},
            {34.055206298828, -324.92373657227, 391.2578125}
        },
        [8] = {
            {42.280960083008, -231.62298583984, 391.2578125},
            {41.030471801758, -234.11665344238, 391.2578125},
            {35.340335845947, -258.99816894531, 391.2578125},
            {34.514934539795, -282.27005004883, 391.2578125},
            {34.635982513428, -318.67440795898, 391.2578125},
            {27.819686889648, -327.60531616211, 391.2578125}
        },
        [9] = {
            {42.250061035156, -231.72912597656, 391.2578125},
            {40.653469085693, -234.11485290527, 391.2578125},
            {36.129238128662, -257.21347045898, 391.2578125},
            {34.363651275635, -328.65209960938, 391.2578125}
        },
        [10] = {
            {42.263786315918, -231.59927368164, 391.2578125},
            {40.717967987061, -234.04945373535, 391.2578125},
            {31.933692932129, -258.23052978516, 391.2578125},
            {27.318283081055, -272.70877075195, 391.2578125},
            {28.006837844849, -321.85070800781, 391.2578125}
        },
        [11] = {
            {42.216163635254, -231.63874816895, 391.2578125},
            {36.399757385254, -231.69770812988, 391.2578125},
            {35.184143066406, -250.30270385742, 391.2578125},
            {26.648471832275, -260.75616455078, 391.2578125},
            {23.364036560059, -264.87582397461, 391.2578125}
        },
        [12] = {
            {43.843509674072, -231.72087097168, 391.2578125},
            {41.129234313965, -234.11849975586, 391.2578125},
            {38.104625701904, -246.30049133301, 391.2578125},
            {26.256246566772, -260.11120605469, 391.2578125},
            {23.375122070312, -264.63571166992, 391.2578125}
        },
        [13] = {
            {42.445182800293, -231.82067871094, 391.2578125},
            {40.944149017334, -234.89335632324, 391.2578125},
            {32.536632537842, -250.67988586426, 391.4140625},
            {25.287998199463, -261.40002441406, 391.2578125},
            {23.152635574341, -264.82150268555, 391.2578125}
        },
        [14] = {
            {43.389442443848, -231.83204650879, 391.2578125},
            {36.642719268799, -231.88194274902, 391.2578125},
            {34.471031188965, -249.82000732422, 391.2578125},
            {26.251495361328, -261.09841918945, 391.2578125},
            {23.193752288818, -264.88043212891, 391.2578125},
            {9.7319440841675, -281.11434936523, 391.2578125}
        },
        [15] = {
            {42.817531585693, -231.65762329102, 391.2578125},
            {38.228343963623, -231.72372436523, 391.2578125},
            {33.086761474609, -269.26254272461, 391.2578125}
        },
        [16] = {
            {44.088314056396, -231.8408203125, 391.2578125},
            {37.92520904541, -231.90116882324, 391.2578125},
            {34.205741882324, -248.77616882324, 391.2578125},
            {27.812089920044, -271.38702392578, 391.2578125}
        },
        [17] = {
            {42.513153076172, -231.47653198242, 391.2578125},
            {41.24295425415, -234.88374328613, 391.2578125},
            {33.296817779541, -260.94522094727, 391.2578125}
        },
        [18] = {
            {43.468235015869, -231.27206420898, 391.2578125},
            {41.12255859375, -233.6473236084, 391.2578125},
            {36.899501800537, -258.04669189453, 391.2578125}
        }
    },
    [3] = {
        [1] = {
            {41.954219818115, -223.75901794434, 391.2578125},
            {22.771249771118, -225.69116210938, 391.2578125}
        },
        [2] = {
            {41.278377532959, -223.70478820801, 391.2578125}
        },
        [3] = {
            {41.938934326172, -223.58139038086, 391.2578125}
        },
        [4] = {
            {41.402084350586, -223.66876220703, 391.2578125},
            {12.422024726868, -221.7232208252, 391.2578125},
            {8.7621011734009, -223.45170593262, 391.2578125}
        },
        [5] = {
            {41.664489746094, -223.4404296875, 391.2578125},
            {38.094013214111, -226.15846252441, 391.2578125},
            {35.826259613037, -235.60914611816, 391.2578125},
            {34.082237243652, -250.86628723145, 391.2578125},
            {27.775608062744, -271.99853515625, 391.2578125}
        },
        [6] = {
            {41.92017364502, -223.36003112793, 391.2578125},
            {37.92537689209, -227.80738830566, 391.2578125}
        },
        [7] = {
            {42.901790618896, -223.43008422852, 391.2578125},
            {41.065128326416, -227.08258056641, 391.2578125},
            {37.769821166992, -248.24140930176, 391.2578125},
            {34.947376251221, -273.22952270508, 391.2578125},
            {34.22184753418, -324.26950073242, 391.2578125}
        },
        [8] = {
            {42.271350860596, -223.71926879883, 391.2578125},
            {26.476119995117, -224.59300231934, 391.2578125},
            {24.005084991455, -230.09564208984, 391.2578125},
            {24.46862411499, -254.13265991211, 391.2578125},
            {27.135175704956, -265.69561767578, 391.2578125},
            {27.684495925903, -324.28720092773, 391.2578125}
        },
        [9] = {
            {42.436191558838, -223.2779083252, 391.2578125},
            {40.974853515625, -225.5562286377, 391.2578125},
            {38.526191711426, -245.43316650391, 391.2578125},
            {35.526165008545, -257.83715820312, 391.2578125},
            {34.207038879395, -328.44400024414, 391.2578125}
        },
        [10] = {
            {42.514232635498, -223.43569946289, 391.2578125},
            {37.343647003174, -227.20785522461, 391.2578125},
            {35.420032501221, -248.67037963867, 391.2578125},
            {27.511444091797, -269.17901611328, 391.2578125},
            {28.282178878784, -319.06665039062, 391.2578125}
        },
        [11] = {
            {42.355453491211, -223.46617126465, 391.2578125},
            {25.386976242065, -226.25772094727, 391.2578125},
            {17.903150558472, -235.28256225586, 391.2578125},
            {17.088319778442, -276.46520996094, 391.2578125}
        },
        [12] = {
            {41.594024658203, -223.58308410645, 391.2578125},
            {36.796901702881, -226.05718994141, 391.2578125},
            {35.887966156006, -233.39736938477, 391.2578125},
            {34.923458099365, -249.84429931641, 391.2578125},
            {26.659715652466, -260.46588134766, 391.2578125},
            {23.474782943726, -264.50988769531, 391.2578125}
        },
        [13] = {
            {42.020225524902, -223.48664855957, 391.2578125},
            {38.008144378662, -226.59657287598, 391.2578125},
            {36.190124511719, -233.58807373047, 391.2578125},
            {34.984180450439, -248.96737670898, 391.2578125},
            {26.744253158569, -259.62692260742, 391.2578125},
            {23.353136062622, -264.48815917969, 391.2578125}
        },
        [14] = {
            {41.412712097168, -223.59365844727, 391.2578125},
            {24.936550140381, -227.04914855957, 391.2578125},
            {23.71587562561, -245.01383972168, 391.2578125}
        },
        [15] = {
            {42.236026763916, -223.74291992188, 391.2578125},
            {40.771781921387, -226.9903717041, 391.2578125},
            {40.294479370117, -243.20880126953, 391.2578125}
        },
        [16] = {
            {42.648948669434, -223.35604858398, 391.2578125},
            {41.015327453613, -226.9312286377, 391.2578125},
            {40.31213760376, -243.1071472168, 391.2578125},
            {28.922220230103, -269.03897094727, 391.2578125}
        },
        [17] = {
            {42.831787109375, -223.6522064209, 391.2578125},
            {40.93741607666, -226.76470947266, 391.2578125},
            {40.285598754883, -242.27253723145, 391.2578125}
        },
        [18] = {
            {41.725025177002, -223.6974029541, 391.2578125},
            {37.416324615479, -226.29895019531, 391.2578125},
            {36.04764175415, -233.37252807617, 391.2578125}
        }
    }
}

local Utils = {
    GetJobMarker = function()
        for _, marker in pairs(getElementsByType("marker", root, true)) do
            local color = {getMarkerColor(marker)}
            if color[1] == 0 and color[2] == 155 and color[3] == 0 and color[4] == 170 then
                return marker
            end
        end
    end,

    GetPathFromMarker = function(marker) 
        if isElement(marker) then
            local position = {getElementPosition(marker)}
            for i, job_pos in pairs(positions) do
                if math.ceil(job_pos[1]) == math.ceil(position[1]) and math.ceil(job_pos[2]) == math.ceil(position[2]) and math.ceil(job_pos[3]) == math.ceil(position[3]) then
                    return i
                end
            end
        end
        return 0
    end,

    GetNearestIndex = function(positions)
        if type(positions) ~= "table" then
            return 0 end

        local last_distance = 9999
        local result = 0
        for index, position in pairs(positions) do
            local localpos = Vector3(getElementPosition(localPlayer))
            local distance = 9999
            if position.x then
                distance = getDistanceBetweenPoints3D(localpos, Vector3(position.x, position.y, position.z))
            else
                distance = getDistanceBetweenPoints3D(localpos, Vector3(position[1], position[2], position[3]))
            end
            if distance < last_distance then
                last_distance = distance
                result = index
            end
        end
        return result
    end,

    GetAngle = function(x1, y1, x2, y2)
        local t = -math.deg(math.atan2(x2 - x1, y2 - y1))
        return t < 0 and t + 360 or t
    end,

    IsStorage = function(position)
        if type(position) ~= "table" then
            return false end
        
        for id, strpos in pairs(storagePoints) do
            if math.ceil(strpos[1]) == math.ceil(position[1]) and math.ceil(strpos[2]) == math.ceil(position[2]) and math.ceil(390.200) == math.ceil(position[3]) then
                return id
            end
        end

        return false
    end,

    LerpAngle = function(from, to, speed)
        local difference = to - from
        if difference > 180 then
            difference = difference - 360
        elseif difference < -180 then
            difference = difference + 360
        end
        return from + difference * speed
    end,

    NormalizeAngle = function(angle)
        return (angle % 360 + 360) % 360
    end,
}

-- Нативная сирена DarkFlame.
local function frontEndSiren()
    playAlert("admin_message", 5000)
end

local activeChatText
local activeChatTick = 0

local function isAdminPresenceMessage(text)
    local lowerText = utf8.lower(text)
    return utf8.find(lowerText, "зашёл")
        or utf8.find(lowerText, "зашел")
        or utf8.find(lowerText, "подключился")
        or utf8.find(lowerText, "вышел")
        or utf8.find(lowerText, "покинул сервер")
end

-- Сообщение администратора: стоп бота и сирена без автоответа.
local function onChatMessage(text, r, g, b, messageType)
    if not _STATE then return end
    activeChatText = text
    activeChatTick = getTickCount()
    if messageType == 0 then
        if utf8.find(text, "Администратор") then -- Проверка если сообщение содержит слово Администратор
            if r == 255 and g == 164 and b == 104 then -- Проверка на цвет сообщения админа, чтобы обработчик не реагировал на игроков
                if isAdminPresenceMessage(text) then return end
                if Settings.AutoDisable then
                    cancelAutoResume()
                    if _STATE then
                        _STATE = false
                        changeBotState(false)
                    end
                end
                frontEndSiren()
            end
        end
    end
end
addEventHandler("onClientChatMessage", root, onChatMessage)

local HELPING_MARKERS = {}

local function destroyHelpingMarker(marker)
    if HELPING_MARKERS[marker] then
        if isElement(marker) then destroyElement(marker) end
        HELPING_MARKERS[marker] = nil
    end
end

local function destroyHelpingMarkers()
    for marker in pairs(HELPING_MARKERS) do
        if isElement(marker) then destroyElement(marker) end
    end
    HELPING_MARKERS = {}
end

-- Creating
local CreateHelpingPoints = function(path_id, invers)
    destroyHelpingMarkers()
    STORAGE.MARKERS = {}
    local storagePaths = new_traectories[STORAGE.STORAGE_ID]
    local path = storagePaths and storagePaths[path_id]
    local jobMarker = Utils.GetJobMarker()
    if type(path) ~= "table" or #path == 0 or not isElement(jobMarker) then
        STORAGE.TARGET = {element = nil, index = 0}
        return false
    end
    local start_index = Utils.GetNearestIndex(path)
    local points_list = {}
    
    if invers then
        for i = 1, start_index do -- Filling helping points
            points_list[i] = path[start_index - i + 1]
        end 
        for _, position in ipairs(points_list) do
            local marker = createMarker(position[1], position[2], position[3], "cylinder", 1, 0, 0, 0, 0)
            setElementInterior(marker, getElementInterior(localPlayer))
            setElementDimension(marker, getElementDimension(localPlayer))

            HELPING_MARKERS[marker] = true
            table.insert(STORAGE.MARKERS, marker)
            if #STORAGE.MARKERS == 1 then
                STORAGE.TARGET.element = marker
                STORAGE.TARGET.index = 1
            end
        end
    else
        for i = start_index, #path do
            points_list[i - start_index + 1] = path[i]
        end
        for _, position in ipairs(points_list) do
            local marker = createMarker(position[1], position[2], position[3], "cylinder", 1, 0, 0, 0, 0)
            setElementInterior(marker, getElementInterior(localPlayer))
            setElementDimension(marker, getElementDimension(localPlayer))

            HELPING_MARKERS[marker] = true
            table.insert(STORAGE.MARKERS, marker)
            if #STORAGE.MARKERS == 1 then
                STORAGE.TARGET.element = marker
                STORAGE.TARGET.index = 1
            end
        end
    end
    table.insert(STORAGE.MARKERS, jobMarker)
    STORAGE.FINISH.element = jobMarker
    STORAGE.FINISH.type = Utils.IsStorage({getElementPosition(STORAGE.FINISH.element)}) and "storage" or "point"
    return true
end

-- ============================================================
-- NOJUMP ЗОНЫ — объявляем здесь чтобы были доступны в BunnyHop
-- ============================================================

local NOJUMP_ZONES = {}

local function isInNoJumpZone()
    local px, py, pz = getElementPosition(localPlayer)

    -- Проверяем ручные NOJUMP_ZONES
    for _, zone in ipairs(NOJUMP_ZONES) do
        local d = getDistanceBetweenPoints3D(px, py, pz, zone.x, zone.y, zone.z)
        if d <= zone.radius then
            return true
        end
    end

    -- Проверяем точки типа nojump из ACTION_POINTS
    if ACTION_POINTS then
        for _, pt in ipairs(ACTION_POINTS) do
            if pt.type == "nojump" then
                local d = getDistanceBetweenPoints3D(px, py, pz, pt.x, pt.y, pt.z)
                if d <= (pt.radius or 3.0) then
                    return true
                end
            end
        end
    end

    return false
end

-- Команда для проверки — показывает находится ли бот в nojump зоне

-- On frame update
local bunnyHopTimer = nil
local isReset, cameraAngle = true, 0

-- Принудительно отменяем прыжок если в NoJump зоне
addEventHandler("onClientPreRender", root, function()
    if not _STATE then return end
    if isInNoJumpZone() then
        setBotControl("jump", false)
    end
end)
local onUpdate = function()
    if not _STATE then return end
    if isElement(STORAGE.TARGET.element) then
        if isReset then
            cameraAngle = -(getPedCameraRotation(localPlayer))
        end

        local position = {getElementPosition(STORAGE.TARGET.element)}
        local localpos = {getElementPosition(localPlayer)}
        local angle = Utils.GetAngle(localpos[1], localpos[2], position[1], position[2])
       
        cameraAngle = Utils.NormalizeAngle(Utils.LerpAngle(cameraAngle, angle, Settings.RotationSpeed / 100))
        setPedCameraRotation(localPlayer, cameraAngle)

        setBotControl("forwards", true)
        setBotControl("sprint", STORAGE.FINISH.type == "point")
        
        if STORAGE.FINISH.type == "point" then
            if not bunnyHopTimer and Settings.AllowBunnyHop then
                bunnyHopTimer = setTimer(function()
                    local position = Vector3(getElementPosition(STORAGE.TARGET.element))
                    local localpos = Vector3(getElementPosition(localPlayer))
                    if getDistanceBetweenPoints3D(localpos, position) > Settings.BunnyHopMinDistance then
                        -- Не прыгаем во время бокового шага и в NoJump зонах
                        if not (WALK and WALK.isMoving) and not isInNoJumpZone() then
                            setBotControl("jump", true)
                            setTimer(setBotControl, 100, 1, "jump", false)
                        end
                    end
                end, Settings.BunnyHopDelay, 0)
            end
        else
            if bunnyHopTimer then
                killTimer(bunnyHopTimer); bunnyHopTimer = nil
            end
        end
        isReset = false
    else
        if not isReset then
            setBotControl("forwards", false)
            setBotControl("sprint", false)
            
            if bunnyHopTimer then
                killTimer(bunnyHopTimer); bunnyHopTimer = nil
            end
            setBotControl("jump", false)
            isReset = true
        end
    end

    if not isElement(STORAGE.FINISH.element) then
        STORAGE.FINISH.element = Utils.GetJobMarker()
        if isElement(STORAGE.FINISH.element) then
            STORAGE.FINISH.type = Utils.IsStorage({getElementPosition(STORAGE.FINISH.element)}) and "storage" or "point"
            if STORAGE.FINISH.type == "point" then
                STORAGE.PATH_ID = Utils.GetPathFromMarker(Utils.GetJobMarker())
                CreateHelpingPoints(Utils.GetPathFromMarker(STORAGE.FINISH.element))
            elseif STORAGE.FINISH.type == "storage" then
                STORAGE.STORAGE_ID = Utils.IsStorage({getElementPosition(STORAGE.FINISH.element)})
                CreateHelpingPoints(STORAGE.PATH_ID, true)
            end
        end
    end
end
addEventHandler("onClientPreRender", root, onUpdate)

-- ============================================================
-- ЗАХВАТ МАРКЕРА ПО РАССТОЯНИЮ + ДЕТЕКТ КРУЧЕНИЯ
-- ============================================================

local CAPTURE_RADIUS = 1.8   -- метров — захват маркера без onClientMarkerHit
local ROUTE_GUARD = {
    target = nil,
    bestDistance = math.huge,
    lastProgress = 0,
    forceDirectUntil = 0,
    MIN_PROGRESS = 0.75,
    NO_PROGRESS_MS = 7000,
}

local function resetRouteProgress()
    ROUTE_GUARD.target = nil
    ROUTE_GUARD.bestDistance = math.huge
    ROUTE_GUARD.lastProgress = 0
end

local function clearRouteGuard()
    resetRouteProgress()
    ROUTE_GUARD.forceDirectUntil = 0
end

-- Общая функция перехода к следующей точке
local function advanceToNextMarker()
    if not _STATE then return end
    if STORAGE.TARGET.index + 1 <= #STORAGE.MARKERS then
        STORAGE.TARGET.element = STORAGE.MARKERS[STORAGE.TARGET.index + 1]
        STORAGE.TARGET.index   = STORAGE.TARGET.index + 1
        resetRouteProgress()
    else
        -- Финальный маркер — имитируем onClientMarkerHit
        if STORAGE.TARGET.element and isElement(STORAGE.TARGET.element) then
            api.triggerEvent("onClientMarkerHit", STORAGE.TARGET.element, localPlayer, true)
        end
    end
end

-- 

addEventHandler("onClientMarkerHit", root, function(player)
    if player == localPlayer then
        for _, marker in pairs(STORAGE.MARKERS) do
            if source == marker then
                if STORAGE.TARGET.index + 1 <= #STORAGE.MARKERS then
                    STORAGE.TARGET.element = STORAGE.MARKERS[STORAGE.TARGET.index + 1]
                    STORAGE.TARGET.index = STORAGE.TARGET.index + 1
                else
                    STORAGE.TARGET.element = nil
                    STORAGE.TARGET.index = 0
                end
                destroyHelpingMarker(marker)
                return
            end
        end
    end
end)

changeBotState = function(state)
    clearRouteGuard()
    if state then
        local jobMarker = Utils.GetJobMarker()
        if not isElement(jobMarker) then
            _STATE = false
            outputChatBox("#FFAA00[JBK] #FFFFFFМаркер работы не найден — бот не запущен", 255,255,255,true)
            syncOptionsBotState()
            syncNativeJbkState()
            return false
        end
        cameraAngle = -(getPedCameraRotation(localPlayer))
        STORAGE.FINISH.element = jobMarker
        STORAGE.FINISH.type = Utils.IsStorage({getElementPosition(STORAGE.FINISH.element)}) and "storage" or "point"
        if STORAGE.FINISH.type == "point" then
            STORAGE.PATH_ID = Utils.GetPathFromMarker(jobMarker)
            CreateHelpingPoints(STORAGE.PATH_ID)
        else
            outputChatBox("#0037FF[JBK] #FFFFFFЗапустите меня когда будете идти за коробкой!", 255, 255, 255, true)
        end
        -- Запускаем живую походку если включена
        if WALK and WALK.enabled then startWalk() end
    else
        destroyHelpingMarkers()
        STORAGE.MARKERS = {}
        STORAGE.PATH_ID = 0
        STORAGE.FINISH = {element = nil, type = "storage"}
        STORAGE.TARGET = {element = nil, index = 0}
        
        releaseBotControls()
        api.mouse("right", false)
        
        if bunnyHopTimer then
            killTimer(bunnyHopTimer); bunnyHopTimer = nil
        end
        setCameraTarget(localPlayer)
        -- Останавливаем живую походку
        if WALK then stopWalk() end
    end
    syncOptionsBotState()
    syncNativeJbkState()
    return true
end

local function toggleBot()
    cancelAutoResume()
    _STATE = not _STATE
    changeBotState(_STATE)
    outputChatBox("#0037FF[JBK] #FFFFFFЖБК Бот "..(_STATE and "#00FF00включен." or "#F00000выключен."), 255, 255, 255, true)
end

setTimer(function()
    if not _STATE or not Settings.AntiAFK then
        api.mouse("right", false)
        return
    end
    api.mouse("right", true)
    setTimer(function() api.mouse("right", false) end, 75, 1)
end, 2000, 0)

-- ============================================================
-- JBK DEBUG v0.1 — редактор промежуточных точек
-- /jbk0.1 — включить/выключить режим отладки
-- P — добавить текущую позицию как промежуточную точку
-- O — удалить последнюю точку
-- I — вывести собранный путь в чат (для копирования)
-- L — показать/скрыть все точки назначения
-- ============================================================

local DEBUG = {
    active = false,
    -- Текущий редактируемый путь
    editStorage = 1,   -- склад 1/2/3
    editTarget = 1,    -- точка назначения 1-18
    editInverse = false, -- false=от склада к точке, true=обратно
    -- Собранные промежуточные точки
    points = {},
    -- Маркеры на карте
    markers = {},
    -- Маркеры всех точек назначения
    destMarkers = {},
    destVisible = false,
}

-- Цвета маркеров
local COLOR_WAYPOINT  = {0, 150, 255, 200}   -- синий — промежуточные точки
local COLOR_DEST      = {255, 100, 0,  200}   -- оранжевый — точки назначения
local COLOR_STORAGE   = {0,   255, 100, 200}  -- зелёный — склады
local COLOR_CURRENT   = {255, 255, 0,  200}   -- жёлтый — текущая позиция

local function dbgMsg(text)
    outputChatBox("#00AAFF[JBK DEBUG] #FFFFFF" .. text, 255, 255, 255, true)
end

-- Создаём маркер в мире
local function makeMarker(x, y, z, color, size)
    size = size or 0.8
    local m = createMarker(x, y, z - 1, "cylinder", size, 0, 0, 0, 0)
    setElementInterior(m, getElementInterior(localPlayer))
    setElementDimension(m, getElementDimension(localPlayer))
    return m
end

-- Удаляем все дебаг-маркеры
local function clearDebugMarkers()
    for _, m in ipairs(DEBUG.markers) do
        if isElement(m) then destroyElement(m) end
    end
    DEBUG.markers = {}
end

-- Перерисовываем маркеры текущего пути
local function redrawPath()
    clearDebugMarkers()
    for i, pt in ipairs(DEBUG.points) do
        local m = makeMarker(pt[1], pt[2], pt[3], COLOR_WAYPOINT, 0.6)
        table.insert(DEBUG.markers, m)
    end
    dbgMsg("Путь: " .. #DEBUG.points .. " точек | Склад=" .. DEBUG.editStorage ..
           " Цель=" .. DEBUG.editTarget .. " Обратно=" .. tostring(DEBUG.editInverse))
end

-- Показываем/скрываем все точки назначения
local function toggleDestMarkers()
    if DEBUG.destVisible then
        for _, m in ipairs(DEBUG.destMarkers) do
            if isElement(m) then destroyElement(m) end
        end
        DEBUG.destMarkers = {}
        DEBUG.destVisible = false
        dbgMsg("Точки назначения скрыты")
    else
        -- Точки назначения (positions)
        for i, pos in ipairs(positions) do
            local m = makeMarker(pos[1], pos[2], pos[3], COLOR_DEST, 1.0)
            table.insert(DEBUG.destMarkers, m)
        end
        -- Склады (storagePoints)
        for i, pos in ipairs(storagePoints) do
            local m = makeMarker(pos[1], pos[2], pos[3], COLOR_STORAGE, 1.2)
            table.insert(DEBUG.destMarkers, m)
        end
        DEBUG.destVisible = true
        dbgMsg("Показано " .. #positions .. " точек назначения + " .. #storagePoints .. " складов")
    end
end

-- Добавляем текущую позицию как промежуточную точку
local function addPoint()
    if not DEBUG.active then return end
    local x, y, z = getElementPosition(localPlayer)
    -- Округляем до 3 знаков
    x = math.floor(x * 1000 + 0.5) / 1000
    y = math.floor(y * 1000 + 0.5) / 1000
    z = math.floor(z * 1000 + 0.5) / 1000
    table.insert(DEBUG.points, {x, y, z})
    redrawPath()
    dbgMsg("Добавлена точка #" .. #DEBUG.points .. ": " ..
           string.format("%.3f, %.3f, %.3f", x, y, z))
end

-- Удаляем последнюю точку
local function removeLastPoint()
    if not DEBUG.active then return end
    if #DEBUG.points == 0 then
        dbgMsg("Нет точек для удаления")
        return
    end
    table.remove(DEBUG.points)
    redrawPath()
    dbgMsg("Удалена последняя точка. Осталось: " .. #DEBUG.points)
end

-- Выводим путь в чат для копирования в скрипт
local function printPath()
    if not DEBUG.active then return end
    if #DEBUG.points == 0 then
        dbgMsg("Нет точек!")
        return
    end
    dbgMsg("=== ПУТЬ [" .. DEBUG.editStorage .. "][" .. DEBUG.editTarget .. "] ===")
    local lines = {}
    for i, pt in ipairs(DEBUG.points) do
        local line = string.format("            {%.3f, %.3f, %.3f},", pt[1], pt[2], pt[3])
        outputChatBox(line, 255, 255, 255, false)
        table.insert(lines, line)
    end
    dbgMsg("Скопируй строки выше в new_traectories[" ..
           DEBUG.editStorage .. "][" .. DEBUG.editTarget .. "]")
end

-- Переключаем редактируемый путь
local function setEditPath(storage, target, inverse)
    DEBUG.editStorage = storage or DEBUG.editStorage
    DEBUG.editTarget  = target  or DEBUG.editTarget
    DEBUG.editInverse = inverse or false
    -- Загружаем существующие точки из скрипта
    DEBUG.points = {}
    local existing = new_traectories[DEBUG.editStorage] and
                     new_traectories[DEBUG.editStorage][DEBUG.editTarget]
    if existing then
        for _, pt in ipairs(existing) do
            table.insert(DEBUG.points, {pt[1], pt[2], pt[3]})
        end
        dbgMsg("Загружено " .. #DEBUG.points .. " существующих точек")
    end
    redrawPath()
end

-- Включаем/выключаем дебаг режим
local function toggleDebug()
    DEBUG.active = not DEBUG.active
    if DEBUG.active then
        dbgMsg("=== РЕЖИМ ОТЛАДКИ ВКЛЮЧЁН ===")
        dbgMsg("P=добавить точку  O=удалить  I=вывести путь  L=показать цели")
        dbgMsg("Текущий путь: Склад=" .. DEBUG.editStorage .. " Цель=" .. DEBUG.editTarget)
        dbgMsg("Смена пути: /jbkpath <склад 1-3> <цель 1-18>")
        -- Показываем текущую позицию
        local x, y, z = getElementPosition(localPlayer)
        dbgMsg(string.format("Твоя позиция: %.3f, %.3f, %.3f", x, y, z))
        setEditPath()
        toggleDestMarkers()
    else
        dbgMsg("Режим отладки выключен")
        clearDebugMarkers()
        if DEBUG.destVisible then toggleDestMarkers() end
    end
end

-- Команды



-- Клавиши (только в режиме отладки)




-- ============================================================
-- JBK Debug v0.2 — диагностика маркеров и вывод точек
-- /jbkmarkers — показать все маркеры рядом с игроком
-- /jbkdump — вывести текущий путь бота в чат
-- ============================================================

-- Вывод всех маркеров в радиусе 200 единиц

-- Вывод текущего состояния бота и пути

-- Автологирование — каждые 3 сек выводит позицию и цель если бот активен
local autoLogEnabled = false
local autoLogTimer = nil



-- ============================================================
-- JBK Debug v0.2 — визуализация маршрутов
-- /jbk0.2 — включить/выключить визуализацию
-- /jbksim — симуляция прохода маршрута (без движения)
-- /jbktest <склад> <цель> — тест конкретного пути
-- ============================================================

local VIS = {
    active = false,
    showAll = false,      -- показывать все маршруты или только текущий
    showNumbers = true,   -- нумерация точек
    showRadius = true,    -- радиус захвата маркера
    simActive = false,    -- симуляция активна
    simIndex = 1,         -- текущая точка симуляции
    simTimer = nil,
    simPath = nil,        -- путь для симуляции
}

-- Симуляция прохода маршрута
local function startSim(storage, target)
    storage = storage or DEBUG.editStorage
    target  = target  or DEBUG.editTarget
    local path = new_traectories[storage] and new_traectories[storage][target]
    if not path or #path == 0 then
        outputChatBox("[SIM] Путь не найден: склад=" .. storage .. " цель=" .. target,
            255,100,100,false)
        return
    end
    -- Если есть точки из дебаггера — используем их
    if DEBUG.active and #DEBUG.points > 0 then
        path = DEBUG.points
        outputChatBox("[SIM] Используем точки из редактора (" .. #path .. " шт)", 0,200,255,false)
    end
    VIS.simPath   = path
    VIS.simIndex  = 1
    VIS.simActive = true
    if VIS.simTimer then killTimer(VIS.simTimer) end
    -- Двигаем "призрак" каждые 800мс
    VIS.simTimer = setTimer(function()
        if not VIS.simActive then return end
        VIS.simIndex = VIS.simIndex + 1
        if VIS.simIndex > #VIS.simPath then
            VIS.simActive = false
            VIS.simIndex  = 1
            killTimer(VIS.simTimer)
            VIS.simTimer = nil
            outputChatBox("[SIM] Симуляция завершена", 0,200,255,false)
        end
    end, 800, #path)
    outputChatBox("[SIM] Запуск симуляции: " .. #path .. " точек", 0,200,255,false)
end

-- Команды v0.2





-- ============================================================
-- JBK PATCH v0.3
-- Фикс кругов + строже маркеры + автодетект в дебаггере
-- Включение бота через /jbk0.1 теперь тоже здесь
-- ============================================================

-- ===== ПАТЧ: строже искать маркер работы =====
-- Оригинальный GetJobMarker ищет только 0,155,0,170
-- Расширяем: логируем все маркеры и ищем по нескольким цветам

local MARKER_LOG = false  -- включить лог всех маркеров при поиске

local _origGetJobMarker = Utils.GetJobMarker
Utils.GetJobMarker = function()
    local best = nil
    local bestDist = 9999
    local px, py, pz = getElementPosition(localPlayer)

    for _, marker in pairs(getElementsByType("marker", root, true)) do
        local r, g, b, a = getMarkerColor(marker)
        local mx, my, mz = getElementPosition(marker)
        local dist = getDistanceBetweenPoints3D(px, py, pz, mx, my, mz)

        if MARKER_LOG then
            outputChatBox(string.format("[MRK] %.1f,%.1f,%.1f rgb=%d,%d,%d,%d dist=%.1f",
                mx, my, mz, r, g, b, a, dist), 200, 200, 200, false)
        end

        -- Основной цвет работы: 0,155,0,170
        -- Также проверяем похожие зелёные маркеры (сервер может менять alpha)
        local isJobMarker = (r == 0 and g == 155 and b == 0 and a == 170)
            or (r == 0 and g == 155 and b == 0 and a >= 100)
            or (r == 0 and g >= 140 and g <= 170 and b == 0 and a >= 100)

        if isJobMarker and dist < bestDist then
            bestDist = dist
            best = marker
        end
    end

    if not best then
        -- Fallback: оригинальный поиск
        best = _origGetJobMarker()
    end

    return best
end

-- ===== ПАТЧ: фикс кругов в GetNearestIndex =====
-- Вместо просто ближайшей точки — берём первую точку ВПЕРЕДИ по пути
-- "Впереди" = точка которая ближе к финишу чем текущая позиция

local _origGetNearestIndex = Utils.GetNearestIndex
Utils.GetNearestIndex = function(pts)
    if type(pts) ~= "table" or #pts == 0 then return 0 end

    local px, py, pz = getElementPosition(localPlayer)

    -- Если только 1 точка — возвращаем её
    if #pts == 1 then return 1 end

    -- Финиш — последняя точка пути
    local finX, finY, finZ = pts[#pts][1], pts[#pts][2], pts[#pts][3]
    local distToFin = getDistanceBetweenPoints3D(px, py, pz, finX, finY, finZ)

    -- Ищем первую точку которая:
    -- 1. Ближе к финишу чем мы (т.е. впереди нас)
    -- 2. Из всех таких — ближайшую к нам
    local bestIdx = 1
    local bestDist = 9999

    for i, pt in ipairs(pts) do
        local ptDistToFin = getDistanceBetweenPoints3D(pt[1], pt[2], pt[3], finX, finY, finZ)
        local ptDistToUs  = getDistanceBetweenPoints3D(px, py, pz, pt[1], pt[2], pt[3])

        -- Точка "впереди" если она ближе к финишу чем мы
        -- Добавляем небольшой допуск 3 единицы чтобы не пропускать точки рядом
        if ptDistToFin <= distToFin + 3 then
            if ptDistToUs < bestDist then
                bestDist = ptDistToUs
                bestIdx = i
            end
        end
    end

    return bestIdx
end

-- ===== ПАТЧ: детектор застревания =====
local stuckPos = nil
local stuckTime = 0
local stuckCount = 0
local stuckCheckTick = 0
local STUCK_DIST = 1.5   -- метров за период
local STUCK_TIME = 4000  -- мс между проверками
local STUCK_MAX  = 2     -- сколько раз подряд застрял = пересчёт

setTimer(function()
    local now = getTickCount()
    if now - stuckCheckTick < STUCK_TIME then return end
    stuckCheckTick = now
    if not _STATE then
        stuckPos = nil; stuckCount = 0; return
    end
    local px, py, pz = getElementPosition(localPlayer)
    if stuckPos then
        local d = getDistanceBetweenPoints3D(px, py, pz, stuckPos[1], stuckPos[2], stuckPos[3])
        if d < STUCK_DIST then
            stuckCount = stuckCount + 1
            if stuckCount >= STUCK_MAX then
                stuckCount = 0
                -- Пересчитываем путь
                if STORAGE.FINISH.element and isElement(STORAGE.FINISH.element) then
                    outputChatBox("[JBK] Застрял — пересчёт пути", 255, 150, 0, false)
                    playAlert("stuck", 60000)
                    if STORAGE.FINISH.type == "point" then
                        CreateHelpingPoints(STORAGE.PATH_ID)
                    else
                        CreateHelpingPoints(STORAGE.PATH_ID, true)
                    end
                end
            end
        else
            stuckCount = 0
        end
    end
    stuckPos = {px, py, pz}
end, 250, 0)

-- ===== ДЕБАГГЕР v0.2: автодетект маршрута =====
-- При включении /jbk0.1 — автоматически определяет склад и цель

local _origToggleDebug = toggleDebug
toggleDebug = function()
    _origToggleDebug()
    if DEBUG.active then
        -- Автодетект: смотрим на маркер сервера
        local jobMarker = Utils.GetJobMarker()
        if jobMarker then
            local mx, my, mz = getElementPosition(jobMarker)
            -- Определяем тип: склад или точка назначения
            local storageId = Utils.IsStorage({mx, my, mz})
            if storageId then
                -- Маркер на складе — значит идём за ящиком
                -- Склад определён, цель = текущий PATH_ID или 1
                DEBUG.editStorage = storageId
                DEBUG.editTarget  = STORAGE.PATH_ID > 0 and STORAGE.PATH_ID or 1
                dbgMsg(string.format("Автодетект: СКЛАД %d (маркер на складе)", storageId))
            else
                -- Маркер на точке назначения — определяем номер
                local pathId = Utils.GetPathFromMarker(jobMarker)
                if pathId > 0 then
                    -- Определяем ближайший склад
                    local px, py, pz = getElementPosition(localPlayer)
                    local nearestStorage = 1
                    local nearestDist = 9999
                    for i, sp in ipairs(storagePoints) do
                        local d = getDistanceBetweenPoints3D(px, py, pz, sp[1], sp[2], sp[3])
                        if d < nearestDist then nearestDist = d; nearestStorage = i end
                    end
                    DEBUG.editStorage = nearestStorage
                    DEBUG.editTarget  = pathId
                    dbgMsg(string.format("Автодетект: Склад=%d Цель=%d (маркер на точке)",
                        nearestStorage, pathId))
                else
                    dbgMsg("Маркер найден но цель не определена — используй /jbkpath")
                end
            end
            setEditPath(DEBUG.editStorage, DEBUG.editTarget)
        else
            dbgMsg("Маркер работы не найден — используй /jbkpath <склад> <цель>")
            dbgMsg("Или включи лог маркеров: /jbkmlog")
        end
    end
end

-- Включить/выключить лог маркеров

-- Ручной поиск маркера прямо сейчас


-- ============================================================
-- JBK AUTO INFO — автовывод склада и номера маркера
-- Каждые 2 сек проверяет маркер и выводит если изменился
-- ============================================================

local lastMarkerInfo = nil  -- последний выведенный маркер

setTimer(function()
    local marker = Utils.GetJobMarker()
    if not marker or not isElement(marker) then
        if lastMarkerInfo ~= "none" then
            lastMarkerInfo = "none"
        end
        return
    end

    local mx, my, mz = getElementPosition(marker)
    local key = string.format("%.1f_%.1f", mx, my)

    -- Обновляем кэш всегда, но выводим только если флаг включён
    if key == lastMarkerInfo then return end
    lastMarkerInfo = key

    -- Выводим только если галочка включена
    if not SHOW_MARKER_CHAT then return end

    -- Определяем тип маркера
    local storageId = Utils.IsStorage({mx, my, mz})
    if storageId then
        outputChatBox(string.format(
            "#00FF88[JBK] #FFFFFFСклад #%d | Иди за ящиком (%.1f, %.1f)",
            storageId, mx, my), 255, 255, 255, true)
    else
        local pathId = Utils.GetPathFromMarker(marker)
        local px, py, pz = getElementPosition(localPlayer)
        local nearestStorage = 1
        local nearestDist = 9999
        for i, sp in ipairs(storagePoints) do
            local d = getDistanceBetweenPoints3D(px, py, pz, sp[1], sp[2], sp[3])
            if d < nearestDist then nearestDist = d; nearestStorage = i end
        end
        if pathId > 0 then
            outputChatBox(string.format(
                "#00AAFF[JBK] #FFFFFFТочка #%d | Склад #%d | Сдай ящик (%.1f, %.1f)",
                pathId, nearestStorage, mx, my), 255, 255, 255, true)
        else
            outputChatBox(string.format(
                "#FFAA00[JBK] #FFFFFFНеизвестная точка (%.1f, %.1f) | Склад #%d",
                mx, my, nearestStorage), 255, 255, 255, true)
        end
    end
end, 2000, 0)

-- ============================================================
-- JBK STRICT MODE — /jbkk
-- Мгновенный поворот к цели без интерполяции
-- Показывает линии к следующим маркерам
-- ============================================================

local STRICT = false  -- строгий режим вкл/выкл


-- ===== Перехватываем onUpdate для строгого режима =====
-- Удаляем старый обработчик и добавляем новый с проверкой STRICT

removeEventHandler("onClientPreRender", root, onUpdate)

local onUpdateStrict = function()
    if not _STATE then return end
    if isElement(STORAGE.TARGET.element) then
        if isReset then
            cameraAngle = -(getPedCameraRotation(localPlayer))
        end

        local position = {getElementPosition(STORAGE.TARGET.element)}
        local localpos = {getElementPosition(localPlayer)}
        local angle = Utils.GetAngle(localpos[1], localpos[2], position[1], position[2])

        if STRICT or getTickCount() < ROUTE_GUARD.forceDirectUntil then
            -- Мгновенный поворот — без интерполяции
            cameraAngle = angle
        else
            -- Оригинальный плавный поворот
            cameraAngle = Utils.NormalizeAngle(
                Utils.LerpAngle(cameraAngle, angle, Settings.RotationSpeed / 100))
        end

        setPedCameraRotation(localPlayer, cameraAngle)
        setBotControl("forwards", true)
        setBotControl("sprint", STORAGE.FINISH.type == "point")

        if STORAGE.FINISH.type == "point" then
            if not bunnyHopTimer and Settings.AllowBunnyHop then
                bunnyHopTimer = setTimer(function()
                    local position = Vector3(getElementPosition(STORAGE.TARGET.element))
                    local localpos = Vector3(getElementPosition(localPlayer))
                    if getDistanceBetweenPoints3D(localpos, position) > Settings.BunnyHopMinDistance then
                        -- Не прыгаем во время бокового шага и в NoJump зонах
                        if not (WALK and WALK.isMoving) and not isInNoJumpZone() then
                            setBotControl("jump", true)
                            setTimer(setBotControl, 100, 1, "jump", false)
                        end
                    end
                end, Settings.BunnyHopDelay, 0)
            end
        else
            if bunnyHopTimer then
                killTimer(bunnyHopTimer); bunnyHopTimer = nil
            end
        end
        isReset = false
    else
        if not isReset then
            setBotControl("forwards", false)
            setBotControl("sprint", false)
            if bunnyHopTimer then
                killTimer(bunnyHopTimer); bunnyHopTimer = nil
            end
            setBotControl("jump", false)
            isReset = true
        end
    end

    if not isElement(STORAGE.FINISH.element) then
        STORAGE.FINISH.element = Utils.GetJobMarker()
        if isElement(STORAGE.FINISH.element) then
            STORAGE.FINISH.type = Utils.IsStorage({getElementPosition(STORAGE.FINISH.element)})
                and "storage" or "point"
            if STORAGE.FINISH.type == "point" then
                STORAGE.PATH_ID = Utils.GetPathFromMarker(Utils.GetJobMarker())
                CreateHelpingPoints(Utils.GetPathFromMarker(STORAGE.FINISH.element))
            elseif STORAGE.FINISH.type == "storage" then
                STORAGE.STORAGE_ID = Utils.IsStorage({getElementPosition(STORAGE.FINISH.element)})
                CreateHelpingPoints(STORAGE.PATH_ID, true)
            end
        end
    end
end

addEventHandler("onClientPreRender", root, onUpdateStrict)

-- ============================================================
-- JBK OPTIONS — единое окно настроек /jbkoptions
-- Все настройки бота в одном месте
-- Включение бота: J или /jbkpz (без изменений)
-- ============================================================

local OPT = {
    window = nil,
    visible = false,
    statusLabel = nil,
    toggleButton = nil,
}

syncOptionsBotState = function()
    if OPT.statusLabel and isElement(OPT.statusLabel) then
        guiSetText(OPT.statusLabel, "Бот: " .. (_STATE and "ВКЛЮЧЁН" or "выключен"))
    end
    if OPT.toggleButton and isElement(OPT.toggleButton) then
        guiSetText(OPT.toggleButton, _STATE and "Выключить бота" or "Включить бота")
    end
end

setTimer(function()
    local target = STORAGE.TARGET.element
    if not _STATE or not isElement(target) then
        resetRouteProgress()
        return
    end

    local px, py, pz = getElementPosition(localPlayer)
    local tx, ty, tz = getElementPosition(target)
    local distance = getDistanceBetweenPoints3D(px, py, pz, tx, ty, tz)
    local now = getTickCount()

    if distance <= CAPTURE_RADIUS then
        api.triggerEvent("onClientMarkerHit", target, localPlayer, true)
        resetRouteProgress()
        return
    end

    if ROUTE_GUARD.target ~= target then
        ROUTE_GUARD.target = target
        ROUTE_GUARD.bestDistance = distance
        ROUTE_GUARD.lastProgress = now
        return
    end

    if distance <= ROUTE_GUARD.bestDistance - ROUTE_GUARD.MIN_PROGRESS then
        ROUTE_GUARD.bestDistance = distance
        ROUTE_GUARD.lastProgress = now
        return
    end
    if now - ROUTE_GUARD.lastProgress < ROUTE_GUARD.NO_PROGRESS_MS then return end

    ROUTE_GUARD.forceDirectUntil = now + 4000
    if target ~= STORAGE.FINISH.element and STORAGE.TARGET.index < #STORAGE.MARKERS then
        local skippedIndex = STORAGE.TARGET.index
        advanceToNextMarker()
        destroyHelpingMarker(target)
        outputChatBox("#FFAA00[JBK] #FFFFFFRoute guard пропустил зацикленную точку #" .. skippedIndex,
            255, 255, 255, true)
    elseif isElement(STORAGE.FINISH.element) then
        outputChatBox("#FFAA00[JBK] #FFFFFFRoute guard перестроил путь к финалу", 255, 255, 255, true)
        if STORAGE.FINISH.type == "point" then
            CreateHelpingPoints(STORAGE.PATH_ID)
        else
            CreateHelpingPoints(STORAGE.PATH_ID, true)
        end
    end
    resetRouteProgress()
end, 500, 0)

local function createOptionsWindow()
    if OPT.window and isElement(OPT.window) then
        guiSetVisible(OPT.window, true)
        showCursor(true)
        OPT.visible = true
        return
    end

    local W, H = 520, 480
    local sx, sy = guiGetScreenSize()
    local wx = (sx - W) / 2
    local wy = (sy - H) / 2

    OPT.window = guiCreateWindow(wx, wy, W, H, "JBK Bot — Настройки (F10)", false)
    guiSetVisible(OPT.window, false)

    -- ===== СЕКЦИЯ: СОСТОЯНИЕ БОТА =====
    guiCreateLabel(10, 25, 200, 16, "СОСТОЯНИЕ БОТА", false, OPT.window)

    -- Статус
    local lblStatus = guiCreateLabel(10, 44, 500, 18,
        "Бот: " .. (_STATE and "ВКЛЮЧЁН" or "выключен"), false, OPT.window)

    -- Кнопка вкл/выкл бота
    local btnToggle = guiCreateButton(10, 64, 150, 28,
        _STATE and "Выключить бота" or "Включить бота", false, OPT.window)

    -- ===== СЕКЦИЯ: РЕЖИМ ДВИЖЕНИЯ =====
    guiCreateLabel(10, 105, 200, 16, "РЕЖИМ ДВИЖЕНИЯ", false, OPT.window)

    local btnStrict = guiCreateButton(10, 124, 200, 28,
        STRICT and "Строгий режим: ВКЛ" or "Строгий режим: ВЫКЛ", false, OPT.window)
    guiCreateLabel(220, 130, 290, 16,
        "Мгновенный поворот к цели", false, OPT.window)

    -- Скорость поворота (только в обычном режиме)
    guiCreateLabel(10, 160, 150, 16, "Плавность поворота:", false, OPT.window)
    local sldRotation = guiCreateScrollBar(160, 160, 200, 16, true, false, OPT.window)
    guiScrollBarSetScrollPosition(sldRotation, Settings.RotationSpeed * 10)
    local lblRotVal = guiCreateLabel(370, 160, 60, 16,
        "x" .. Settings.RotationSpeed, false, OPT.window)

    -- BunnyHop
    local chkBhop = guiCreateCheckBox(10, 185, 200, 18,
        "BunnyHop", Settings.AllowBunnyHop, false, OPT.window)
    guiCreateLabel(220, 185, 290, 16,
        "Прыжки при беге к точке", false, OPT.window)

    -- ===== СЕКЦИЯ: ВИЗУАЛИЗАЦИЯ =====
    guiCreateLabel(10, 215, 200, 16, "ВИЗУАЛИЗАЦИЯ", false, OPT.window)

    local chkVis = guiCreateCheckBox(10, 234, 200, 18,
        "Показывать путь (jbk0.2)", VIS.active, false, OPT.window)
    local chkNumbers = guiCreateCheckBox(10, 255, 200, 18,
        "Нумерация точек", VIS.showNumbers, false, OPT.window)
    local chkRadius = guiCreateCheckBox(10, 276, 200, 18,
        "Радиус захвата маркера", VIS.showRadius, false, OPT.window)
    local chkDebug = guiCreateCheckBox(220, 234, 200, 18,
        "Дебаггер (jbk0.1)", DEBUG.active, false, OPT.window)
    local chkAutoLog = guiCreateCheckBox(220, 255, 200, 18,
        "Лог маркеров", MARKER_LOG, false, OPT.window)

    -- ===== СЕКЦИЯ: ДЕТЕКТОР ЗАСТРЕВАНИЯ =====
    guiCreateLabel(10, 308, 200, 16, "ДЕТЕКТОР ЗАСТРЕВАНИЯ", false, OPT.window)

    guiCreateLabel(10, 327, 120, 16, "Порог (метры):", false, OPT.window)
    local editStuck = guiCreateEdit(135, 325, 60, 20,
        tostring(STUCK_DIST), false, OPT.window)

    guiCreateLabel(210, 327, 120, 16, "Интервал (мс):", false, OPT.window)
    local editStuckTime = guiCreateEdit(335, 325, 70, 20,
        tostring(STUCK_TIME), false, OPT.window)

    guiCreateLabel(10, 350, 200, 16, "Попыток до пересчёта:", false, OPT.window)
    local editStuckMax = guiCreateEdit(215, 348, 50, 20,
        tostring(STUCK_MAX), false, OPT.window)

    -- ===== КНОПКИ ВНИЗУ =====
    local btnApply = guiCreateButton(10, 435, 120, 30, "Применить", false, OPT.window)
    local btnSim   = guiCreateButton(140, 435, 120, 30, "Симуляция пути", false, OPT.window)
    local btnFind  = guiCreateButton(270, 435, 120, 30, "Найти маркер", false, OPT.window)
    local btnClose = guiCreateButton(390, 435, 120, 30, "Закрыть", false, OPT.window)

    -- ===== ОБРАБОТЧИКИ =====

    -- Вкл/выкл бота
    addEventHandler("onClientGUIClick", btnToggle, function()
        toggleBot()
        guiSetText(btnToggle, _STATE and "Выключить бота" or "Включить бота")
        guiSetText(lblStatus, "Бот: " .. (_STATE and "ВКЛЮЧЁН" or "выключен"))
    end, false)

    -- Строгий режим
    addEventHandler("onClientGUIClick", btnStrict, function()
        STRICT = not STRICT
        guiSetText(btnStrict, STRICT and "Строгий режим: ВКЛ" or "Строгий режим: ВЫКЛ")
        outputChatBox("[JBK] Строгий режим: " .. (STRICT and "ВКЛ" or "ВЫКЛ"), 0,200,255,false)
    end, false)

    -- Плавность поворота
    addEventHandler("onClientGUIScroll", sldRotation, function()
        local val = math.floor(guiScrollBarGetScrollPosition(sldRotation) / 10)
        val = math.max(1, math.min(100, val))
        Settings.RotationSpeed = val
        guiSetText(lblRotVal, "x" .. val)
    end, false)

    -- BunnyHop
    addEventHandler("onClientGUIClick", chkBhop, function()
        Settings.AllowBunnyHop = guiCheckBoxGetSelected(chkBhop)
        outputChatBox("[JBK] BunnyHop: " .. (Settings.AllowBunnyHop and "ВКЛ" or "ВЫКЛ"), 0,200,255,false)
    end, false)

    -- Визуализация
    addEventHandler("onClientGUIClick", chkVis, function()
        VIS.active = guiCheckBoxGetSelected(chkVis)
        outputChatBox("[JBK] Визуализация: " .. (VIS.active and "ВКЛ" or "ВЫКЛ"), 0,200,255,false)
    end, false)

    addEventHandler("onClientGUIClick", chkNumbers, function()
        VIS.showNumbers = guiCheckBoxGetSelected(chkNumbers)
    end, false)

    addEventHandler("onClientGUIClick", chkRadius, function()
        VIS.showRadius = guiCheckBoxGetSelected(chkRadius)
    end, false)

    addEventHandler("onClientGUIClick", chkDebug, function()
        local want = guiCheckBoxGetSelected(chkDebug)
        if want ~= DEBUG.active then toggleDebug() end
    end, false)

    addEventHandler("onClientGUIClick", chkAutoLog, function()
        MARKER_LOG = guiCheckBoxGetSelected(chkAutoLog)
        outputChatBox("[JBK] Лог маркеров: " .. (MARKER_LOG and "ВКЛ" or "ВЫКЛ"), 0,200,255,false)
    end, false)

    -- Применить настройки застревания
    addEventHandler("onClientGUIClick", btnApply, function()
        local d = tonumber(guiGetText(editStuck))
        local t = tonumber(guiGetText(editStuckTime))
        local m = tonumber(guiGetText(editStuckMax))
        if d then STUCK_DIST = d end
        if t then STUCK_TIME = t end
        if m then STUCK_MAX  = m end
        outputChatBox(string.format("[JBK] Застревание: порог=%.1fm интервал=%dмс попыток=%d",
            STUCK_DIST, STUCK_TIME, STUCK_MAX), 0,200,255,false)
    end, false)

    -- Симуляция
    addEventHandler("onClientGUIClick", btnSim, function()
        startSim()
        outputChatBox("[JBK] Симуляция запущена", 0,200,255,false)
    end, false)

    -- Найти маркер
    addEventHandler("onClientGUIClick", btnFind, function()
        MARKER_LOG = true
        local m = Utils.GetJobMarker()
        MARKER_LOG = false
        if m then
            local mx, my, mz = getElementPosition(m)
            local r, g, b, a = getMarkerColor(m)
            outputChatBox(string.format("[JBK] Маркер: %.2f,%.2f,%.2f rgb=%d,%d,%d,%d",
                mx, my, mz, r, g, b, a), 0,255,100,false)
        else
            outputChatBox("[JBK] Маркер не найден", 255,100,100,false)
        end
    end, false)

    -- Закрыть
    addEventHandler("onClientGUIClick", btnClose, function()
        guiSetVisible(OPT.window, false)
        showCursor(false)
        OPT.visible = false
    end, false)

    guiSetVisible(OPT.window, true)
    showCursor(true)
    OPT.visible = true
end

-- Команда


-- ============================================================
-- JBK OPTIONS PATCH — дополнения к меню
-- + галочка вывода сообщений в чат
-- + частота bunny hop
-- + кнопка инструкция
-- ============================================================

-- Глобальный флаг вывода сообщений о маркерах в чат
local SHOW_MARKER_CHAT = false

-- Патчим автовывод маркера — добавляем проверку флага
local _origMarkerTimer = nil  -- уже запущен выше, патчим через флаг
-- (флаг SHOW_MARKER_CHAT проверяется внутри setTimer выше через переопределение)

-- Переопределяем вывод маркера с учётом флага
-- Находим и заменяем outputChatBox в блоке автовывода через обёртку
local _origOutputMarker = outputChatBox
local function markerChatBox(text, ...)
    -- Вызывается только если SHOW_MARKER_CHAT = true
    if SHOW_MARKER_CHAT then
        outputChatBox(text, ...)
    end
end

-- Патч setTimer для автовывода маркера — пересоздаём с проверкой флага
-- (предыдущий таймер уже работает, добавляем второй с флагом)
-- Отменяем старый вывод через флаг lastMarkerInfo = nil при смене флага

-- Инструкция
local INSTRUCTIONS = [[
=== JBK BOT — ИНСТРУКЦИЯ ===

ОСНОВНОЕ УПРАВЛЕНИЕ:
  F10 — открыть или закрыть окно настроек
  Кнопка «Включить бота» в окне — запуск
  Кнопка «Выключить бота» — остановка

ЗАПУСК БОТА:
  1. Встань у маркера работы (зелёный)
  2. Открой F10 и нажми «Включить бота»
  3. Бот идёт к ящику, берёт его, несёт на точку
  4. Цикл повторяется автоматически

НАСТРОЙКИ В ОКНЕ F10:
  BunnyHop включён по умолчанию
  Anti-AFK включён по умолчанию и нативно нажимает ПКМ каждые 2 сек
  Доступны BunnyHop, строгий режим и живая походка
  Можно настроить задержки, дистанции и safety-пороги
  Точки действий редактируются кнопкой в этом же окне

БЕЗОПАСНОСТЬ (авто-стоп):
  Высота > 395 — бот останавливается (телепорт вверх)
  Перемещение > 30м за 500мс — бот останавливается
  Через 3 мин — бот автоматически возобновляется
  Safety-stop, застревание и админ рядом включают сирену DarkFlame

ADMIN WATCHER (авто-детект админов):
  Каждые 2 сек сканирует игроков рядом
  Если админ рядом — сирена и остановка бота
  После стопа по админу бот остаётся выключенным до ручного запуска
  Вход и выход админа с сервера выводятся только в чат
  Новые ники автодетектируются из чата (администратором Имя_Фамилия)

УДАР ПО АДМИНУ:
  Если админ в радиусе 8м — бот подходит и бьёт
  Один удар раз в 30 секунд
  Настройка радиуса и кулдауна находится в окне F10
  Автоматически включается при стопе по высоте

КОМАНД В ЧАТЕ НЕТ — ВСЁ УПРАВЛЕНИЕ ЧЕРЕЗ F10.
]]

-- Пересоздаём окно настроек с новыми элементами
local _origCreateOptions = createOptionsWindow
createOptionsWindow = function()
    if OPT.window and isElement(OPT.window) then
        guiSetVisible(OPT.window, true)
        showCursor(true)
        OPT.visible = true
        return
    end

    local W, H = 540, 540
    local sx, sy = guiGetScreenSize()
    local wx = (sx - W) / 2
    local wy = (sy - H) / 2

    OPT.window = guiCreateWindow(wx, wy, W, H, "JBK Bot — Настройки (F10)", false)
    guiSetVisible(OPT.window, false)

    -- СОСТОЯНИЕ БОТА
    guiCreateLabel(10, 25, 200, 16, "СОСТОЯНИЕ БОТА", false, OPT.window)
    local lblStatus = guiCreateLabel(10, 44, 500, 18,
        "Бот: " .. (_STATE and "ВКЛЮЧЁН" or "выключен"), false, OPT.window)
    local btnToggle = guiCreateButton(10, 64, 150, 28,
        _STATE and "Выключить бота" or "Включить бота", false, OPT.window)

    -- РЕЖИМ ДВИЖЕНИЯ
    guiCreateLabel(10, 105, 200, 16, "РЕЖИМ ДВИЖЕНИЯ", false, OPT.window)
    local btnStrict = guiCreateButton(10, 124, 200, 28,
        STRICT and "Строгий режим: ВКЛ" or "Строгий режим: ВЫКЛ", false, OPT.window)
    guiCreateLabel(220, 130, 290, 16, "Мгновенный поворот к цели", false, OPT.window)

    guiCreateLabel(10, 160, 150, 16, "Плавность поворота:", false, OPT.window)
    local sldRotation = guiCreateScrollBar(160, 160, 200, 16, true, false, OPT.window)
    guiScrollBarSetScrollPosition(sldRotation, Settings.RotationSpeed * 10)
    local lblRotVal = guiCreateLabel(370, 160, 60, 16, "x"..Settings.RotationSpeed, false, OPT.window)

    -- BUNNY HOP
    guiCreateLabel(10, 185, 200, 16, "BUNNY HOP", false, OPT.window)
    local chkBhop = guiCreateCheckBox(10, 204, 180, 18,
        "BunnyHop включён", Settings.AllowBunnyHop, false, OPT.window)

    guiCreateLabel(10, 226, 130, 16, "Задержка (мс):", false, OPT.window)
    local editBhopDelay = guiCreateEdit(145, 224, 70, 20,
        tostring(Settings.BunnyHopDelay), false, OPT.window)
    guiCreateLabel(225, 226, 200, 16, "чем меньше — чаще прыгает", false, OPT.window)

    guiCreateLabel(10, 250, 130, 16, "Мин. дистанция:", false, OPT.window)
    local editBhopDist = guiCreateEdit(145, 248, 70, 20,
        tostring(Settings.BunnyHopMinDistance), false, OPT.window)
    guiCreateLabel(225, 250, 200, 16, "прыгать если цель дальше N м", false, OPT.window)

    -- ВИЗУАЛИЗАЦИЯ
    guiCreateLabel(10, 278, 200, 16, "ВИЗУАЛИЗАЦИЯ И УВЕДОМЛЕНИЯ", false, OPT.window)
    local chkVis     = guiCreateCheckBox(10,  297, 200, 18, "Показывать путь (jbk0.2)", VIS.active, false, OPT.window)
    local chkNumbers = guiCreateCheckBox(10,  318, 200, 18, "Нумерация точек", VIS.showNumbers, false, OPT.window)
    local chkRadius  = guiCreateCheckBox(10,  339, 200, 18, "Радиус захвата маркера", VIS.showRadius, false, OPT.window)
    local chkDebug   = guiCreateCheckBox(270, 297, 200, 18, "Дебаггер (jbk0.1)", DEBUG.active, false, OPT.window)
    local chkMLog    = guiCreateCheckBox(270, 318, 200, 18, "Лог маркеров", MARKER_LOG, false, OPT.window)
    -- НОВАЯ галочка — вывод сообщений о маркерах в чат
    local chkMarkerChat = guiCreateCheckBox(270, 339, 240, 18,
        "Сообщения о маркерах в чат", SHOW_MARKER_CHAT, false, OPT.window)

    -- ДЕТЕКТОР ЗАСТРЕВАНИЯ
    guiCreateLabel(10, 368, 200, 16, "ДЕТЕКТОР ЗАСТРЕВАНИЯ", false, OPT.window)
    guiCreateLabel(10,  387, 120, 16, "Порог (м):", false, OPT.window)
    local editStuck = guiCreateEdit(135, 385, 55, 20, tostring(STUCK_DIST), false, OPT.window)
    guiCreateLabel(200, 387, 100, 16, "Интервал (мс):", false, OPT.window)
    local editStuckTime = guiCreateEdit(305, 385, 65, 20, tostring(STUCK_TIME), false, OPT.window)
    guiCreateLabel(10,  410, 150, 16, "Попыток до пересчёта:", false, OPT.window)
    local editStuckMax = guiCreateEdit(165, 408, 50, 20, tostring(STUCK_MAX), false, OPT.window)

    -- КНОПКИ ВНИЗУ
    local btnApply  = guiCreateButton(10,  445, 110, 30, "Применить", false, OPT.window)
    local btnSim    = guiCreateButton(130, 445, 110, 30, "Симуляция", false, OPT.window)
    local btnFind   = guiCreateButton(250, 445, 110, 30, "Найти маркер", false, OPT.window)
    local btnHelp   = guiCreateButton(370, 445, 80,  30, "Инструкция", false, OPT.window)
    local btnClose  = guiCreateButton(460, 445, 70,  30, "Закрыть", false, OPT.window)

    -- ОБРАБОТЧИКИ
    addEventHandler("onClientGUIClick", btnToggle, function()
        toggleBot()
        guiSetText(btnToggle, _STATE and "Выключить бота" or "Включить бота")
        guiSetText(lblStatus, "Бот: " .. (_STATE and "ВКЛЮЧЁН" or "выключен"))
    end, false)

    addEventHandler("onClientGUIClick", btnStrict, function()
        STRICT = not STRICT
        guiSetText(btnStrict, STRICT and "Строгий режим: ВКЛ" or "Строгий режим: ВЫКЛ")
        outputChatBox("[JBK] Строгий режим: " .. (STRICT and "ВКЛ" or "ВЫКЛ"), 0,200,255,false)
    end, false)

    addEventHandler("onClientGUIScroll", sldRotation, function()
        local val = math.max(1, math.min(100, math.floor(guiScrollBarGetScrollPosition(sldRotation) / 10)))
        Settings.RotationSpeed = val
        guiSetText(lblRotVal, "x"..val)
    end, false)

    addEventHandler("onClientGUIClick", chkBhop, function()
        Settings.AllowBunnyHop = guiCheckBoxGetSelected(chkBhop)
        outputChatBox("[JBK] BunnyHop: " .. (Settings.AllowBunnyHop and "ВКЛ" or "ВЫКЛ"), 0,200,255,false)
    end, false)

    addEventHandler("onClientGUIClick", chkVis, function()
        VIS.active = guiCheckBoxGetSelected(chkVis)
    end, false)
    addEventHandler("onClientGUIClick", chkNumbers, function()
        VIS.showNumbers = guiCheckBoxGetSelected(chkNumbers)
    end, false)
    addEventHandler("onClientGUIClick", chkRadius, function()
        VIS.showRadius = guiCheckBoxGetSelected(chkRadius)
    end, false)
    addEventHandler("onClientGUIClick", chkDebug, function()
        local want = guiCheckBoxGetSelected(chkDebug)
        if want ~= DEBUG.active then toggleDebug() end
    end, false)
    addEventHandler("onClientGUIClick", chkMLog, function()
        MARKER_LOG = guiCheckBoxGetSelected(chkMLog)
    end, false)

    -- Галочка сообщений о маркерах
    addEventHandler("onClientGUIClick", chkMarkerChat, function()
        SHOW_MARKER_CHAT = guiCheckBoxGetSelected(chkMarkerChat)
        outputChatBox("[JBK] Сообщения о маркерах: " ..
            (SHOW_MARKER_CHAT and "ВКЛ" or "ВЫКЛ"), 0,200,255,false)
        -- Сбрасываем кэш чтобы следующее сообщение вышло сразу
        lastMarkerInfo = nil
    end, false)

    -- Применить
    addEventHandler("onClientGUIClick", btnApply, function()
        local d  = tonumber(guiGetText(editStuck))
        local t  = tonumber(guiGetText(editStuckTime))
        local m  = tonumber(guiGetText(editStuckMax))
        local bd = tonumber(guiGetText(editBhopDelay))
        local bm = tonumber(guiGetText(editBhopDist))
        if d  then STUCK_DIST = d end
        if t  then STUCK_TIME = t end
        if m  then STUCK_MAX  = m end
        if bd then Settings.BunnyHopDelay       = bd end
        if bm then Settings.BunnyHopMinDistance = bm end
        outputChatBox(string.format(
            "[JBK] Применено: застревание=%.1fm/%dмс/%d | bhop=%dмс/%.1fm",
            STUCK_DIST, STUCK_TIME, STUCK_MAX,
            Settings.BunnyHopDelay, Settings.BunnyHopMinDistance), 0,200,255,false)
    end, false)

    addEventHandler("onClientGUIClick", btnSim, function()
        startSim(); outputChatBox("[JBK] Симуляция запущена", 0,200,255,false)
    end, false)

    addEventHandler("onClientGUIClick", btnFind, function()
        MARKER_LOG = true
        local mk = Utils.GetJobMarker()
        MARKER_LOG = false
        if mk then
            local mx,my,mz = getElementPosition(mk)
            local r,g,b,a  = getMarkerColor(mk)
            outputChatBox(string.format("[JBK] Маркер: %.2f,%.2f,%.2f rgb=%d,%d,%d,%d",
                mx,my,mz,r,g,b,a), 0,255,100,false)
        else
            outputChatBox("[JBK] Маркер не найден", 255,100,100,false)
        end
    end, false)

    -- Инструкция — открываем отдельное окно
    addEventHandler("onClientGUIClick", btnHelp, function()
        local hw = guiCreateWindow(50, 50, 500, 500, "JBK Bot — Инструкция", false)
        local memo = guiCreateMemo(5, 25, 490, 430, INSTRUCTIONS, false, hw)
        guiMemoSetReadOnly(memo, true)
        local bc = guiCreateButton(190, 462, 120, 30, "Закрыть", false, hw)
        addEventHandler("onClientGUIClick", bc, function()
            destroyElement(hw)
        end, false)
    end, false)

    addEventHandler("onClientGUIClick", btnClose, function()
        guiSetVisible(OPT.window, false)
        showCursor(false)
        OPT.visible = false
    end, false)

    guiSetVisible(OPT.window, true)
    showCursor(true)
    OPT.visible = true
end

-- Патч автовывода маркера — проверяем SHOW_MARKER_CHAT
-- Переопределяем через обёртку над setTimer (уже запущен, добавляем второй таймер)
-- Второй таймер с флагом заменяет вывод первого
setTimer(function()
    if not SHOW_MARKER_CHAT then return end
    local marker = Utils.GetJobMarker()
    if not marker or not isElement(marker) then return end
    local mx, my, mz = getElementPosition(marker)
    local key = string.format("%.1f_%.1f", mx, my)
    if key == lastMarkerInfo then return end
    -- lastMarkerInfo обновляется первым таймером, здесь только вывод
end, 2000, 0)

-- ============================================================
-- JBK ACTION POINTS — точки действий бота
-- /jbkactions — открыть редактор точек
-- Типы: left (шаги влево), right (шаги вправо), jump (прыжок)
-- Работают глобально, независимо от маршрута
-- ============================================================

-- Таблица точек действий — заполняется через GUI или вручную
-- Формат: {x, y, z, type="left"/"right"/"jump", radius=3.0, duration=500}
local ACTION_POINTS = {
    -- #1 right
    {x=38.731, y=-233.717, z=391.258, type="right", radius=0.8, duration=500},
    -- #2 right
    {x=38.731, y=-233.717, z=391.258, type="right", radius=0.8, duration=500},
    -- #3 jump
    {x=20.672, y=-259.994, z=392.407, type="jump", radius=0.8, duration=500},
    -- #4 jump
    {x=21.111, y=-266.067, z=393.805, type="jump", radius=1.0, duration=500},
    -- #5 left
    {x=9.661, y=-224.214, z=391.258, type="left", radius=0.5, duration=500},
    -- #6 jump
    {x=38.873, y=-258.097, z=391.433, type="jump", radius=0.5, duration=500},
    -- #7 jump
    {x=38.884, y=-260.205, z=392.515, type="jump", radius=0.5, duration=500},
    -- #8 jump
    {x=15.592, y=-242.993, z=391.258, type="jump", radius=2.0, duration=500},
    -- #9 left
    {x=33.132, y=-314.644, z=391.258, type="left", radius=3.0, duration=200},
    -- #10 right
    {x=38.928, y=-257.900, z=391.333, type="right", radius=0.5, duration=500},
    -- #11 right
    {x=38.875, y=-260.223, z=392.524, type="right", radius=0.5, duration=500},
   
    {x=38.927, y=-262.621, z=393.750, type="left", radius=1.0, duration=500},
   
    {x=39.974, y=-265.077, z=393.805, type="left", radius=1.0, duration=500},

    {x=39.467, y=-266.180, z=393.805, type="jump", radius=1.0, duration=500},
   
    {x=38.867, y=-272.418, z=391.258, type="jump", radius=1.0, duration=500},
    
    {x=39.541, y=-255.800, z=391.258, type="right", radius=0.5, duration=500},
    
    {x=31.003, y=-240.189, z=391.258, type="jump", radius=2.0, duration=500},
   
    {x=30.915, y=-241.278, z=392.383, type="jump", radius=2.0, duration=500},
  
    {x=32.222, y=-319.772, z=391.258, type="right", radius=0.3, duration=500},
}

-- Состояние выполнения действия
local AP_STATE = {
    active    = false,   -- сейчас выполняется действие
    timer     = nil,     -- таймер действия
    cooldowns = {},      -- кулдаун по индексу точки (чтобы не спамить)
    COOLDOWN  = 3000,    -- мс между повторными срабатываниями одной точки
}

-- ===== ЗАЩИТА: стоп при высоте > 397 или телепорте > 30м =====
-- После остановки ждём 3 минуты и возобновляем
local SAFETY = {
    lastPos       = nil,
    resumeTimer   = nil,
    resumeToken   = 0,
    paused        = false,
    MAX_Z         = 395,      -- выше этой высоты = стоп
    MAX_JUMP_DIST = 30,       -- резкое перемещение > 30м = стоп
    RESUME_DELAY  = 180000,   -- 3 минуты в мс
}

cancelAutoResume = function()
    SAFETY.resumeToken = SAFETY.resumeToken + 1
    if SAFETY.resumeTimer and isTimer(SAFETY.resumeTimer) then
        killTimer(SAFETY.resumeTimer)
    end
    SAFETY.resumeTimer = nil
    SAFETY.paused = false
    SAFETY.lastPos = nil
end

local function safetyStop(reason)
    if not _STATE then return end
    if SAFETY.paused then return end
    SAFETY.paused = true
    _STATE = false
    changeBotState(false)
    outputChatBox("#FF4444[SAFETY] #FFFFFFБот остановлен: " .. reason, 255,255,255,true)
    outputChatBox(string.format("#FF4444[SAFETY] #FFFFFFАвтовозобновление через %.1f мин...",
        SAFETY.RESUME_DELAY / 60000), 255,255,255,true)
    playAlert("safety_stop", 10000)
    -- Включаем удар по админу при остановке
    if ADMINHIT then ADMINHIT.enabled = true end
    -- Убиваем старый таймер если есть
    if SAFETY.resumeTimer then killTimer(SAFETY.resumeTimer) end
    SAFETY.resumeToken = SAFETY.resumeToken + 1
    local resumeToken = SAFETY.resumeToken
    SAFETY.resumeTimer = setTimer(function()
        if resumeToken ~= SAFETY.resumeToken then return end
        SAFETY.resumeTimer = nil
        SAFETY.paused = false
        SAFETY.lastPos = nil
        outputChatBox("#00FF88[SAFETY] #FFFFFFБот автоматически возобновлён", 255,255,255,true)
        -- Проверяем что маркер работы есть перед запуском
        local marker = Utils.GetJobMarker()
        if marker and isElement(marker) then
            _STATE = true
            changeBotState(true)
            outputChatBox("#00FF88[SAFETY] #FFFFFFМаркер найден — бот запущен", 255,255,255,true)
        else
            outputChatBox("#FFAA00[SAFETY] #FFFFFFМаркер не найден — открой Shift+R > JBK Bot и запусти вручную", 255,255,255,true)
        end
    end, SAFETY.RESUME_DELAY, 1)
end

-- Проверка каждые 500мс
setTimer(function()
    if not _STATE then return end
    local px, py, pz = getElementPosition(localPlayer)

    -- Проверка высоты
    if pz > SAFETY.MAX_Z then
        safetyStop(string.format("высота %.1f > %d (телепорт вверх)", pz, SAFETY.MAX_Z))
        return
    end

    -- Проверка резкого перемещения
    if SAFETY.lastPos then
        local dist = getDistanceBetweenPoints3D(
            px, py, pz,
            SAFETY.lastPos.x, SAFETY.lastPos.y, SAFETY.lastPos.z)
        if dist > SAFETY.MAX_JUMP_DIST then
            safetyStop(string.format("резкое перемещение %.1f м", dist))
            return
        end
    end

    SAFETY.lastPos = {x=px, y=py, z=pz}
end, 500, 0)

-- Выполняем действие точки
local function executeAction(pt)
    if AP_STATE.active then return end
    AP_STATE.active = true

    if pt.type == "left" then
        setBotControl("left", true)
        setBotControl("forwards", false)
        AP_STATE.timer = setTimer(function()
            setBotControl("left", false)
            AP_STATE.active = false
        end, pt.duration or 500, 1)

    elseif pt.type == "right" then
        setBotControl("right", true)
        setBotControl("forwards", false)
        AP_STATE.timer = setTimer(function()
            setBotControl("right", false)
            AP_STATE.active = false
        end, pt.duration or 500, 1)

    elseif pt.type == "jump" then
        -- Не прыгаем в NoJump зонах
        if not isInNoJumpZone() then
            setBotControl("jump", true)
            AP_STATE.timer = setTimer(function()
                setBotControl("jump", false)
                AP_STATE.active = false
            end, pt.duration or 300, 1)
        else
            AP_STATE.active = false
        end

    elseif pt.type == "nojump" then
        -- Просто снимаем активность — зона уже учитывается через isInNoJumpZone()
        -- Точка nojump работает как постоянная зона пока бот в радиусе
        AP_STATE.active = false
    end
end

-- Проверяем точки каждые 100мс
setTimer(function()
    if not _STATE then return end
    if AP_STATE.active then return end

    local px, py, pz = getElementPosition(localPlayer)
    local now = getTickCount()

    for i, pt in ipairs(ACTION_POINTS) do
        local dist = getDistanceBetweenPoints3D(px, py, pz, pt.x, pt.y, pt.z)
        local radius = pt.radius or 3.0

        if dist <= radius then
            -- nojump точки не требуют кулдауна — они работают постоянно через isInNoJumpZone()
            if pt.type == "nojump" then
                -- ничего не делаем, зона уже учитывается в isInNoJumpZone()
            else
                -- Проверяем кулдаун для остальных типов
                local lastTime = AP_STATE.cooldowns[i] or 0
                if now - lastTime >= AP_STATE.COOLDOWN then
                    AP_STATE.cooldowns[i] = now
                    executeAction(pt)
                    break
                end
            end
        end
    end
end, 100, 0)

-- ============================================================
-- GUI редактор точек действий /jbkactions
-- ============================================================

local APGUI = { window = nil, visible = false, editIdx = nil }

local function apRefreshList(grid)
    guiGridListClear(grid)
    for i, pt in ipairs(ACTION_POINTS) do
        local row = guiGridListAddRow(grid)
        guiGridListSetItemText(grid, row, 1, tostring(i), false, false)
        guiGridListSetItemText(grid, row, 2, pt.type, false, false)
        guiGridListSetItemText(grid, row, 3,
            string.format("%.1f, %.1f, %.1f", pt.x, pt.y, pt.z), false, false)
        guiGridListSetItemText(grid, row, 4, tostring(pt.radius or 3.0), false, false)
        guiGridListSetItemText(grid, row, 5, tostring(pt.duration or 500), false, false)
    end
end

local function apExportText()
    local lines = {"-- ACTION_POINTS = {"}
    for i, pt in ipairs(ACTION_POINTS) do
        lines[#lines+1] = string.format(
            "    -- #%d %s", i, pt.type)
        lines[#lines+1] = string.format(
            "    {x=%.3f, y=%.3f, z=%.3f, type=%q, radius=%.1f, duration=%d},",
            pt.x, pt.y, pt.z, pt.type, pt.radius or 3.0, pt.duration or 500)
    end
    lines[#lines+1] = "-- }"
    return table.concat(lines, "\n")
end

local function openActionPoints()
    if APGUI.window and isElement(APGUI.window) then
        APGUI.visible = not APGUI.visible
        guiSetVisible(APGUI.window, APGUI.visible)
        showCursor(APGUI.visible)
        return
    end

    local W, H = 620, 540
    local sx, sy = guiGetScreenSize()
    APGUI.window = guiCreateWindow(
        (sx - W) / 2, (sy - H) / 2, W, H,
        "JBK Action Points — Точки действий", false)

    -- Список точек
    guiCreateLabel(10, 25, 400, 16, "Список точек:", false, APGUI.window)
    local grid = guiCreateGridList(10, 44, 600, 180, false, APGUI.window)
    guiGridListAddColumn(grid, "#",        0.05)
    guiGridListAddColumn(grid, "Тип",      0.12)
    guiGridListAddColumn(grid, "Позиция",  0.40)
    guiGridListAddColumn(grid, "Радиус",   0.12)
    guiGridListAddColumn(grid, "Длит(мс)", 0.15)

    -- Форма создания/редактирования
    guiCreateLabel(10, 234, 100, 16, "Тип действия:", false, APGUI.window)
    local radioRight  = guiCreateRadioButton(115, 232, 120, 20, "right — вправо",      false, APGUI.window)
    local radioLeft   = guiCreateRadioButton(115, 252, 120, 20, "left  — влево",       false, APGUI.window)
    local radioJump   = guiCreateRadioButton(115, 272, 120, 20, "jump  — прыжок",      false, APGUI.window)
    local radioNojump = guiCreateRadioButton(115, 292, 160, 20, "nojump — без прыжка", false, APGUI.window)
    guiRadioButtonSetSelected(radioRight, true)

    guiCreateLabel(10, 300, 100, 16, "Радиус (м):", false, APGUI.window)
    local editRadius = guiCreateEdit(115, 298, 60, 20, "3.0", false, APGUI.window)

    guiCreateLabel(185, 300, 110, 16, "Длительность (мс):", false, APGUI.window)
    local editDur = guiCreateEdit(300, 298, 70, 20, "500", false, APGUI.window)

    -- Позиция
    guiCreateLabel(10, 326, 100, 16, "X:", false, APGUI.window)
    local editX = guiCreateEdit(30,  324, 80, 20, "", false, APGUI.window)
    guiCreateLabel(120, 326, 20, 16, "Y:", false, APGUI.window)
    local editY = guiCreateEdit(135, 324, 80, 20, "", false, APGUI.window)
    guiCreateLabel(225, 326, 20, 16, "Z:", false, APGUI.window)
    local editZ = guiCreateEdit(240, 324, 80, 20, "", false, APGUI.window)

    local btnGetPos = guiCreateButton(330, 324, 130, 20,
        "Взять мою позицию", false, APGUI.window)

    -- Кнопки управления
    local btnAdd    = guiCreateButton(10,  355, 100, 28, "Добавить",     false, APGUI.window)
    local btnUpdate = guiCreateButton(120, 355, 100, 28, "Обновить",     false, APGUI.window)
    local btnDel    = guiCreateButton(230, 355, 100, 28, "Удалить",      false, APGUI.window)
    local btnClear  = guiCreateButton(340, 355, 100, 28, "Очистить всё", false, APGUI.window)

    -- Экспорт
    guiCreateLabel(10, 393, 200, 16, "Экспорт (скопируй в скрипт):", false, APGUI.window)
    local memo = guiCreateMemo(10, 412, 600, 80, "", false, APGUI.window)
    guiMemoSetReadOnly(memo, true)

    local btnExport = guiCreateButton(10,  500, 120, 30, "Экспортировать", false, APGUI.window)
    local btnClose  = guiCreateButton(490, 500, 120, 30, "Закрыть",        false, APGUI.window)

    -- Вспомогательная функция — тип из радиокнопок
    local function getSelectedType()
        if guiRadioButtonGetSelected(radioRight)  then return "right"
        elseif guiRadioButtonGetSelected(radioLeft)   then return "left"
        elseif guiRadioButtonGetSelected(radioJump)   then return "jump"
        elseif guiRadioButtonGetSelected(radioNojump) then return "nojump"
        else return "right" end
    end

    -- Взять позицию игрока
    addEventHandler("onClientGUIClick", btnGetPos, function()
        local x, y, z = getElementPosition(localPlayer)
        guiSetText(editX, string.format("%.3f", x))
        guiSetText(editY, string.format("%.3f", y))
        guiSetText(editZ, string.format("%.3f", z))
    end, false)

    -- Выбор строки — заполняем форму
    addEventHandler("onClientGUIClick", grid, function()
        local row, _ = guiGridListGetSelectedItem(grid)
        if row and row ~= -1 then
            local idx = tonumber(guiGridListGetItemText(grid, row, 1))
            if idx and ACTION_POINTS[idx] then
                local pt = ACTION_POINTS[idx]
                APGUI.editIdx = idx
                guiSetText(editX, string.format("%.3f", pt.x))
                guiSetText(editY, string.format("%.3f", pt.y))
                guiSetText(editZ, string.format("%.3f", pt.z))
                guiSetText(editRadius, tostring(pt.radius or 3.0))
                guiSetText(editDur, tostring(pt.duration or 500))
                guiRadioButtonSetSelected(radioRight,  pt.type == "right")
                guiRadioButtonSetSelected(radioLeft,   pt.type == "left")
                guiRadioButtonSetSelected(radioJump,   pt.type == "jump")
                guiRadioButtonSetSelected(radioNojump, pt.type == "nojump")
            end
        end
    end, false)

    -- Добавить точку
    addEventHandler("onClientGUIClick", btnAdd, function()
        local x = tonumber(guiGetText(editX))
        local y = tonumber(guiGetText(editY))
        local z = tonumber(guiGetText(editZ))
        if not x or not y or not z then
            outputChatBox("[AP] Нажми 'Взять мою позицию' или введи координаты", 255,150,0,false)
            return
        end
        local pt = {
            x        = x,
            y        = y,
            z        = z,
            type     = getSelectedType(),
            radius   = tonumber(guiGetText(editRadius)) or 3.0,
            duration = tonumber(guiGetText(editDur)) or 500,
        }
        table.insert(ACTION_POINTS, pt)
        apRefreshList(grid)
        outputChatBox(string.format("[AP] Добавлена точка #%d [%s] r=%.1f",
            #ACTION_POINTS, pt.type, pt.radius), 0,200,255,false)
    end, false)

    -- Обновить выбранную точку
    addEventHandler("onClientGUIClick", btnUpdate, function()
        local idx = APGUI.editIdx
        if not idx or not ACTION_POINTS[idx] then
            outputChatBox("[AP] Выбери точку в списке", 255,150,0,false)
            return
        end
        local x = tonumber(guiGetText(editX))
        local y = tonumber(guiGetText(editY))
        local z = tonumber(guiGetText(editZ))
        if not x or not y or not z then return end
        ACTION_POINTS[idx] = {
            x        = x,
            y        = y,
            z        = z,
            type     = getSelectedType(),
            radius   = tonumber(guiGetText(editRadius)) or 3.0,
            duration = tonumber(guiGetText(editDur)) or 500,
        }
        apRefreshList(grid)
        outputChatBox("[AP] Точка #" .. idx .. " обновлена", 0,200,255,false)
    end, false)

    -- Удалить выбранную точку
    addEventHandler("onClientGUIClick", btnDel, function()
        local idx = APGUI.editIdx
        if not idx or not ACTION_POINTS[idx] then
            outputChatBox("[AP] Выбери точку в списке", 255,150,0,false)
            return
        end
        table.remove(ACTION_POINTS, idx)
        APGUI.editIdx = nil
        apRefreshList(grid)
        outputChatBox("[AP] Точка удалена", 0,200,255,false)
    end, false)

    -- Очистить все точки
    addEventHandler("onClientGUIClick", btnClear, function()
        ACTION_POINTS = {}
        AP_STATE.cooldowns = {}
        APGUI.editIdx = nil
        apRefreshList(grid)
        outputChatBox("[AP] Все точки удалены", 0,200,255,false)
    end, false)

    -- Экспорт
    addEventHandler("onClientGUIClick", btnExport, function()
        guiMemoSetReadOnly(memo, false)
        guiSetText(memo, apExportText())
        guiMemoSetReadOnly(memo, true)
        outputChatBox("[AP] Скопируй текст из поля и вставь в ACTION_POINTS", 0,200,255,false)
    end, false)

    -- Закрыть
    addEventHandler("onClientGUIClick", btnClose, function()
        guiSetVisible(APGUI.window, false)
        showCursor(false)
        APGUI.visible = false
    end, false)

    apRefreshList(grid)
    guiSetVisible(APGUI.window, true)
    showCursor(true)
    APGUI.visible = true
end



-- ============================================================
-- JBK ADMIN WATCHER — интегрированный чекер админов
-- Список из копии + автодетект из чата
-- При появлении админа рядом — сирена и остановка бота
-- ============================================================

-- ===== СПИСОК АДМИНОВ (сервер 185.71.66.70) =====
-- Добавь свой сервер или оставь как есть
local ADMIN_NAMES = {}
do
    local serverAdmins = {
        ["185.71.66.80:22003"] = {
            "Emily_Quincy","Dmitriy_Ogonkov","Alim_Komarov","Jack_Morozov","Sergey_Streltsov",
            "Adrian_Litvintsev","Alex_Morrison","Alexander_Grozniy","Andrey_Semp","Andrey_Valyaev",
            "Artem_Augustov","Artemiy_Lisiuk","Egor_Sobakevich","Ivan_Gryb","Kamilla_Florenz",
            "Maksim_Harlamow","Melody_Wayne","Nestor_Rutherford","Nick_Kotik","Vladimir_Smash",
            "Wolfgang_Schneiderhan","Yuriy_Sokolovskiy","Johnny_Smith","Kiyotaki_Darkness",
            "Pavel_Toporov","David_Seliverstov","Ivan_Prahodskiy","Matthew_Rutherford","Veynar_Halvardsen","Aleksey_Bombovich2"
        },
        ["185.71.66.70:22003"] = {
            "Maria_Alekseeva","Rodion_Topolskiy","Alexander_Krutov","Arseniy_Maltsev","William_Great",
            "Alexander_Gasanov","Antonio_Zubenko","Ksenia_Groznaya","Franklin_White","Daniil_Lantratov",
            "Evgeny_Grossman","Illya_Santiz","Jack_Turner","Vladislav_Alen","Ludwig_Salvatore",
            "Timofey_Fabin","Karolina_Kaspiyskaya","Leo_Guerra","Max_Dante","Roman_Lisov",
            "Vladislav_Townley","Maxim_Gornadzorov","Maksim_Alferov","Mia_Krutova","Aleksandr_Grozniy",
            "Aleksandr_Biketov","Don_Vice","Daniil_Wolskiy","Dmitriy_Ostrovskiy","Nik_Romadin",
            "Pablo_King","Vyacheslav_Rublev","Dmitry_Pretty","Stefan_King","Danil_Naumow",
            "Tony_King","Kita_Kayman","Reimondo_Grossman"
        },
        ["185.71.66.79:22003"] = {
            "Vladislav_Kiselev","Eric_Collins","Danil_Astov","Dmitriy_Scheglov","Eduard_Vysotskiy",
            "Fedor_Khalifa","Prokhor_Lukoyanov","Vladislav_Sutagin","Alex_Trushin","Alexandr_Yankee",
            "Alina_Solntsevskaya","Anastasia_Crossman","Dmitriy_Dennica","George_Gaiduk","Ivan_Ryzov",
            "Kirill_Mirnyy","Marat_Sorokin","Radmir_Pyatkin","Rodion_Vistnik","Sergei_Black",
            "Thomas_Barinov","Vladislav_Macalister","Walter_Neal","Balthazar_Moskov","Elisey_Marlboro",
            "Francesco_Grizzly","Mironu_Kataray","Sergei_Yamrazh","Daniil_Shock","Ilya_Anillov",
            "Roman_Ray","Aaron_Campbell","Artur_Iskandarov","Aurora_Vlasova","Danil_Moskov",
            "Daniil_Tairov","Egor_Hill","Jevgeni_Djagilev","Maksim_Kancler","Richard_Frank","Yuko_Mori"
        },
        ["185.71.66.64:22003"] = {
            "Claus_Nevskiy","Anastasia_MacAlister","Sevil_Esposito","Melissa_Witty","Evgeniy_Holmes",
            "Serg_Antonov","Augustine_Morgan","Artem_Fedukov","Pavel_Morello","Robert_Dobrov",
            "Mirella_Mayers","Varlam_Bobko","Matthew_Esposito","Anthony_Manrique","Benjamin_Watson",
            "Alexander_Potok","Dmitriy_Prostorov","Saburo_Itto","Anatoliy_Vercetti","Nick_Tverskoy",
            "Yaroslav_Mayers","Xavier_Manchine","Vasiliy_Tverskoy","Anna_Tverskaya","Anatoliy_Nesterov",
            "Anton_Sambur","Ali_Henderson","Maximilian_Watanabe","Chad_Morgan","Floren_Winners",
            "Aman_Dubrovskiy","Kirill_Reall","Norman_Excellent"
        },
        ["185.71.66.66:22003"] = {
            "Platon_Seven","Kai_Mironov","Alexandr_Silych","Daniil_Caffrey","Dmitriy_Polanski",
            "Gottfried_Boyarin","Sergey_Belikov","Aleksey_Khrusch","Alex_Gutmann","Alex_Vatkov",
            "Alexandr_Venevtsev","Artem_Tyhkanov","Diana_Creighton","Dmitriy_Repin","Dmitry_Tomin",
            "Emmanuel_Deus","Mitsuo_Fox","Robert_Sychev","Rostislav_Imenov","Alex_Robinson",
            "Alexander_McCartney","Amin_Cherry","Danil_Polaneychick","Aleksandr_McLaren","Artem_Bobrovskiy",
            "Giuseppe_Gazdeliani","Mark_Andrusenko","Artem_Rose","Konstantin_Kurochkin","Mehad_Still"
        },
        ["185.71.66.81:22003"] = {
            "Denis_Manafort","Andrey_Novak","Markus_Berg","Elizaveta_Berg","Alexander_Good",
            "Paul_Good","David_Bystrov","Artem_Darmin","Georgiy_Zhilin","Maksim_KuIiy",
            "Arthur_Daniels","Averardo_Capone","Vladislav_Manarskiy","Polter_Sokirovskiy","Oliver_Capone",
            "Savva_Sharkov","Hades_Manarskiy","Andrew_Maguire","Shiro_Vi","Sergey_Berg",
            "Osiris_Reinhardt","Egoriy_Bobryshev","Yuriy_Topolskin","Hugo_Wolf","Agato_Massino",
            "Andrey_Loverd","Ilya_Bobryshev","Emmanuel_Capone","Vladislav_Verkalo","Lee_Capone",
            "Selvester_Loverd","Shai_Massino","Ajay_Jackson","Monte_Good","August_Hoffmann",
            "Dany_Good","Moki_Nellson","Attaviano_Capone","Mike_McCoy","Vadim_Good"
        },
        ["185.71.66.88:22003"] = {
            "Anthony_Paris","Arnold_Fenix","Stanislav_Zybinskiy","Artemiy_Kornyakov","Leonid_Bosow",
            "Ivan_Homyakov","Anton_Marshalov","Artemiy_Mikado","Denis_Milize","Anton_Zalutcki",
            "Daniel_Harrington","Aleksey_Novgorodskiy","Wyatt_Yeat","Nikolay_Bosow","Jet_Rakhimov",
            "August_Verstappen","Kevin_Reichelderfer","Mark_Admiralov","Michael_Mikado","Sergey_Sheremetev",
            "Mars_Holmes","Valentin_Mikado","Zagid_Mikado","Nathaniel_Revazov","Felix_Kogut",
            "Lina_Reichelderfer","Daniele_Homyakov","Nikolay_Wolf","Potap_Hennessy","Fedor_Santoni",
            "Matteo_Williams","Cary_Mikado","Maxim_Sharganov","Aleksey_Akimov","Chapman_Navarro",
            "Pavel_Homyakov","Arsenii_Harrington","Ryan_Lindberg","Hariton_Bosow","Anthony_Verov",
            "Mark_Lotkov","Nikita_Muver","Ethan_Mikado","Christopher_Winchester","Khavr_Zakharov"
        }
    }
    -- Загружаем список для текущего сервера
    local ip = getServerIp and getServerIp(true) or ""
    local list = serverAdmins[ip] or {}
    for _, v in ipairs(list) do
        ADMIN_NAMES[v] = true
    end
end

-- Автодетект новых ников из чата
local AW_PATTERNS = {
    "администратором%s+([A-Za-z]+_[A-Za-z]+)",
    "администратор%s+([A-Za-z]+_[A-Za-z]+)",
    "Admin%s+([A-Za-z]+_[A-Za-z]+)",
    "Администратор%s+([A-Za-z]+_[A-Za-z]+)",
    "заблокирован%s+([A-Za-z]+_[A-Za-z]+)",
    "наказан%s+([A-Za-z]+_[A-Za-z]+)",
    "предупреждён%s+([A-Za-z]+_[A-Za-z]+)",
    "предупрежден%s+([A-Za-z]+_[A-Za-z]+)",
}

-- Состояние чекера
local AW = {
    adminNearby   = false,   -- есть ли админ рядом
    adminNick     = nil,     -- ник ближайшего админа
    stopOnAdmin   = true,
    SCAN_RADIUS   = 200,     -- радиус поиска (весь стриминг)
}

-- Очищаем ник от цветовых кодов
local function awClean(n)
    return (n or ""):gsub("#%x%x%x%x%x%x", "")
end

-- Проверяем является ли ник админом
local function isAdmin(nick)
    return ADMIN_NAMES[nick] == true
end

addEventHandler("onClientPlayerJoin", root, function()
    if not _STATE then return end
    local player = source
    setTimer(function()
        if not _STATE or not isElement(player) then return end
        local nick = awClean(getPlayerNametagText(player) or getPlayerName(player) or "")
        if isAdmin(nick) then
            outputChatBox("#FFAA00[AW] #FFFFFFАдмин зашёл на сервер: " .. nick, 255,255,255,true)
        end
    end, 500, 1)
end)

addEventHandler("onClientPlayerQuit", root, function()
    if not _STATE then return end
    local nick = awClean(getPlayerNametagText(source) or getPlayerName(source) or "")
    if isAdmin(nick) then
        outputChatBox("#00FF88[AW] #FFFFFFАдмин вышел с сервера: " .. nick, 255,255,255,true)
    end
end)

-- Сканируем игроков рядом на наличие админов
setTimer(function()
    if not _STATE then return end
    local adminFound = false
    local adminNick  = nil

    for _, p in ipairs(getElementsByType("player", root, true)) do
        if p ~= localPlayer then
            local nick = awClean(getPlayerNametagText(p) or "")
            if isAdmin(nick) then
                local px, py, pz = getElementPosition(localPlayer)
                local ax, ay, az = getElementPosition(p)
                if getDistanceBetweenPoints3D(px, py, pz, ax, ay, az) <= AW.SCAN_RADIUS then
                    adminFound = true
                    adminNick  = nick
                    break
                end
            end
        end
    end

    if adminFound and not AW.adminNearby then
        -- Админ появился
        AW.adminNearby  = true
        AW.adminNick    = adminNick
        outputChatBox("#FF4444[AW] #FFFFFFАдмин рядом: " .. adminNick, 255,255,255,true)
        playAlert("admin_near", 30000)
        -- Останавливаем бота
        if AW.stopOnAdmin then
            cancelAutoResume()
            if _STATE then
                _STATE = false
                changeBotState(false)
                outputChatBox("#FF4444[AW] #FFFFFFБот полностью остановлен (админ рядом)", 255,255,255,true)
            end
        end
    elseif not adminFound and AW.adminNearby then
        -- Админ ушёл
        AW.adminNearby = false
        outputChatBox("#00FF88[AW] #FFFFFFАдмин ушёл: " .. (AW.adminNick or "?"), 255,255,255,true)
        AW.adminNick = nil
    end
end, 2000, 0)

-- Слушаем чат — реагируем только на сообщения от известных админов
local awChatBusy = false
addEventHandler("onClientChatMessage", root, function(text, r, g, b)
    if awChatBusy then return end
    if type(text) ~= "string" or text == "" then return end
    local activeForMessage = _STATE or (activeChatText == text
        and getTickCount() - activeChatTick <= 250)
    activeChatText = nil
    if not activeForMessage then return end
    awChatBusy = true
    local presenceMessage = isAdminPresenceMessage(text)

    -- Автодетект новых ников
    for _, pat in ipairs(AW_PATTERNS) do
        local nick = text:match(pat)
        if nick and nick:match("^[A-Z][a-z]+_[A-Z][a-z]+$") then
            if not ADMIN_NAMES[nick] then
                ADMIN_NAMES[nick] = true
                outputChatBox("#FF8800[AW] #FFFFFFНовый админ из чата: " .. nick, 255,255,255,true)
                if not presenceMessage then playAlert("admin_message", 5000) end
            end
            break
        end
    end

    -- Формат Province: "Ник[ID]: текст". Расстояние до админа не важно.
    local senderNick = text:match("^([A-Za-z_]+%[%d+%]):")
    if senderNick then
        senderNick = senderNick:match("^([A-Za-z_]+)")
    end
    if senderNick and isAdmin(senderNick) and not presenceMessage then
        playAlert("admin_message", 5000)
    end

    awChatBusy = false
end)


-- ============================================================
-- JBK ADMIN HIT — удар по админу раз в 30 сек если он рядом
-- /jbkhit — включить/выключить
-- Если админ в радиусе 8м — подходим и бьём один раз в 30 сек
-- ============================================================

ADMINHIT = {
    enabled   = false,
    lastHit   = 0,
    COOLDOWN  = 30000,   -- 30 сек между ударами
    RADIUS    = 8.0,     -- радиус обнаружения
    HIT_DIST  = 1.5,     -- дистанция для удара
    walking   = false,   -- сейчас идём к цели
    target    = nil,     -- текущая цель (player element)
}

-- Команда: добавить зону на текущей позиции

-- Команда: удалить зону по номеру

-- Команда: список зон

-- ============================================================
-- ЖИВАЯ ПОХОДКА — покачивание влево/вправо во время движения
-- ============================================================

WALK = {
    enabled     = false,
    interval    = 1200,   -- интервал между шагами (мс)
    stepDur     = 200,    -- длительность нажатия клавиши (мс)
    minDist     = 15.0,   -- минимальное расстояние до цели чтобы шагать (как BunnyHop)
    timer       = nil,
    phase       = 0,      -- чередование: 1 = влево, -1 = вправо
    isMoving    = false,  -- сейчас выполняется боковой шаг
    stepTimer   = nil,
}

local function walkTick()
    if not _STATE then return end
    if WALK.isMoving then return end

    -- Шагаем только когда несём ящик до склада
    if STORAGE.FINISH.type ~= "storage" then return end

    -- Проверяем что есть цель
    if not STORAGE.TARGET.element or not isElement(STORAGE.TARGET.element) then return end

    -- Проверяем минимальное расстояние до цели (как BunnyHop)
    local lx, ly, lz = getElementPosition(localPlayer)
    local tx, ty, tz = getElementPosition(STORAGE.TARGET.element)
    local dist = getDistanceBetweenPoints3D(lx, ly, lz, tx, ty, tz)
    if dist < WALK.minDist then return end

    -- Чередуем влево/вправо
    WALK.phase = -WALK.phase
    if WALK.phase == 0 then WALK.phase = 1 end

    local key = WALK.phase > 0 and "left" or "right"

    WALK.isMoving = true
    setBotControl(key, true)

    WALK.stepTimer = setTimer(function()
        setBotControl(key, false)
        WALK.isMoving = false
    end, WALK.stepDur, 1)
end

startWalk = function()
    if WALK.timer then killTimer(WALK.timer); WALK.timer = nil end
    WALK.timer = setTimer(walkTick, WALK.interval, 0)
end

stopWalk = function()
    if WALK.timer    then killTimer(WALK.timer);    WALK.timer    = nil end
    if WALK.stepTimer then killTimer(WALK.stepTimer); WALK.stepTimer = nil end
    setBotControl("left", false)
    setBotControl("right", false)
    WALK.isMoving = false
    WALK.phase    = 0
end

-- Запускаем вместе с ботом
addEventHandler("onClientResourceStart", resourceRoot, function()
    if WALK.enabled then startWalk() end
end)

-- Тик каждые 500мс
setTimer(function()
    if not _STATE or not ADMINHIT.enabled then return end

    local now = getTickCount()
    local px, py, pz = getElementPosition(localPlayer)

    -- Ищем ближайшего админа в радиусе 8м
    local closestAdmin = nil
    local closestDist  = ADMINHIT.RADIUS

    for _, p in ipairs(getElementsByType("player", root, true)) do
        if p ~= localPlayer then
            local nick = awClean(getPlayerNametagText(p) or "")
            if isAdmin(nick) then
                local tx, ty, tz = getElementPosition(p)
                local d = getDistanceBetweenPoints3D(px, py, pz, tx, ty, tz)
                if d < closestDist then
                    closestDist  = d
                    closestAdmin = p
                end
            end
        end
    end

    if not closestAdmin then
        -- Нет админа рядом — останавливаем движение к цели
        if ADMINHIT.walking then
            ADMINHIT.walking = false
            ADMINHIT.target  = nil
            setBotControl("forwards", false)
        end
        return
    end

    -- Кулдаун не истёк
    if now - ADMINHIT.lastHit < ADMINHIT.COOLDOWN then return end

    ADMINHIT.target = closestAdmin
    local tx, ty, tz = getElementPosition(closestAdmin)
    local dist = getDistanceBetweenPoints3D(px, py, pz, tx, ty, tz)

    -- Всегда смотрим на цель
    local angle = -math.deg(math.atan2(tx - px, ty - py))
    setPedCameraRotation(localPlayer, angle)

    if dist <= ADMINHIT.HIT_DIST then
        -- Достаточно близко — бьём
        ADMINHIT.walking = false
        setBotControl("forwards", false)
        setBotControl("fire", true)
        setTimer(function()
            setBotControl("fire", false)
        end, 200, 1)
        ADMINHIT.lastHit = now
        local nick = awClean(getPlayerNametagText(closestAdmin) or "?")
        outputChatBox("#FF4444[HIT] #FFFFFFУдарил админа: " .. nick, 255,255,255,true)
    else
        -- Идём к цели
        ADMINHIT.walking = true
        setBotControl("forwards", true)
    end
end, 500, 0)



-- ============================================================
-- JBK OPTIONS v2 — расширенное меню настроек
-- Переопределяем createOptionsWindow с новыми секциями
-- ============================================================

local _prevCreateOptions = createOptionsWindow
createOptionsWindow = function()
    -- Если окно уже открыто — просто показываем
    if OPT.window and isElement(OPT.window) then
        syncOptionsBotState()
        guiSetVisible(OPT.window, true)
        showCursor(true)
        OPT.visible = true
        return
    end

    local W, H = 600, 640
    local sx, sy = guiGetScreenSize()
    OPT.window = guiCreateWindow(
        (sx-W)/2, (sy-H)/2, W, H,
        "JBK Bot — Настройки v2 (F10)", false)
    guiSetVisible(OPT.window, false)

    -- ===== СЕКЦИЯ: БОТ =====
    guiCreateLabel(10, 25, 580, 16, "БОТ", false, OPT.window)
    local lblStatus = guiCreateLabel(10, 44, 580, 18,
        "Бот: " .. (_STATE and "ВКЛЮЧЁН" or "выключен"), false, OPT.window)
    local btnToggle = guiCreateButton(10, 64, 140, 26,
        _STATE and "Выключить бота" or "Включить бота", false, OPT.window)
    OPT.statusLabel = lblStatus
    OPT.toggleButton = btnToggle
    local btnStrict = guiCreateButton(160, 64, 160, 26,
        STRICT and "Строгий режим: ВКЛ" or "Строгий режим: ВЫКЛ", false, OPT.window)

    guiCreateLabel(10, 96, 120, 16, "Плавность:", false, OPT.window)
    local sldRot = guiCreateScrollBar(130, 96, 180, 16, true, false, OPT.window)
    guiScrollBarSetScrollPosition(sldRot, Settings.RotationSpeed * 10)
    local lblRot = guiCreateLabel(318, 96, 50, 16, "x"..Settings.RotationSpeed, false, OPT.window)

    local chkBhop = guiCreateCheckBox(10, 118, 180, 18,
        "BunnyHop", Settings.AllowBunnyHop, false, OPT.window)
    guiCreateLabel(200, 118, 80, 16, "Задержка:", false, OPT.window)
    local editBhopD = guiCreateEdit(285, 116, 60, 20, tostring(Settings.BunnyHopDelay), false, OPT.window)
    guiCreateLabel(355, 118, 60, 16, "Мин.дист:", false, OPT.window)
    local editBhopM = guiCreateEdit(420, 116, 60, 20, tostring(Settings.BunnyHopMinDistance), false, OPT.window)

    -- ===== СЕКЦИЯ: БЕЗОПАСНОСТЬ =====
    guiCreateLabel(10, 145, 580, 16, "БЕЗОПАСНОСТЬ", false, OPT.window)

    guiCreateLabel(10, 164, 120, 16, "Макс. высота Z:", false, OPT.window)
    local editMaxZ = guiCreateEdit(135, 162, 60, 20, tostring(SAFETY.MAX_Z), false, OPT.window)

    guiCreateLabel(210, 164, 130, 16, "Макс. прыжок (м):", false, OPT.window)
    local editMaxJump = guiCreateEdit(345, 162, 60, 20, tostring(SAFETY.MAX_JUMP_DIST), false, OPT.window)

    guiCreateLabel(10, 188, 150, 16, "Пауза (мин):", false, OPT.window)
    local editResume = guiCreateEdit(165, 186, 60, 20,
        tostring(math.floor(SAFETY.RESUME_DELAY/60000)), false, OPT.window)

    -- ===== СЕКЦИЯ: ДЕТЕКТОР ЗАСТРЕВАНИЯ =====
    guiCreateLabel(10, 214, 580, 16, "ДЕТЕКТОР ЗАСТРЕВАНИЯ", false, OPT.window)

    guiCreateLabel(10, 233, 100, 16, "Порог (м):", false, OPT.window)
    local editStuck = guiCreateEdit(115, 231, 55, 20, tostring(STUCK_DIST), false, OPT.window)
    guiCreateLabel(180, 233, 100, 16, "Интервал (мс):", false, OPT.window)
    local editStuckT = guiCreateEdit(285, 231, 65, 20, tostring(STUCK_TIME), false, OPT.window)
    guiCreateLabel(360, 233, 100, 16, "Попыток:", false, OPT.window)
    local editStuckM = guiCreateEdit(465, 231, 50, 20, tostring(STUCK_MAX), false, OPT.window)

    -- ===== СЕКЦИЯ: ТОЧКИ ПРОБЛЕМНЫХ МЕСТ =====
    guiCreateLabel(10, 260, 580, 16, "ТОЧКИ ПРОБЛЕМНЫХ МЕСТ", false, OPT.window)

    local apGrid = guiCreateGridList(10, 278, 580, 100, false, OPT.window)
    guiGridListAddColumn(apGrid, "#",     0.05)
    guiGridListAddColumn(apGrid, "Тип",   0.12)
    guiGridListAddColumn(apGrid, "X,Y,Z", 0.45)
    guiGridListAddColumn(apGrid, "R",     0.10)
    guiGridListAddColumn(apGrid, "мс",    0.12)

    local function refreshApGrid()
        guiGridListClear(apGrid)
        for i, pt in ipairs(ACTION_POINTS) do
            local row = guiGridListAddRow(apGrid)
            guiGridListSetItemText(apGrid, row, 1, tostring(i), false, false)
            guiGridListSetItemText(apGrid, row, 2, pt.type, false, false)
            guiGridListSetItemText(apGrid, row, 3,
                string.format("%.1f,%.1f,%.1f", pt.x, pt.y, pt.z), false, false)
            guiGridListSetItemText(apGrid, row, 4, tostring(pt.radius or 3), false, false)
            guiGridListSetItemText(apGrid, row, 5, tostring(pt.duration or 500), false, false)
        end
    end
    refreshApGrid()

    local btnOpenAP = guiCreateButton(10, 382, 180, 24,
        "Открыть редактор точек", false, OPT.window)

    -- ===== СЕКЦИЯ: ADMIN WATCHER =====
    guiCreateLabel(10, 414, 580, 16, "ADMIN WATCHER", false, OPT.window)

    local chkAW = guiCreateCheckBox(10, 433, 200, 18,
        "Останавливать бота при админе", AW.stopOnAdmin, false, OPT.window)

    local chkAntiAFK = guiCreateCheckBox(220, 433, 220, 18,
        "Anti-AFK: ПКМ каждые 2 сек", Settings.AntiAFK, false, OPT.window)

    -- ===== СЕКЦИЯ: УДАР ПО АДМИНУ =====
    guiCreateLabel(10, 476, 580, 16, "УДАР ПО АДМИНУ", false, OPT.window)

    local chkHit = guiCreateCheckBox(10, 495, 200, 18,
        "Удар по админу", ADMINHIT.enabled, false, OPT.window)

    guiCreateLabel(220, 495, 80, 16, "Радиус (м):", false, OPT.window)
    local editHitR = guiCreateEdit(305, 493, 50, 20, tostring(ADMINHIT.RADIUS), false, OPT.window)

    guiCreateLabel(365, 495, 100, 16, "Кулдаун (сек):", false, OPT.window)
    local editHitCD = guiCreateEdit(470, 493, 60, 20,
        tostring(math.floor(ADMINHIT.COOLDOWN/1000)), false, OPT.window)

    -- ===== СЕКЦИЯ: ЖИВАЯ ПОХОДКА =====
    guiCreateLabel(10, 522, 580, 16, "ЖИВАЯ ПОХОДКА (только при несении ящика до склада)", false, OPT.window)

    local chkWalk = guiCreateCheckBox(10, 541, 200, 18,
        "Покачивание влево/вправо", WALK.enabled, false, OPT.window)

    guiCreateLabel(220, 541, 90, 16, "Интервал (мс):", false, OPT.window)
    local editWalkInt = guiCreateEdit(315, 539, 55, 20,
        tostring(WALK.interval), false, OPT.window)

    guiCreateLabel(380, 541, 90, 16, "Длит. шага (мс):", false, OPT.window)
    local editWalkDur = guiCreateEdit(475, 539, 50, 20,
        tostring(WALK.stepDur), false, OPT.window)

    guiCreateLabel(220, 559, 110, 16, "Мин. дист. до цели:", false, OPT.window)
    local editWalkDist = guiCreateEdit(335, 557, 50, 20,
        tostring(WALK.minDist), false, OPT.window)
    guiCreateLabel(393, 559, 200, 16, "м (как BunnyHop — не шагать если близко)", false, OPT.window)

    -- ===== КНОПКИ =====
    local btnApply  = guiCreateButton(10,  585, 110, 30, "Применить",    false, OPT.window)
    local btnFind   = guiCreateButton(130, 585, 110, 30, "Найти маркер", false, OPT.window)
    local btnHelp   = guiCreateButton(250, 585, 110, 30, "Инструкция",   false, OPT.window)
    local btnClose  = guiCreateButton(490, 585, 100, 30, "Закрыть",      false, OPT.window)

    -- ===== ОБРАБОТЧИКИ =====
    addEventHandler("onClientGUIClick", btnToggle, function()
        toggleBot()
        guiSetText(btnToggle, _STATE and "Выключить бота" or "Включить бота")
        guiSetText(lblStatus, "Бот: " .. (_STATE and "ВКЛЮЧЁН" or "выключен"))
    end, false)

    addEventHandler("onClientGUIClick", btnStrict, function()
        STRICT = not STRICT
        guiSetText(btnStrict, STRICT and "Строгий режим: ВКЛ" or "Строгий режим: ВЫКЛ")
    end, false)

    addEventHandler("onClientGUIScroll", sldRot, function()
        local v = math.max(1, math.min(100, math.floor(guiScrollBarGetScrollPosition(sldRot)/10)))
        Settings.RotationSpeed = v
        guiSetText(lblRot, "x"..v)
    end, false)

    addEventHandler("onClientGUIClick", chkBhop, function()
        Settings.AllowBunnyHop = guiCheckBoxGetSelected(chkBhop)
    end, false)

    addEventHandler("onClientGUIClick", btnOpenAP, function()
        openActionPoints()
    end, false)

    addEventHandler("onClientGUIClick", chkAW, function()
        AW.stopOnAdmin = guiCheckBoxGetSelected(chkAW)
    end, false)

    addEventHandler("onClientGUIClick", chkAntiAFK, function()
        Settings.AntiAFK = guiCheckBoxGetSelected(chkAntiAFK)
        if not Settings.AntiAFK then
            api.mouse("right", false)
        end
    end, false)

    addEventHandler("onClientGUIClick", chkHit, function()
        ADMINHIT.enabled = guiCheckBoxGetSelected(chkHit)
        outputChatBox("[HIT] Удар по админу: " ..
            (ADMINHIT.enabled and "ВКЛ" or "ВЫКЛ"), 0,200,255,false)
    end, false)

    -- Обработчики ЖИВОЙ ПОХОДКИ
    addEventHandler("onClientGUIClick", chkWalk, function()
        WALK.enabled = guiCheckBoxGetSelected(chkWalk)
        if WALK.enabled and _STATE then
            startWalk()
            outputChatBox("[WALK] Живая походка: ВКЛ", 0, 200, 255, false)
        else
            stopWalk()
            outputChatBox("[WALK] Живая походка: ВЫКЛ", 0, 200, 255, false)
        end
    end, false)

    addEventHandler("onClientGUIClick", btnApply, function()
        -- Бот
        local bd = tonumber(guiGetText(editBhopD))
        local bm = tonumber(guiGetText(editBhopM))
        if bd then Settings.BunnyHopDelay = bd end
        if bm then Settings.BunnyHopMinDistance = bm end
        -- Безопасность
        local mz = tonumber(guiGetText(editMaxZ))
        local mj = tonumber(guiGetText(editMaxJump))
        local mr = tonumber(guiGetText(editResume))
        if mz then SAFETY.MAX_Z = mz end
        if mj then SAFETY.MAX_JUMP_DIST = mj end
        if mr then SAFETY.RESUME_DELAY = mr * 60000 end
        -- Застревание
        local sd = tonumber(guiGetText(editStuck))
        local st = tonumber(guiGetText(editStuckT))
        local sm = tonumber(guiGetText(editStuckM))
        if sd then STUCK_DIST = sd end
        if st then STUCK_TIME = st end
        if sm then STUCK_MAX  = sm end
        -- Удар
        local hr  = tonumber(guiGetText(editHitR))
        local hcd = tonumber(guiGetText(editHitCD))
        if hr  then ADMINHIT.RADIUS   = hr end
        if hcd then ADMINHIT.COOLDOWN = hcd * 1000 end
        -- Живая походка
        local wi = tonumber(guiGetText(editWalkInt))
        local wd = tonumber(guiGetText(editWalkDur))
        local wm = tonumber(guiGetText(editWalkDist))
        if wi and wi > 100 then
            WALK.interval = wi
            if WALK.enabled and _STATE then
                stopWalk(); startWalk()
            end
        end
        if wd and wd > 0   then WALK.stepDur = wd end
        if wm and wm >= 0  then WALK.minDist = wm end
        refreshApGrid()
        outputChatBox("[JBK] Настройки применены", 0,200,255,false)
    end, false)

    addEventHandler("onClientGUIClick", btnFind, function()
        MARKER_LOG = true
        local mk = Utils.GetJobMarker()
        MARKER_LOG = false
        if mk then
            local mx,my,mz = getElementPosition(mk)
            outputChatBox(string.format("[JBK] Маркер: %.2f,%.2f,%.2f",mx,my,mz),0,255,100,false)
        else
            outputChatBox("[JBK] Маркер не найден",255,100,100,false)
        end
    end, false)

    addEventHandler("onClientGUIClick", btnHelp, function()
        local hw = guiCreateWindow(50,50,500,500,"JBK Bot — Инструкция",false)
        local memo = guiCreateMemo(5,25,490,430,INSTRUCTIONS,false,hw)
        guiMemoSetReadOnly(memo,true)
        local bc = guiCreateButton(190,462,120,30,"Закрыть",false,hw)
        addEventHandler("onClientGUIClick",bc,function() destroyElement(hw) end,false)
    end, false)

    addEventHandler("onClientGUIClick", btnClose, function()
        guiSetVisible(OPT.window, false)
        showCursor(false)
        OPT.visible = false
    end, false)

    guiSetVisible(OPT.window, true)
    showCursor(true)
    OPT.visible = true
end

local nativeState = {}

local function nativeUpdate(key, value)
    value = tostring(value)
    if nativeState[key] == value then return end
    nativeState[key] = value
    api.updateState(key, value)
end

local function nativeBool(value)
    return value and "1" or "0"
end

syncNativeJbkState = function()
    api.alertMonitor(Settings.PlaySiren and _STATE)
    local status = "выключен"
    if _STATE then
        status = "работает"
    elseif AW.adminNearby then
        status = "остановлен: админ рядом"
    elseif SAFETY.paused then
        status = "остановлен защитой"
    end
    nativeUpdate("loaded", "1")
    nativeUpdate("bot", nativeBool(_STATE))
    nativeUpdate("bhop", nativeBool(Settings.AllowBunnyHop))
    nativeUpdate("anti_afk", nativeBool(Settings.AntiAFK))
    nativeUpdate("auto_disable", nativeBool(Settings.AutoDisable))
    nativeUpdate("siren", nativeBool(Settings.PlaySiren))
    nativeUpdate("stop_admin", nativeBool(AW.stopOnAdmin))
    nativeUpdate("strict", nativeBool(STRICT))
    nativeUpdate("admin_hit", nativeBool(ADMINHIT.enabled))
    nativeUpdate("walk", nativeBool(WALK.enabled))
    nativeUpdate("rotation_speed", Settings.RotationSpeed)
    nativeUpdate("bhop_delay", Settings.BunnyHopDelay)
    nativeUpdate("bhop_distance", Settings.BunnyHopMinDistance)
    nativeUpdate("max_z", SAFETY.MAX_Z)
    nativeUpdate("max_jump", SAFETY.MAX_JUMP_DIST)
    nativeUpdate("resume_minutes", SAFETY.RESUME_DELAY / 60000)
    nativeUpdate("stuck_distance", STUCK_DIST)
    nativeUpdate("stuck_time", STUCK_TIME)
    nativeUpdate("stuck_max", STUCK_MAX)
    nativeUpdate("hit_radius", ADMINHIT.RADIUS)
    nativeUpdate("hit_cooldown", ADMINHIT.COOLDOWN / 1000)
    nativeUpdate("walk_interval", WALK.interval)
    nativeUpdate("walk_duration", WALK.stepDur)
    nativeUpdate("walk_distance", WALK.minDist)
    nativeUpdate("status", status)
end

local function commandBool(value)
    return value == "1" or value == "true"
end

local function commandNumber(value, minimum, maximum)
    local number = tonumber(value)
    if not number then return nil end
    return math.max(minimum, math.min(maximum, number))
end

local function applyNativeCommand(command)
    local key, value = command:match("^([^:]+):?(.*)$")
    if not key then return end
    if key == "bot" then
        cancelAutoResume()
        local enabled = commandBool(value)
        if _STATE ~= enabled then
            _STATE = enabled
            changeBotState(_STATE)
            outputChatBox("#0037FF[JBK] #FFFFFFЖБК Бот " ..
                (_STATE and "#00FF00включен." or "#F00000выключен."), 255,255,255,true)
        end
    elseif key == "bhop" then
        Settings.AllowBunnyHop = commandBool(value)
        if not Settings.AllowBunnyHop then
            if bunnyHopTimer then killTimer(bunnyHopTimer); bunnyHopTimer = nil end
            setBotControl("jump", false)
        end
    elseif key == "anti_afk" then
        Settings.AntiAFK = commandBool(value)
        if not Settings.AntiAFK then api.mouse("right", false) end
    elseif key == "auto_disable" then
        Settings.AutoDisable = commandBool(value)
    elseif key == "siren" then
        Settings.PlaySiren = commandBool(value)
        api.alertMonitor(Settings.PlaySiren and _STATE)
    elseif key == "stop_admin" then
        AW.stopOnAdmin = commandBool(value)
    elseif key == "strict" then
        STRICT = commandBool(value)
    elseif key == "admin_hit" then
        ADMINHIT.enabled = commandBool(value)
        if not ADMINHIT.enabled then
            setBotControl("fire", false)
            if ADMINHIT.walking then setBotControl("forwards", false) end
            ADMINHIT.walking = false
        end
    elseif key == "walk" then
        WALK.enabled = commandBool(value)
        if WALK.enabled and _STATE then startWalk() else stopWalk() end
    elseif key == "rotation_speed" then
        Settings.RotationSpeed = commandNumber(value, 1, 10) or Settings.RotationSpeed
    elseif key == "bhop_delay" then
        Settings.BunnyHopDelay = commandNumber(value, 100, 10000) or Settings.BunnyHopDelay
        if bunnyHopTimer then killTimer(bunnyHopTimer); bunnyHopTimer = nil end
    elseif key == "bhop_distance" then
        Settings.BunnyHopMinDistance = commandNumber(value, 0, 100) or Settings.BunnyHopMinDistance
    elseif key == "max_z" then
        SAFETY.MAX_Z = commandNumber(value, 0, 10000) or SAFETY.MAX_Z
    elseif key == "max_jump" then
        SAFETY.MAX_JUMP_DIST = commandNumber(value, 1, 500) or SAFETY.MAX_JUMP_DIST
    elseif key == "resume_minutes" then
        local minutes = commandNumber(value, 1, 60)
        if minutes then SAFETY.RESUME_DELAY = minutes * 60000 end
    elseif key == "stuck_distance" then
        STUCK_DIST = commandNumber(value, 0.1, 20) or STUCK_DIST
    elseif key == "stuck_time" then
        STUCK_TIME = commandNumber(value, 250, 30000) or STUCK_TIME
    elseif key == "stuck_max" then
        STUCK_MAX = commandNumber(value, 1, 20) or STUCK_MAX
    elseif key == "hit_radius" then
        ADMINHIT.RADIUS = commandNumber(value, 1, 100) or ADMINHIT.RADIUS
    elseif key == "hit_cooldown" then
        local seconds = commandNumber(value, 1, 600)
        if seconds then ADMINHIT.COOLDOWN = seconds * 1000 end
    elseif key == "walk_interval" then
        WALK.interval = commandNumber(value, 100, 10000) or WALK.interval
        if WALK.enabled and _STATE then stopWalk(); startWalk() end
    elseif key == "walk_duration" then
        WALK.stepDur = commandNumber(value, 50, 5000) or WALK.stepDur
    elseif key == "walk_distance" then
        WALK.minDist = commandNumber(value, 0, 100) or WALK.minDist
    end
    syncNativeJbkState()
end

setTimer(function()
    for _ = 1, 32 do
        local command = api.takeCommand()
        if not command then break end
        applyNativeCommand(command)
    end
end, 50, 0)

setTimer(syncNativeJbkState, 500, 0)
syncNativeJbkState()
outputChatBox("#0037FF[DarkFlame] #FFFFFFЖБК Бот загружен: Shift+R > JBK Bot.", 255,255,255,true)

if type(onUnload) == "function" then
    onUnload(function()
        _STATE = false
        pcall(changeBotState, false)
        releaseBotControls()
        api.mouse("right", false)
        api.alertMonitor(false)
        api.updateState("loaded", "0")
    end)
end

api.alertMonitor(Settings.PlaySiren and _STATE)

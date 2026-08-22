#pragma once

namespace LuaBridgePayload
{
inline constexpr char Bootstrap[] = R"DFLUA(
local O,G=false,0
local TS,TL=triggerServerEvent,triggerLatentServerEvent
local R=getThisResource()
local Global=getfenv(0)
local PCall,Raise,Kind=pcall,error,type
local RawSet,SetMeta=rawset,setmetatable
local Scope=dfTrapScope

local function restore(g)
    if g~=G then return end
    hideFunctionCall(false)
end

local function injectError(kind,value)
    dfEmit('inject',value,0)
    outputChatBox('[DarkFlame] '..kind..': '..tostring(value),255,64,64)
end

local function emitEvent(sourceResource,functionName,allowed,file,line,...)
    local args={...}
    local resourceName=sourceResource and getResourceName(sourceResource)
    if resourceName=='province_afktimer'
        or tostring(args[1])=='inventory:getPlayersObjectInfo'
        or tostring(args[1])=='radio:onPlayerSyncBoomBox'
        or tostring(args[1])=='player:requestStreamData' then
        return
    end
    local ok,formatted=pcall(inspect,args)
    if not ok then formatted='<inspect error: '..tostring(formatted)..'>' end
    local row='['..tostring(resourceName)..' | '..tostring(file)..':'
        ..tostring(line)..'] '..tostring(functionName)..'(args: '
        ..tostring(#args)..'): '..tostring(formatted)
    dfEmit('event',row)
end

local function hiddenCall(name,original,...)
    if dfHideActive() then
        local info=debug and debug.getinfo and debug.getinfo(3,'Sl')
        emitEvent(R,name,true,info and info.short_src or '[injected]',
            info and info.currentline or 0,...)
    end
    return original(...)
end

local function wrappedServerEvent(...)
    return hiddenCall('triggerServerEvent',TS,...)
end

local function wrappedLatentEvent(...)
    return hiddenCall('triggerLatentServerEvent',TL,...)
end

local function finishScope(ok,...)
    Scope(false)
    if not ok then Raise((...),0) end
    return ...
end

local function scopedCall(original,...)
    Scope(true)
    return finishScope(PCall(original,...))
end

local function protect(original)
    return function(...)
        return scopedCall(original,...)
    end
end

local function injectedEnvironment()
    local env={}
    RawSet(env,'_G',env)
    RawSet(env,'triggerServerEvent',protect(wrappedServerEvent))
    RawSet(env,'triggerLatentServerEvent',protect(wrappedLatentEvent))
    SetMeta(env,{
        __index=function(_,key)
            local value=Global[key]
            if Kind(value)~='function' then return value end
            local wrapped=protect(value)
            RawSet(env,key,wrapped)
            return wrapped
        end,
        __newindex=function(_,key,value)
            RawSet(Global,key,value)
        end,
    })
    return env
end

local function update()
    local visible=dfMenuOpen()
    if visible~=O then
        O=visible
        toggleAllControls(not visible)
        guiSetInputMode(visible and 'no_binds' or 'allow_binds')
        showCursor(visible)
        if visible then setCursorAlpha(255) end
    end

    local code,target=dfTake()
    if not code then return end
    G=G+1
    hideFunctionCall(true)
    local ok,result=dfInject(target,code)
    if not ok then injectError('Inject error',result)
    else outputChatBox('[DarkFlame] Lua thread '..tostring(result)..' started.',96,255,128) end
    setTimer(restore,2000,1,G)
end

setTimer(update,100,0)
dfEmit('monitor','outgoing Lua packet monitor active',0)
)DFLUA";
}

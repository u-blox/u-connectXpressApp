-- u-connectXpress AT decoding for the Wireshark captures written by u-connectXpressApp
-- (at.trace <file>.pcapng, or at.trace.pcap <file>.trace).
--
-- Wireshark already shows these captures without this script (its AT dissector, the URC meaning and the command
-- time in the Info column). This script adds:
--   ucx.kind      command, response, final, urc or echo
--   ucx.cmd       command or URC name without AT+ or +, e.g. USPSWB, UEWLU
--   ucx.param     each parameter (ucx.params is the whole list)
--   ucx.result    OK or ERROR on the final line
--   ucx.time      milliseconds from the command to its OK/ERROR
--   ucx.request_in / ucx.response_in  links between a command and its final line
--   ucx.meaning   what a URC means
-- and expert info for failed commands, commands without OK/ERROR before the next command or a restart, and restarts.
--
-- Filters: ucx.cmd == "USPSWB", ucx.time > 100, ucx.result == "ERROR", ucx.kind == "urc", ucx.no_reply
--
-- Install: copy this file to the personal Lua plugins folder (Help > About Wireshark > Folders), or run once with
--   wireshark -X lua_script:ucxapp_at.lua trace.pcapng
--   tshark -X lua_script:ucxapp_at.lua -2 -r trace.pcapng -Y "ucx.time > 100"

local ucx = Proto("ucx", "u-connectXpress AT")

local pf = {
    kind = ProtoField.string("ucx.kind", "Kind"),
    cmd = ProtoField.string("ucx.cmd", "Command"),
    params = ProtoField.string("ucx.params", "Parameters"),
    param = ProtoField.string("ucx.param", "Parameter"),
    result = ProtoField.string("ucx.result", "Result"),
    time = ProtoField.uint32("ucx.time", "Response time (ms)"),
    request = ProtoField.framenum("ucx.request_in", "Command in", base.NONE, frametype.REQUEST),
    response = ProtoField.framenum("ucx.response_in", "Final response in", base.NONE, frametype.RESPONSE),
    meaning = ProtoField.string("ucx.meaning", "Meaning"),
}
ucx.fields = { pf.kind, pf.cmd, pf.params, pf.param, pf.result, pf.time, pf.request, pf.response, pf.meaning }

local ex = {
    error = ProtoExpert.new("ucx.error", "Command failed", expert.group.RESPONSE_CODE, expert.severity.WARN),
    no_reply = ProtoExpert.new("ucx.no_reply", "No OK/ERROR before the next command or a module restart",
                               expert.group.SEQUENCE, expert.severity.ERROR),
    startup = ProtoExpert.new("ucx.startup", "Module started", expert.group.SEQUENCE, expert.severity.CHAT),
}
ucx.experts = { ex.error, ex.no_reply, ex.startup }

local pending   -- the command waiting for OK/ERROR during the first pass
local frames = {}  -- per frame number: what the first pass found (rewritten whenever a frame is not yet visited)

function ucx.init()
    pending = nil
end

-- The exported-PDU header written by u-connectXpressApp: big-endian tag, length, value; tag 12 "at" first, 35 the
-- direction (0 sent), 36 the Info text, 0 the end. Read straight from the bytes: tshark's first pass builds no tree,
-- so field extractors would be empty there. Returns nil for any other frame (e.g. merged sniffer captures).
local function header(tvb)
    if tvb:len() < 12 or tvb(0, 2):uint() ~= 12 or tvb(2, 2):uint() ~= 4 or tvb(4, 4):string() ~= "at" then
        return nil
    end
    local h = { off = 8 }
    while h.off + 4 <= tvb:len() do
        local tag, len = tvb(h.off, 2):uint(), tvb(h.off + 2, 2):uint()
        h.off = h.off + 4
        if tag == 0 then
            return h.off < tvb:len() and h or nil
        end
        if h.off + len > tvb:len() then
            return nil
        end
        if tag == 35 and len == 4 then
            h.tx = tvb(h.off, 4):int() == 0
        elseif tag == 36 and len > 0 then
            h.info = tvb(h.off, len):string()
        end
        h.off = h.off + len
    end
    return nil
end

-- "0,\"a,b\",6" -> { "0", "\"a,b\"", "6" }
local function split(params)
    local out, cur, quote = {}, {}, false
    for c in params:gmatch(".") do
        if c == '"' then
            quote = not quote
        end
        if c == "," and not quote then
            out[#out + 1] = table.concat(cur)
            cur = {}
        else
            cur[#cur + 1] = c
        end
    end
    out[#out + 1] = table.concat(cur)
    return out
end

-- kind, name and parameters of one line; pending is the command still waiting for OK/ERROR.
local function classify(text, tx, waiting)
    if tx then
        local name, rest = text:match("^AT%+?([%u%d&]*)(.*)$")
        return "command", name or "", rest and rest:match("^[=?]*(.*)$") or ""
    end
    if text == "OK" or text:match("^ERROR") or text:match("^%+CME ERROR") then
        return "final", "", text:match(":%s*(.*)$") or ""
    end
    if text:match("^AT") then
        return "echo", text:match("^AT%+?([%u%d&]*)") or "", ""
    end
    local name = text:match("^%+([%u%d]+)")
    if name ~= nil then
        local params = text:match("^%+[%u%d]+:?(.*)$") or ""
        if (name:match("^UE") or name == "STARTUP") and not (waiting and waiting.name == name) then
            return "urc", name, params
        end
        return "response", name, params
    end
    return "response", "", text
end

function ucx.dissector(tvb, pinfo, tree)
    local h = header(tvb)
    if h == nil then
        return
    end
    local range = tvb(h.off)
    local text = range:string():gsub("[\r\n]+$", "")
    local tx = h.tx == true
    local num = pinfo.number

    if not pinfo.visited then
        local kind, name = classify(text, tx, pending)
        local fr = { kind = kind, name = name }
        if kind == "command" then
            if pending ~= nil then
                frames[pending.num].no_reply = true
            end
            pending = { num = num, name = name, ts = pinfo.abs_ts }
        elseif kind == "urc" and name == "STARTUP" and pending ~= nil then
            frames[pending.num].no_reply = true
            pending = nil
        elseif pending ~= nil and (kind == "response" or kind == "final") then
            fr.request = pending.num
            if kind == "final" then
                fr.time = math.floor((pinfo.abs_ts - pending.ts) * 1000 + 0.5)
                frames[pending.num].response = num
                pending = nil
            end
        end
        frames[num] = fr
    end

    local fr = frames[num] or {}
    local kind, name, params = classify(text, tx, fr.kind == "response" and { name = fr.name } or nil)
    local sub = tree:add(ucx, range, "u-connectXpress " .. kind .. (name ~= "" and (": " .. name) or ""))
    sub:add(pf.kind, kind):set_generated()
    if name ~= "" then
        sub:add(pf.cmd, name):set_generated()
    end
    if params ~= "" and kind ~= "final" then
        local list = sub:add(pf.params, params)
        list:set_generated()
        for _, p in ipairs(split(params)) do
            list:add(pf.param, p):set_generated()
        end
    end
    if kind == "final" then
        local result = text == "OK" and "OK" or "ERROR"
        sub:add(pf.result, result):set_generated()
        if text == "ERROR" then
            sub:add_proto_expert_info(ex.error)
        elseif result ~= "OK" then
            sub:add_proto_expert_info(ex.error, "Command failed: " .. text)
        end
    end
    if fr.time ~= nil then
        sub:add(pf.time, fr.time):set_generated()
    end
    if fr.request ~= nil then
        sub:add(pf.request, fr.request):set_generated()
    end
    if fr.response ~= nil then
        sub:add(pf.response, fr.response):set_generated()
    end
    if fr.no_reply then
        sub:add_proto_expert_info(ex.no_reply)
    end
    if kind == "urc" then
        if h.info ~= nil then
            sub:add(pf.meaning, (h.info:gsub(":.*$", ""))):set_generated()
        end
        if name == "STARTUP" then
            sub:add_proto_expert_info(ex.startup)
        end
    end
end

register_postdissector(ucx)

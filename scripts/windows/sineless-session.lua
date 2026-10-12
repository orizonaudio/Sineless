-- Drives REAPER through a Sineless session without clicks. Run by Run-Reaper.ps1, which starts
-- REAPER with this script on its command line. Writes everything to <resource path>/sineless-out/:
--   session.log   what happened, step by step
--   measure.txt   REAPER's memory and handles, and Sineless's WebView2 processes
--   chunks.txt    each Sineless's state as saved
--   *.png         screenshots
--   done.txt      written last
--
-- Phase "session" (the default): a 48 kHz project with 4 tracks of tone into Sineless; plays;
-- opens the 4 editors as floating windows and screenshots them; opens and closes track 1's editor
-- SINELESS_CYCLES times (default 50) while playing, measuring before, halfway and after; saves the
-- project and its chunks, then quits.
-- Phase "verify" (SINELESS_PHASE=verify, project given on the command line): compares each
-- Sineless's chunk with chunks.txt, writes verify.txt, then quits.
-- Phase "shots" (SINELESS_PHASE=shots): only the 4 tracks and a screenshot of track 1's editor at its
-- default size, then at its minimum size, for the display-scale checks.

local scriptDir = debug.getinfo(1, "S").source:match("^@(.*[\\/])")
local out = reaper.GetResourcePath() .. "/sineless-out/"
reaper.RecursiveCreateDirectory(out, 0)
local phase = os.getenv("SINELESS_PHASE") or "session"
local cycles = tonumber(os.getenv("SINELESS_CYCLES") or "50")
local prefix = os.getenv("SINELESS_PREFIX") or ""
local log = io.open(out .. phase .. ".log", "w")

local function say(...)
  local parts = {}
  for i, value in ipairs({...}) do parts[i] = tostring(value) end
  log:write(os.date("%H:%M:%S "), table.concat(parts, " "), "\n")
  log:flush()
end

-- Hidden: a console window would come to the front and take the keys meant for an editor.
local function powershellCommand(script, arguments)
  return 'powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "' ..
    scriptDir .. script .. '" ' .. arguments
end

local function powershell(script, arguments)
  local result = reaper.ExecProcess(powershellCommand(script, arguments), 60000) or "failed to start"
  return (result:gsub("\r", ""):gsub("\n$", ""))
end

-- Not waited for, so that REAPER keeps handling its windows (a drag, a click) meanwhile.
local function powershellInBackground(script, arguments)
  reaper.ExecProcess(powershellCommand(script, arguments), -2)
end

local function measure(label, withModules)
  local result = powershell("Measure-Reaper.ps1",
    '-Out "' .. out .. 'measure.txt" -Label ' .. label .. (withModules and " -Modules" or ""))
  say("measure", result)
end

local function capture(name, titlePattern)
  local result = powershell("Capture-Window.ps1",
    '-Out "' .. out .. prefix .. name .. '.png" -Title "' .. titlePattern .. '"')
  say("capture", name, result)
end

-- Coroutine steps run from defer, so that REAPER keeps running while the script waits.
local function wait(seconds)
  local untilTime = reaper.time_precise() + seconds
  while reaper.time_precise() < untilTime do coroutine.yield() end
end

local function findFx(pattern)
  local i = 0
  while true do
    local ok, name = reaper.EnumInstalledFX(i)
    if not ok then return nil end
    if name:find(pattern) then return name end
    i = i + 1
  end
end

local function sinelessOn(track)
  for fx = 0, reaper.TrackFX_GetCount(track) - 1 do
    local _, name = reaper.TrackFX_GetFXName(track, fx)
    if name:find("Sineless") then return fx end
  end
  return nil
end

-- The Sineless block of the track's state, as the project stores it.
local function sinelessChunk(track)
  local _, chunk = reaper.GetTrackStateChunk(track, "", false)
  local block = chunk:match("(<VST3?[^\n]*Sineless.->)\n")
  return block or ""
end

local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

local function fromBase64(text)
  text = text:gsub("[^%w%+/=]", "")
  return (text:gsub(".", function(c)
    if c == "=" then return "" end
    local value, bits = alphabet:find(c, 1, true) - 1, ""
    for i = 6, 1, -1 do bits = bits .. (value % 2 ^ i - value % 2 ^ (i - 1) > 0 and "1" or "0") end
    return bits
  end):gsub("%d%d%d?%d?%d?%d?%d?%d?", function(bits)
    if #bits ~= 8 then return "" end
    return string.char(tonumber(bits, 2))
  end))
end

local function toBase64(data)
  return ((data:gsub(".", function(c)
    local bits, byte = "", c:byte()
    for i = 8, 1, -1 do bits = bits .. (byte % 2 ^ i - byte % 2 ^ (i - 1) > 0 and "1" or "0") end
    return bits
  end) .. "0000"):gsub("%d%d%d?%d?%d?%d?", function(bits)
    if #bits < 6 then return "" end
    local value = 0
    for i = 1, 6 do value = value + (bits:sub(i, i) == "1" and 2 ^ (6 - i) or 0) end
    return alphabet:sub(value + 1, value + 1)
  end) .. ({"", "==", "="})[#data % 3 + 1])
end

-- Gives a Sineless its own settings: edits of the same length in the JSON of its state, so that
-- the sizes in REAPER's and the plugin's headers stay right.
local function setSinelessState(track, fx, group, timeSpan)
  local ok, chunk = reaper.TrackFX_GetNamedConfigParm(track, fx, "vst_chunk")
  if not ok then return false end
  local data = fromBase64(chunk)
  local edited = data:gsub('"group": "%a"', '"group": "' .. group .. '"', 1)
    :gsub('"timeSpan": %d%.%d', '"timeSpan": ' .. string.format("%.1f", timeSpan), 1)
  if #edited ~= #data then return false end
  return reaper.TrackFX_SetNamedConfigParm(track, fx, "vst_chunk", toBase64(edited))
end

local function sinelessSettings(track)
  local ok, chunk = reaper.TrackFX_GetNamedConfigParm(track, sinelessOn(track), "vst_chunk")
  if not ok then return "(no chunk)" end
  local data = fromBase64(chunk)
  return (data:match('"group": "%a"') or "?") .. ", " .. (data:match('"timeSpan": [%d%.]+') or "?")
end

local function floatTitle(trackNumber)
  -- REAPER titles a floating FX window "VST3: Sineless (Orizon Audio) - Track N"; in a dedicated
  -- process, "Sineless (x64 bridged)" for every track (the front one is the one just opened).
  return "Sineless.*(Track " .. trackNumber .. "\\b|bridged\\))"
end

local function buildProject()
  reaper.Main_OnCommand(40023, 0) -- File: New project.
  reaper.GetSetProjectInfo(0, "PROJECT_SRATE", 48000, true)
  reaper.GetSetProjectInfo(0, "PROJECT_SRATE_USE", 1, true)
  local tone = findFx("[Tt]one [Gg]enerator")
  local sineless = findFx("Sineless")
  say("tone fx:", tone, "| sineless fx:", sineless)
  local frequencies = {110, 220, 440, 880}
  for i = 1, 4 do
    reaper.InsertTrackAtIndex(i - 1, true)
    local track = reaper.GetTrack(0, i - 1)
    reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "Tone " .. frequencies[i], true)
    local toneFx = reaper.TrackFX_AddByName(track, tone, false, -1)
    for p = 0, reaper.TrackFX_GetNumParams(track, toneFx) - 1 do
      local _, paramName = reaper.TrackFX_GetParamName(track, toneFx, p)
      if paramName:lower():find("freq") then
        reaper.TrackFX_SetParam(track, toneFx, p, frequencies[i])
        say("track", i, "tone", paramName, "=", frequencies[i])
      end
    end
    local scopeFx = reaper.TrackFX_AddByName(track, sineless, false, -1)
    say("track", i, "fx", toneFx, scopeFx, "params", reaper.TrackFX_GetNumParams(track, scopeFx))
  end
  -- Distinct Sineless states: groups A to D, and seconds spans (used in SEC mode).
  local groups, spans = {"a", "b", "c", "d"}, {1, 2, 4, 0.5}
  for i = 1, 4 do
    local track = reaper.GetTrack(0, i - 1)
    local set = setSinelessState(track, sinelessOn(track), groups[i], spans[i])
    say("track", i, "state set", set, "->", sinelessSettings(track))
  end
  say("project rate", reaper.GetSetProjectInfo(0, "PROJECT_SRATE", 0, false),
    "device rate", select(2, reaper.GetAudioDeviceInfo("SRATE")))
end

local function session()
  measure("start", false)
  buildProject()
  capture("p1-01-reaper", "REAPER v")
  reaper.OnPlayButton()
  wait(2)
  say("play state", reaper.GetPlayState(), "position", reaper.GetPlayPosition())

  -- Each editor is captured as it opens, while it is the front window: REAPER cannot bring
  -- another forward while it runs the capture.
  for i = 1, 4 do
    local track = reaper.GetTrack(0, i - 1)
    reaper.TrackFX_Show(track, sinelessOn(track), 3)
    wait(6) -- WebView2 starts and the page loads.
    capture(string.format("p1-%02d-editor-track%d", i + 1, i), floatTitle(i))
  end
  measure("editors-open", true)
  say("device rate", select(2, reaper.GetAudioDeviceInfo("SRATE")))
  for i = 1, 4 do
    local track = reaper.GetTrack(0, i - 1)
    reaper.TrackFX_Show(track, sinelessOn(track), 2)
  end
  wait(8)
  measure("before-cycles", false)

  local track = reaper.GetTrack(0, 0)
  local fx = sinelessOn(track)
  for cycle = 1, cycles do
    reaper.TrackFX_Show(track, fx, 3)
    wait(2)
    reaper.TrackFX_Show(track, fx, 2)
    wait(1)
    if cycle == cycles // 2 then
      wait(5)
      measure("after-" .. cycle, false)
    end
  end
  wait(8)
  measure("after-" .. cycles, false)
  say("play state", reaper.GetPlayState(), "position", reaper.GetPlayPosition())

  reaper.OnStopButton()
  local chunks = io.open(out .. "chunks.txt", "w")
  for i = 1, 4 do
    local track = reaper.GetTrack(0, i - 1)
    chunks:write("== track ", i, "\n", sinelessChunk(track), "\n")
    say("track", i, "before saving:", sinelessSettings(track))
  end
  chunks:close()
  local project = out .. "session.rpp"
  reaper.Main_SaveProjectEx(0, project, 0)
  -- Saving a copy leaves the project untitled and unsaved; reopened, it is clean.
  reaper.Main_openProject("noprompt:" .. project)
  say("saved", project)
end

local function verify()
  wait(3)
  local saved = io.open(out .. "chunks.txt"):read("a")
  local report = io.open(out .. "verify.txt", "w")
  local same = 0
  for i = 1, 4 do
    local track = reaper.GetTrack(0, i - 1)
    local expected = saved:match("== track " .. i .. "\n(.-)\n== track") or
      saved:match("== track " .. i .. "\n(.-)\n$")
    local actual = track and sinelessChunk(track) or "(no track)"
    local ok = expected ~= nil and expected == actual
    if ok then same = same + 1 end
    report:write("track ", i, ok and " same" or " DIFFERENT", " (",
      track and sinelessSettings(track) or "-", ")\n")
    if not ok then report:write("expected:\n", tostring(expected), "\nactual:\n", actual, "\n") end
  end
  report:write(same, " of 4 chunks unchanged\n")
  report:close()
  say("verify", same, "of 4 unchanged")
end

local function shots()
  buildProject()
  reaper.OnPlayButton()
  local track = reaper.GetTrack(0, 0)
  reaper.TrackFX_Show(track, sinelessOn(track), 3)
  wait(8)
  -- Into the work area, clear of the taskbar, on a short screen: a resize by nothing moves it.
  powershellInBackground("Resize-Window.ps1", '-Title "' .. floatTitle(1) .. '" -ShrinkHeight 0')
  wait(10)
  capture("editor-default", floatTitle(1))
  powershellInBackground("Resize-Window.ps1", '-Title "' .. floatTitle(1) .. '" -ShrinkHeight 250')
  wait(10)
  capture("editor-minimum", floatTitle(1))
  reaper.TrackFX_Show(track, sinelessOn(track), 2)
  reaper.OnStopButton()
end

-- Space in an editor whose page has focus, outside any text field, should reach REAPER and
-- toggle its transport. The page is focused by a click on Sineless's logo.
local function keys()
  buildProject()
  reaper.OnPlayButton()
  local track = reaper.GetTrack(0, 0)
  reaper.TrackFX_Show(track, sinelessOn(track), 3)
  wait(8)
  local report = io.open(out .. "keys.txt", "w")
  local sent = 0
  local function linesIn(path)
    local file, count = io.open(path), 0
    if file then
      for _ in file:lines() do count = count + 1 end
      file:close()
    end
    return count
  end
  local function press(click)
    local before = reaper.GetPlayState()
    powershellInBackground("Send-Input.ps1", '-Title "' .. floatTitle(1) .. '"' ..
      (click and " -ClickX 45 -ClickY 70" or "") .. ' -Keys " " -Log "' .. out .. 'input.log"')
    -- Every change of the play state, to tell a key lost from a key handled twice, from the
    -- start until 2 s after Send-Input has logged the key (PowerShell takes a while to start).
    sent = sent + 1
    local changes, last, start, deadline = {}, before, reaper.time_precise(), nil
    while not deadline or reaper.time_precise() < deadline do
      if not deadline and (linesIn(out .. "input.log") >= sent or
                           reaper.time_precise() - start > 20) then
        deadline = reaper.time_precise() + 2
      end
      local now = reaper.GetPlayState()
      if now ~= last then
        changes[#changes + 1] = string.format("%d at %.2f s", now, reaper.time_precise() - start)
        last = now
      end
      coroutine.yield()
    end
    local after = reaper.GetPlayState()
    local line = string.format("space%s: play state %d -> %d (changes: %s)",
      click and " after a click on the logo" or "", before, after,
      #changes > 0 and table.concat(changes, ", ") or "none")
    report:write(line, "\n")
    say(line)
  end
  press(true)  -- Expected: playing (1) -> stopped (0).
  press(false) -- Expected: stopped (0) -> playing (1).
  report:close()
  reaper.TrackFX_Show(track, sinelessOn(track), 2)
  reaper.OnStopButton()
end

local function finish()
  say("done")
  log:close()
  io.open(out .. "done.txt", "w"):close()
  -- Quit without the save prompt: a project not saved yet goes to a scratch file.
  if reaper.IsProjectDirty(0) ~= 0 then
    local scratch = out .. "scratch-" .. phase .. ".rpp"
    reaper.Main_SaveProjectEx(0, scratch, 0)
    reaper.Main_openProject("noprompt:" .. scratch)
  end
  reaper.Main_OnCommand(40004, 0) -- File: Quit REAPER.
end

local steps = coroutine.create(function()
  local ok, failure = pcall(function()
    if phase == "verify" then verify()
    elseif phase == "shots" then shots()
    elseif phase == "keys" then keys()
    else session() end
  end)
  if not ok then say("FAILED", failure) end
  finish()
end)

local function resume()
  local ok, failure = coroutine.resume(steps)
  if not ok then say("coroutine failed", failure) end
  if coroutine.status(steps) ~= "dead" then reaper.defer(resume) end
end
reaper.defer(resume)

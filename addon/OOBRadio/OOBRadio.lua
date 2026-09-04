-- Out of Bounds Radio -- in-game radio for WoW 3.3.5a
--
-- Audio is streamed by the WowRadio client extension (DivxDecoder.dll), which
-- exposes Radio_Play / Radio_Stop / Radio_SetVolume / Radio_GetStatus to Lua.
-- This addon is the player-facing part: a minimap button that opens a small
-- panel with the on/off switch, volume and the current track.
--
-- While the radio is on, the game's own music is kept off (zone changes keep
-- trying to restart it) and restored when the radio is turned off.

local ADDON_TITLE = "Out of Bounds Radio"
local CHAT_PREFIX = "|cffffa040[OOB Radio]|r "

local STATIONS = {
    { name = "Out of Bounds Radio", desc = "Your gaming radio station",
      url = "http://radio.outofbounds.live:8000/radio.mp3" },
}

local DEFAULTS = {
    enabled    = false,   -- radio was on at logout; resume on login
    volume     = 60,      -- 0..100, the radio's own volume
    station    = 1,
    minimapPos = 220,     -- degrees around the minimap
    hideButton = false,
}

OOBRadioDB = OOBRadioDB or {}
local db

-- ---- helpers ----------------------------------------------------------------
local function Say(msg)
    DEFAULT_CHAT_FRAME:AddMessage(CHAT_PREFIX .. msg)
end

local function HasDLL()
    return type(Radio_GetStatus) == "function"
end

local function Status()
    if not HasDLL() then return "missing", "", "", 0, "" end
    return Radio_GetStatus()
end

local function IsOn()
    local state = Status()
    return state == "playing" or state == "connecting"
end

-- Volume actually sent to the DLL: the panel slider scaled by the game's master
-- volume, muted when all sound is disabled.
local function EffectiveVolume()
    if GetCVar("Sound_EnableAllSound") ~= "1" then return 0 end
    local master = tonumber(GetCVar("Sound_MasterVolume")) or 1
    return (db.volume / 100) * master
end

local function SuppressGameMusic()
    if GetCVar("Sound_EnableMusic") ~= "0" then SetCVar("Sound_EnableMusic", "0") end
    StopMusic()
end

local function RestoreGameMusic()
    if db.savedMusic ~= nil then
        SetCVar("Sound_EnableMusic", db.savedMusic)
        db.savedMusic = nil
    end
end

-- ---- radio control ------------------------------------------------------------
local Refresh   -- forward: repaints the UI from the DLL state

local function TurnOn(silent)
    if not HasDLL() then
        Say("The radio extension is not loaded. Make sure DivxDecoder.dll and bass.dll are in the game folder.")
        return
    end
    local st = STATIONS[db.station] or STATIONS[1]
    if db.savedMusic == nil then db.savedMusic = GetCVar("Sound_EnableMusic") end
    SuppressGameMusic()
    Radio_SetVolume(EffectiveVolume())
    Radio_SetBackgroundMute(GetCVar("Sound_EnableSoundWhenGameIsInBG") ~= "1")
    Radio_Play(st.url)
    db.enabled = true
    if not silent then Say("Tuning in to " .. st.name .. "...") end
    Refresh()
end

local function TurnOff(silent)
    if HasDLL() then Radio_Stop() end
    RestoreGameMusic()
    db.enabled = false
    if not silent then Say("Radio off.") end
    Refresh()
end

local function Toggle()
    if IsOn() then TurnOff() else TurnOn() end
end

-- ---- minimap button -------------------------------------------------------------
local button = CreateFrame("Button", "OOBRadioMinimapButton", Minimap)
button:SetWidth(32); button:SetHeight(32)
button:SetFrameStrata("MEDIUM")
button:SetFrameLevel(8)
button:RegisterForClicks("LeftButtonUp", "RightButtonUp")
button:RegisterForDrag("LeftButton")
button:SetHighlightTexture("Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight")

local icon = button:CreateTexture(nil, "BACKGROUND")
icon:SetTexture("Interface\\AddOns\\OOBRadio\\icon")
icon:SetWidth(21); icon:SetHeight(21)
icon:SetPoint("CENTER", button, "CENTER", 0, 1)

local ring = button:CreateTexture(nil, "OVERLAY")
ring:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
ring:SetWidth(54); ring:SetHeight(54)
ring:SetPoint("TOPLEFT", button, "TOPLEFT", 0, 0)

-- small "on air" light in the corner
local led = button:CreateTexture(nil, "OVERLAY")
led:SetTexture("Interface\\COMMON\\Indicator-Green")
led:SetWidth(10); led:SetHeight(10)
led:SetPoint("BOTTOMRIGHT", button, "BOTTOMRIGHT", -4, 4)
led:Hide()

local function PlaceButton()
    local angle = math.rad(db.minimapPos or 220)
    button:ClearAllPoints()
    button:SetPoint("CENTER", Minimap, "CENTER", math.cos(angle) * 80, math.sin(angle) * 80)
end

local function DragUpdate()
    local mx, my = Minimap:GetCenter()
    local px, py = GetCursorPosition()
    local scale = Minimap:GetEffectiveScale()
    px, py = px / scale, py / scale
    db.minimapPos = math.deg(math.atan2(py - my, px - mx)) % 360
    PlaceButton()
end

button:SetScript("OnDragStart", function(self)
    self:LockHighlight()
    self:SetScript("OnUpdate", DragUpdate)
end)
button:SetScript("OnDragStop", function(self)
    self:SetScript("OnUpdate", nil)
    self:UnlockHighlight()
end)

-- ---- panel ------------------------------------------------------------------------
local PANEL_W = 230
local panel = CreateFrame("Frame", "OOBRadioPanel", UIParent)
panel:SetWidth(PANEL_W); panel:SetHeight(158)
panel:SetFrameStrata("DIALOG")
panel:SetClampedToScreen(true)
panel:SetBackdrop({
    bgFile   = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 14,
    insets = { left = 4, right = 4, top = 4, bottom = 4 },
})
panel:SetBackdropColor(0, 0, 0, 0.92)
panel:EnableMouse(true)
panel:Hide()
tinsert(UISpecialFrames, "OOBRadioPanel")   -- Escape closes it

local title = panel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
title:SetPoint("TOPLEFT", panel, "TOPLEFT", 14, -12)
title:SetText("|cffffa040" .. ADDON_TITLE .. "|r")

local closeBtn = CreateFrame("Button", nil, panel, "UIPanelCloseButton")
closeBtn:SetPoint("TOPRIGHT", panel, "TOPRIGHT", 2, 2)
closeBtn:SetWidth(26); closeBtn:SetHeight(26)

-- power button
local power = CreateFrame("Button", "OOBRadioPowerButton", panel, "UIPanelButtonTemplate")
power:SetWidth(PANEL_W - 28); power:SetHeight(24)
power:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -10)
power:SetScript("OnClick", function()
    PlaySound("igMainMenuOptionCheckBoxOn")
    Toggle()
end)

-- now playing
local npLabel = panel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
npLabel:SetPoint("TOPLEFT", power, "BOTTOMLEFT", 2, -10)
npLabel:SetText("NOW PLAYING")
npLabel:SetTextColor(0.6, 0.6, 0.6)

local npState = panel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
npState:SetPoint("TOPRIGHT", power, "BOTTOMRIGHT", -2, -10)
npState:SetJustifyH("RIGHT")

local npTrack = panel:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
npTrack:SetPoint("TOPLEFT", npLabel, "BOTTOMLEFT", 0, -3)
npTrack:SetWidth(PANEL_W - 32)
npTrack:SetHeight(28)
npTrack:SetJustifyH("LEFT")
npTrack:SetJustifyV("TOP")

-- volume
local volLabel = panel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
volLabel:SetPoint("TOPLEFT", npTrack, "BOTTOMLEFT", 0, -8)
volLabel:SetText("VOLUME")
volLabel:SetTextColor(0.6, 0.6, 0.6)

local volPct = panel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
volPct:SetPoint("TOPRIGHT", npTrack, "BOTTOMRIGHT", 0, -8)
volPct:SetJustifyH("RIGHT")

local slider = CreateFrame("Slider", "OOBRadioVolumeSlider", panel, "OptionsSliderTemplate")
slider:SetOrientation("HORIZONTAL")
slider:SetWidth(PANEL_W - 32); slider:SetHeight(16)
slider:SetPoint("TOPLEFT", volLabel, "BOTTOMLEFT", 0, -6)
slider:SetMinMaxValues(0, 100)
slider:SetValueStep(1)
getglobal(slider:GetName() .. "Low"):SetText("")
getglobal(slider:GetName() .. "High"):SetText("")
getglobal(slider:GetName() .. "Text"):SetText("")
slider:EnableMouseWheel(true)
slider:SetScript("OnValueChanged", function(self, value)
    value = math.floor(value + 0.5)
    db.volume = value
    volPct:SetText(value .. "%")
    if HasDLL() then Radio_SetVolume(EffectiveVolume()) end
end)
slider:SetScript("OnMouseWheel", function(self, delta)
    self:SetValue(self:GetValue() + delta * 5)
end)

-- Anchor the panel beside the button, on whichever side has room.
local function PlacePanel()
    panel:ClearAllPoints()
    local x = button:GetCenter()
    local half = UIParent:GetWidth() / 2
    if x and x > half then
        panel:SetPoint("TOPRIGHT", button, "BOTTOMLEFT", 6, -2)
    else
        panel:SetPoint("TOPLEFT", button, "BOTTOMRIGHT", -6, -2)
    end
end

local function TogglePanel()
    if panel:IsShown() then panel:Hide() else PlacePanel(); Refresh(); panel:Show() end
end

-- ---- painting ---------------------------------------------------------------------
local lastTrack
Refresh = function()
    local state, track = Status()
    local on = (state == "playing" or state == "connecting")

    if state == "missing" then
        power:SetText("Extension not loaded")
        power:Disable()
        npState:SetText("|cffff4040unavailable|r")
        npTrack:SetText("The radio extension is not loaded.\nCheck DivxDecoder.dll and bass.dll.")
        icon:SetVertexColor(0.4, 0.4, 0.4)
        led:Hide()
        slider:Hide(); volLabel:Hide(); volPct:Hide()
        return
    end

    power:Enable()
    slider:Show(); volLabel:Show(); volPct:Show()
    if on then
        power:SetText("Turn Off")
        icon:SetVertexColor(1, 1, 1)
        led:Show()
    else
        power:SetText("Tune In")
        icon:SetVertexColor(0.55, 0.55, 0.55)
        led:Hide()
    end

    if state == "playing" then
        npState:SetText("|cff40ff40live|r")
        npTrack:SetText(track ~= "" and track or "...")
    elseif state == "connecting" then
        npState:SetText("|cffffd040connecting|r")
        npTrack:SetText("...")
    elseif state == "error" then
        npState:SetText("|cffff4040error|r")
        npTrack:SetText("Could not reach the station. Try again in a moment.")
    else
        npState:SetText("|cff808080off|r")
        npTrack:SetText("")
    end

    if slider:GetValue() ~= db.volume then slider:SetValue(db.volume) end
    volPct:SetText(db.volume .. "%")

    if on and track ~= "" and track ~= lastTrack then
        lastTrack = track
        if not panel:IsShown() then Say("Now playing: " .. track) end
    elseif not on then
        lastTrack = nil
    end
end

-- ---- tooltip ----------------------------------------------------------------------
local function ShowTooltip(self)
    GameTooltip:SetOwner(self, "ANCHOR_LEFT")
    GameTooltip:SetText(ADDON_TITLE)
    local state, track = Status()
    if state == "playing" then
        GameTooltip:AddLine("|cff40ff40On air|r  " .. (track ~= "" and track or ""), 1, 1, 1, true)
    elseif state == "missing" then
        GameTooltip:AddLine("Extension not loaded", 1, 0.3, 0.3)
    else
        GameTooltip:AddLine("Off", 0.6, 0.6, 0.6)
    end
    GameTooltip:AddLine(" ")
    GameTooltip:AddLine("Left-click: open the radio", 0.8, 0.8, 0.8)
    GameTooltip:AddLine("Right-click: tune in / turn off", 0.8, 0.8, 0.8)
    GameTooltip:AddLine("Drag: move the button", 0.8, 0.8, 0.8)
    GameTooltip:Show()
end

button:SetScript("OnEnter", ShowTooltip)
button:SetScript("OnLeave", function() GameTooltip:Hide() end)
button:SetScript("OnClick", function(self, mouse)
    if mouse == "RightButton" then
        PlaySound("igMainMenuOptionCheckBoxOn")
        Toggle()
    else
        TogglePanel()
    end
end)

-- ---- housekeeping ---------------------------------------------------------------
-- Every quarter second: keep game music off while on, follow master volume,
-- refresh the track display.
local ticker = CreateFrame("Frame")
local acc, lastEff, lastBg = 0, -1, nil
ticker:SetScript("OnUpdate", function(self, elapsed)
    acc = acc + elapsed
    if acc < 0.25 then return end
    acc = 0
    if not db or not HasDLL() then return end
    if IsOn() then
        if GetCVar("Sound_EnableMusic") ~= "0" then SuppressGameMusic() end
        local eff = EffectiveVolume()
        if eff ~= lastEff then Radio_SetVolume(eff); lastEff = eff end
        local bg = GetCVar("Sound_EnableSoundWhenGameIsInBG") ~= "1"
        if bg ~= lastBg then Radio_SetBackgroundMute(bg); lastBg = bg end
    end
    if panel:IsShown() or IsOn() then Refresh() end
    if GameTooltip:IsOwned(button) and GameTooltip:IsShown() then ShowTooltip(button) end
end)

local events = CreateFrame("Frame")
events:RegisterEvent("ADDON_LOADED")
events:RegisterEvent("PLAYER_ENTERING_WORLD")
events:RegisterEvent("ZONE_CHANGED")
events:RegisterEvent("ZONE_CHANGED_INDOORS")
events:RegisterEvent("ZONE_CHANGED_NEW_AREA")
events:RegisterEvent("PLAYER_LOGOUT")
events:SetScript("OnEvent", function(self, event, arg1)
    if event == "ADDON_LOADED" then
        if arg1 ~= "OOBRadio" then return end
        for k, v in pairs(DEFAULTS) do
            if OOBRadioDB[k] == nil then OOBRadioDB[k] = v end
        end
        db = OOBRadioDB
        PlaceButton()
        if db.hideButton then button:Hide() end
        slider:SetValue(db.volume)
        Refresh()
        if not HasDLL() then
            Say("Radio extension not detected; the radio will not play until DivxDecoder.dll and bass.dll are installed.")
        end
    elseif event == "PLAYER_ENTERING_WORLD" then
        if not db then return end
        if IsOn() then
            SuppressGameMusic()             -- still playing across /reload
        elseif db.enabled and HasDLL() then
            TurnOn(true)                    -- resume from last session
        elseif db.savedMusic ~= nil then
            RestoreGameMusic()              -- stale value from a crash
        end
        Refresh()
    elseif event == "PLAYER_LOGOUT" then
        -- keep the player's real music setting in Config.wtf, not our forced zero
        if db and db.savedMusic ~= nil then SetCVar("Sound_EnableMusic", db.savedMusic) end
    else
        if IsOn() then SuppressGameMusic() end
    end
end)

-- ---- slash commands ---------------------------------------------------------------
SLASH_OOBRADIO1 = "/radio"
SLASH_OOBRADIO2 = "/oob"
SlashCmdList["OOBRADIO"] = function(msg)
    msg = string.lower(msg or "")
    local cmd, arg = string.match(msg, "^(%S*)%s*(.-)$")
    if cmd == "on" then TurnOn()
    elseif cmd == "off" then TurnOff()
    elseif cmd == "vol" or cmd == "volume" then
        local v = tonumber(arg)
        if v then slider:SetValue(math.max(0, math.min(100, v))) else Say("Volume is " .. db.volume .. "%") end
    elseif cmd == "button" then
        db.hideButton = not db.hideButton
        if db.hideButton then button:Hide() else button:Show() end
    elseif cmd == "" or cmd == "show" then TogglePanel()
    else
        Say("/radio  -  open the radio panel")
        Say("/radio on | off | vol <0-100> | button")
    end
end

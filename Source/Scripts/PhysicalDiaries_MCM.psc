; ======== PhysicalDiaries_MCM  -  extends SKI_ConfigBase ========

Scriptname PhysicalDiaries_MCM extends SKI_ConfigBase

; ======== Native functions (registered in PapyrusAPI.cpp on "PhysicalDiaries_MCM") ========

bool Function RegenerateTextsOnly() global native
bool Function ResetAllDiaries() global native
bool Function GetDebugLog()                      global native
     Function SetDebugLog(bool value)            global native
bool Function GetShowDateHeaders()               global native
     Function SetShowDateHeaders(bool value)     global native
bool Function GetPlayerDiaryBooks()              global native
     Function SetPlayerDiaryBooks(int value)     global native  ; 1 on, 0 off, -1 default (by writing mode)
int  Function GetEntriesPerVolume()              global native
     Function SetEntriesPerVolume(int value)     global native
int  Function GetFontSizeTitle()                 global native
     Function SetFontSizeTitle(int value)        global native
int  Function GetFontSizeDate()                  global native
     Function SetFontSizeDate(int value)         global native
int  Function GetFontSizeContent()               global native
     Function SetFontSizeContent(int value)      global native
int  Function GetFontSizeSmall()                 global native
     Function SetFontSizeSmall(int value)        global native
bool Function IsWritingOn()                      global native  ; Ink & Quill installed, writing on
int  Function GetDeleteKey()                     global native
     Function SetDeleteKey(int value)            global native
int  Function GetNewEntryKey()                   global native
     Function SetNewEntryKey(int value)          global native
string Function CheckWritingKey(int value)       global native  ; "": Ink & Quill lets it act while writing; else why not
string Function GetFontFace()                    global native
       Function SetFontFace(string value)        global native
Function RefreshSkyrimNetSettings() global native  ; read SkyrimNet's diary settings again (on open)
; [NpcDiaries] settings by key: Enabled, DailyRandom, RunHour, CloseBoost; and SkyrimNet's own diary
; settings, SkyrimNetDiaries and SkyrimNetDayBoundary (changed in SkyrimNet's config)
int  Function GetNpcSetting(string key)          global native
     Function SetNpcSetting(string key, int value) global native
; This save's NPCs who write every day, in the order added.  Add: 1 added, 0 already there, -1 can't.
bool     Function IsDailyWriter(Actor akActor)     global native
int      Function AddDailyWriter(Actor akActor)    global native
string[] Function GetDailyWriterNames()            global native
         Function RemoveDailyWriter(int index)     global native

; ======== Option handles ========
int oidEntriesPerVolume  = -1
int oidShowDateHeaders   = -1
int oidPlayerDiaryBooks  = -1
int oidDebugLog          = -1
int oidFontSizeTitle     = -1
int oidFontSizeDate      = -1
int oidFontSizeContent   = -1
int oidFontSizeSmall     = -1
int oidFontFace          = -1
int oidDeleteKey         = -1
int oidNewEntryKey       = -1
int oidResetAll          = -1
int oidNpcEnabled        = -1
int oidNpcDailyRandom    = -1
int oidNpcRunHour        = -1
int oidNpcCloseBoost     = -1
int oidSkyrimNetDiaries   = -1
int oidSkyrimNetDayBound  = -1
int oidNpcDailyWriters   = -1
int oidAddTarget         = -1

; The Daily Writers page: each writer's option and name, in the order added, and the NPC in the crosshair.
int[] _writerOids
string[] _writerNames
int _writerCount = 0
Actor _target

; Font presets
string[] _fontValues
string[] _fontDisplayNames
int _fontIndex = 0

; Snapshot on open for change detection
int _fontTitleOnOpen   = 0
int _fontDateOnOpen    = 0
int _fontContentOnOpen = 0
int _fontSmallOnOpen   = 0
string _fontFaceOnOpen = ""

; ======== Lifecycle ========

event OnConfigInit()
    ModName = "Physical Diaries"
    SetPages()

    _fontValues = new string[3]
    _fontValues[0] = "$HandwrittenFont"
    _fontValues[1] = "$EverywhereFont"
    _fontValues[2] = "$SkyrimBooks"

    _fontDisplayNames = new string[3]
    _fontDisplayNames[0] = "Handwritten"
    _fontDisplayNames[1] = "Everywhere"
    _fontDisplayNames[2] = "Book"
endevent

; Also on open: OnConfigInit runs once per save, so a save made before the Daily Writers page needs it here.
function SetPages()
    Pages    = new string[3]
    Pages[0] = "$SNPD_PageSettings"
    Pages[1] = "$SNPD_PageDailyWriters"
    Pages[2] = "$SNPD_PageMaintenance"
endfunction

event OnConfigOpen()
    SetPages()
    ; Always rebuild font arrays (OnConfigInit only runs once per save,
    ; so these may be uninitialized on existing saves with older scripts)
    _fontValues = new string[3]
    _fontValues[0] = "$HandwrittenFont"
    _fontValues[1] = "$EverywhereFont"
    _fontValues[2] = "$SkyrimBooks"

    _fontDisplayNames = new string[3]
    _fontDisplayNames[0] = "Handwritten"
    _fontDisplayNames[1] = "Everywhere"
    _fontDisplayNames[2] = "Book"

    RefreshSkyrimNetSettings()
    _fontTitleOnOpen   = GetFontSizeTitle()
    _fontDateOnOpen    = GetFontSizeDate()
    _fontContentOnOpen = GetFontSizeContent()
    _fontSmallOnOpen   = GetFontSizeSmall()
    _fontFaceOnOpen    = GetFontFace()
endevent

event OnConfigClose()
    if _fontTitleOnOpen   != GetFontSizeTitle()   || \
       _fontDateOnOpen    != GetFontSizeDate()    || \
       _fontContentOnOpen != GetFontSizeContent() || \
       _fontSmallOnOpen   != GetFontSizeSmall()   || \
       _fontFaceOnOpen    != GetFontFace()
        RegenerateTextsOnly()
    endif
endevent

function UpdateFontIndex()
    string current = GetFontFace()
    _fontIndex = 0
    int i = 0
    while i < _fontValues.Length
        if _fontValues[i] == current
            _fontIndex = i
            return
        endif
        i += 1
    endwhile
endfunction

; ======== Page rendering ========

event OnPageReset(string page)
    SetCursorFillMode(TOP_TO_BOTTOM)
    SetCursorPosition(0)

    oidEntriesPerVolume = -1
    oidShowDateHeaders  = -1
    oidPlayerDiaryBooks = -1
    oidDebugLog         = -1
    oidFontSizeTitle    = -1
    oidFontSizeDate     = -1
    oidFontSizeContent  = -1
    oidFontSizeSmall    = -1
    oidFontFace         = -1
    oidDeleteKey        = -1
    oidNewEntryKey      = -1
    oidResetAll         = -1
    oidNpcEnabled       = -1
    oidNpcDailyRandom   = -1
    oidNpcRunHour       = -1
    oidNpcCloseBoost    = -1
    oidSkyrimNetDiaries = -1
    oidSkyrimNetDayBound = -1
    oidNpcDailyWriters  = -1
    oidAddTarget        = -1
    _writerCount        = 0

    if page == Pages[0]
        RenderSettingsPage()
    elseif page == Pages[1]
        RenderDailyWritersPage()
    elseif page == Pages[2]
        RenderMaintenancePage()
    endif
endevent

function RenderSettingsPage()
    AddHeaderOption("$SNPD_HeaderDiaryVolumes")
    oidEntriesPerVolume = AddSliderOption("$SNPD_EntriesPerVolume", GetEntriesPerVolume(), "{0}")
    oidShowDateHeaders  = AddToggleOption("$SNPD_ShowDateHeaders", GetShowDateHeaders())
    oidPlayerDiaryBooks = AddToggleOption("$SNPD_PlayerDiaryBooks", GetPlayerDiaryBooks())

    AddHeaderOption("$SNPD_HeaderFontSizes")
    UpdateFontIndex()
    oidFontFace        = AddMenuOption("$SNPD_FontFace", _fontDisplayNames[_fontIndex])
    oidFontSizeTitle   = AddSliderOption("$SNPD_TitleFontSize",   GetFontSizeTitle(),   "{0}")
    oidFontSizeDate    = AddSliderOption("$SNPD_DateFontSize",    GetFontSizeDate(),    "{0}")
    oidFontSizeContent = AddSliderOption("$SNPD_ContentFontSize", GetFontSizeContent(), "{0}")
    oidFontSizeSmall   = AddSliderOption("$SNPD_SmallFontSize",   GetFontSizeSmall(),   "{0}")

    SetCursorPosition(1)  ; the right column
    AddHeaderOption("$SNPD_HeaderWriting")
    ; 0 in the INI is unbound (the default); SkyUI shows -1 as no key.  Writing is Ink & Quill's key.
    int newEntryKey = GetNewEntryKey()
    if newEntryKey == 0
        newEntryKey = -1
    endif
    int writingFlags = OPTION_FLAG_NONE
    if !IsWritingOn()
        writingFlags = OPTION_FLAG_DISABLED
    endif
    oidDeleteKey = AddKeyMapOption("$SNPD_DeleteKey", GetDeleteKey(), writingFlags)
    oidNewEntryKey = AddKeyMapOption("$SNPD_NewEntryKey", newEntryKey, writingFlags)

    AddHeaderOption("$SNPD_HeaderNpcDiaries")
    oidNpcEnabled      = AddToggleOption("$SNPD_NpcEnabled", GetNpcSetting("Enabled") != 0)
    oidNpcDailyRandom  = AddSliderOption("$SNPD_NpcDailyRandom", GetNpcSetting("DailyRandom"), "{0}")
    oidNpcRunHour      = AddSliderOption("$SNPD_NpcRunHour", GetNpcSetting("RunHour"), "{0}:00")
    oidNpcCloseBoost   = AddToggleOption("$SNPD_NpcCloseBoost", GetNpcSetting("CloseBoost") != 0)

    AddHeaderOption("$SNPD_HeaderSkyrimNet")
    oidSkyrimNetDiaries = AddToggleOption("$SNPD_SkyrimNetDiaries", GetNpcSetting("SkyrimNetDiaries") != 0)
    oidSkyrimNetDayBound = AddToggleOption("$SNPD_SkyrimNetDayBoundary", GetNpcSetting("SkyrimNetDayBoundary") != 0)
endfunction

; The switch and the add button, a rule, then the writers two to a row (selecting one asks to remove it).
; At most 128 are listed.
function RenderDailyWritersPage()
    SetCursorFillMode(LEFT_TO_RIGHT)
    oidNpcDailyWriters = AddToggleOption("$SNPD_NpcDailyWriters", GetNpcSetting("DailyWriters") != 0)
    _target = Game.GetCurrentCrosshairRef() as Actor
    if _target && _target != Game.GetPlayer() && !IsDailyWriter(_target)
        oidAddTarget = AddTextOption("$SNPD_AddTarget", _target.GetDisplayName())
    else
        oidAddTarget = AddTextOption("$SNPD_AddTarget", "", OPTION_FLAG_DISABLED)
    endif
    AddHeaderOption("")
    AddHeaderOption("")

    _writerNames = GetDailyWriterNames()
    _writerOids = new int[128]
    _writerCount = _writerNames.Length
    if _writerCount > 128
        _writerCount = 128
    endif
    int i = 0
    while i < _writerCount
        _writerOids[i] = AddTextOption(_writerNames[i], "")
        i += 1
    endwhile
endfunction

function RenderMaintenancePage()
    AddHeaderOption("$SNPD_HeaderMaintenance")
    oidResetAll = AddTextOption("$SNPD_ResetAllDiaries", "")

    SetCursorPosition(1)
    AddHeaderOption("$SNPD_HeaderLogging")
    oidDebugLog = AddToggleOption("$SNPD_DebugLogging", GetDebugLog())
endfunction

; ======== Option select (toggles, reset button) ========

event OnOptionSelect(int oid)
    if oid == oidShowDateHeaders
        bool newVal = !GetShowDateHeaders()
        SetShowDateHeaders(newVal)
        SetToggleOptionValue(oid, newVal)
        RegenerateTextsOnly()
    elseif oid == oidPlayerDiaryBooks
        bool newVal = !GetPlayerDiaryBooks()
        SetPlayerDiaryBooks(newVal as int)
        SetToggleOptionValue(oid, newVal)
    elseif oid == oidDebugLog
        bool newVal = !GetDebugLog()
        SetDebugLog(newVal)
        SetToggleOptionValue(oid, newVal)
    elseif oid == oidNpcEnabled
        bool newVal = GetNpcSetting("Enabled") == 0
        SetNpcSetting("Enabled", newVal as int)
        SetToggleOptionValue(oid, newVal)
    elseif oid == oidNpcCloseBoost
        bool newVal = GetNpcSetting("CloseBoost") == 0
        SetNpcSetting("CloseBoost", newVal as int)
        SetToggleOptionValue(oid, newVal)
    elseif oid == oidSkyrimNetDiaries
        bool newVal = GetNpcSetting("SkyrimNetDiaries") == 0
        SetNpcSetting("SkyrimNetDiaries", newVal as int)
        SetToggleOptionValue(oid, newVal)
    elseif oid == oidSkyrimNetDayBound
        bool newVal = GetNpcSetting("SkyrimNetDayBoundary") == 0
        SetNpcSetting("SkyrimNetDayBoundary", newVal as int)
        SetToggleOptionValue(oid, newVal)
    elseif oid == oidResetAll
        bool confirmed = ShowMessage( \
            "$SNPD_ResetConfirmMsg", \
            true, "$SNPD_Confirm", "$SNPD_Cancel")
        if confirmed
            bool ok = ResetAllDiaries()
            if ok
                ShowMessage("$SNPD_ResetSuccessMsg", false, "$SNPD_OK")
            else
                ShowMessage("$SNPD_ResetErrorMsg", false, "$SNPD_OK")
            endif
            ForcePageReset()
        endif
    elseif oid == oidNpcDailyWriters
        bool newVal = GetNpcSetting("DailyWriters") == 0
        SetNpcSetting("DailyWriters", newVal as int)
        SetToggleOptionValue(oid, newVal)
    elseif oid == oidAddTarget
        if AddDailyWriter(_target) >= 0
            ForcePageReset()
        endif
    else
        int i = 0
        while i < _writerCount
            if _writerOids[i] == oid
                if ShowMessage("$SNPD_RemoveWriter{" + _writerNames[i] + "}", true, "$SNPD_Confirm", "$SNPD_Cancel")
                    RemoveDailyWriter(i)
                    ForcePageReset()
                endif
                return
            endif
            i += 1
        endwhile
    endif
endevent

; ======== Menu open (font selector dropdown) ========

event OnOptionMenuOpen(int oid)
    ; Only one menu exists — always populate it
    SetMenuDialogOptions(_fontDisplayNames)
    SetMenuDialogStartIndex(_fontIndex)
    SetMenuDialogDefaultIndex(0)
endevent

; ======== Menu accept (font selected from dropdown) ========

event OnOptionMenuAccept(int oid, int idx)
    ; Accept any valid menu selection — only one menu exists on this page
    if idx >= 0 && idx < _fontValues.Length
        _fontIndex = idx
        SetFontFace(_fontValues[idx])
        SetMenuOptionValue(oidFontFace, _fontDisplayNames[idx])
    endif
endevent

; ======== Slider open ========

event OnOptionSliderOpen(int oid)
    if oid == oidEntriesPerVolume
        SetSliderDialogStartValue(GetEntriesPerVolume())
        SetSliderDialogDefaultValue(10)
        SetSliderDialogRange(1, 50)
        SetSliderDialogInterval(1)
    elseif oid == oidFontSizeTitle
        SetSliderDialogStartValue(GetFontSizeTitle())
        SetSliderDialogDefaultValue(18)
        SetSliderDialogRange(8, 24)
        SetSliderDialogInterval(1)
    elseif oid == oidFontSizeDate
        SetSliderDialogStartValue(GetFontSizeDate())
        SetSliderDialogDefaultValue(16)
        SetSliderDialogRange(8, 24)
        SetSliderDialogInterval(1)
    elseif oid == oidFontSizeContent
        SetSliderDialogStartValue(GetFontSizeContent())
        SetSliderDialogDefaultValue(14)
        SetSliderDialogRange(8, 24)
        SetSliderDialogInterval(1)
    elseif oid == oidFontSizeSmall
        SetSliderDialogStartValue(GetFontSizeSmall())
        SetSliderDialogDefaultValue(12)
        SetSliderDialogRange(8, 24)
        SetSliderDialogInterval(1)
    elseif oid == oidNpcDailyRandom
        SetSliderDialogStartValue(GetNpcSetting("DailyRandom"))
        SetSliderDialogDefaultValue(3)
        SetSliderDialogRange(0, 20)
        SetSliderDialogInterval(1)
    elseif oid == oidNpcRunHour
        SetSliderDialogStartValue(GetNpcSetting("RunHour"))
        SetSliderDialogDefaultValue(22)
        SetSliderDialogRange(12, 23)
        SetSliderDialogInterval(1)
    endif
endevent

; ======== Slider accept ========

event OnOptionSliderAccept(int oid, float value)
    int intVal = value as int
    if oid == oidEntriesPerVolume
        SetEntriesPerVolume(intVal)
        SetSliderOptionValue(oid, intVal, "{0}")
    elseif oid == oidFontSizeTitle
        SetFontSizeTitle(intVal)
        SetSliderOptionValue(oid, intVal, "{0}")
    elseif oid == oidFontSizeDate
        SetFontSizeDate(intVal)
        SetSliderOptionValue(oid, intVal, "{0}")
    elseif oid == oidFontSizeContent
        SetFontSizeContent(intVal)
        SetSliderOptionValue(oid, intVal, "{0}")
    elseif oid == oidFontSizeSmall
        SetFontSizeSmall(intVal)
        SetSliderOptionValue(oid, intVal, "{0}")
    elseif oid == oidNpcDailyRandom
        SetNpcSetting("DailyRandom", intVal)
        SetSliderOptionValue(oid, intVal, "{0}")
    elseif oid == oidNpcRunHour
        SetNpcSetting("RunHour", intVal)
        SetSliderOptionValue(oid, intVal, "{0}:00")
    endif
endevent

; ======== Key maps (tear-out and new-entry keys) ========

; A key Ink & Quill refuses while writing is refused here too, with its reason; the old key stays.
event OnOptionKeyMapChange(int oid, int keyCode, string conflictControl, string conflictName)
    if keyCode <= 0 || (oid != oidDeleteKey && oid != oidNewEntryKey)
        return
    endif
    string refused = CheckWritingKey(keyCode)
    if refused != ""
        ShowMessage(refused, false)
        return
    endif
    if oid == oidDeleteKey
        SetDeleteKey(keyCode)
    else
        SetNewEntryKey(keyCode)
    endif
    SetKeyMapOptionValue(oid, keyCode)
endevent

; ======== Highlight / tooltip ========

event OnOptionHighlight(int oid)
    if oid == oidEntriesPerVolume
        SetInfoText("$SNPD_TipEntriesPerVolume")
    elseif oid == oidShowDateHeaders
        SetInfoText("$SNPD_TipShowDateHeaders")
    elseif oid == oidPlayerDiaryBooks
        SetInfoText("$SNPD_TipPlayerDiaryBooks")
    elseif oid == oidDebugLog
        SetInfoText("$SNPD_TipDebugLog")
    elseif oid == oidFontFace
        SetInfoText("$SNPD_TipFontFace")
    elseif oid == oidFontSizeTitle
        SetInfoText("$SNPD_TipTitleFontSize")
    elseif oid == oidFontSizeDate
        SetInfoText("$SNPD_TipDateFontSize")
    elseif oid == oidFontSizeContent
        SetInfoText("$SNPD_TipContentFontSize")
    elseif oid == oidFontSizeSmall
        SetInfoText("$SNPD_TipSmallFontSize")
    elseif oid == oidDeleteKey
        SetInfoText("$SNPD_TipDeleteKey")
    elseif oid == oidNewEntryKey
        SetInfoText("$SNPD_TipNewEntryKey")
    elseif oid == oidResetAll
        SetInfoText("$SNPD_TipResetAll")
    elseif oid == oidNpcEnabled
        SetInfoText("$SNPD_TipNpcEnabled")
    elseif oid == oidNpcDailyRandom
        SetInfoText("$SNPD_TipNpcDailyRandom")
    elseif oid == oidNpcRunHour
        SetInfoText("$SNPD_TipNpcRunHour")
    elseif oid == oidNpcCloseBoost
        SetInfoText("$SNPD_TipNpcCloseBoost")
    elseif oid == oidSkyrimNetDiaries
        SetInfoText("$SNPD_TipSkyrimNetDiaries")
    elseif oid == oidSkyrimNetDayBound
        SetInfoText("$SNPD_TipSkyrimNetDayBoundary")
    elseif oid == oidNpcDailyWriters
        SetInfoText("$SNPD_TipNpcDailyWriters")
    elseif oid == oidAddTarget
        SetInfoText("$SNPD_TipAddTarget")
    elseif _writerCount > 0 && _writerOids.Find(oid) >= 0 && _writerOids.Find(oid) < _writerCount
        SetInfoText("$SNPD_TipWriterRemove")
    endif
endevent

; ======== Default reset ========

event OnOptionDefault(int oid)
    if oid == oidNpcDailyWriters
        SetNpcSetting("DailyWriters", 1)
        SetToggleOptionValue(oid, true)
    elseif oid == oidEntriesPerVolume
        SetEntriesPerVolume(10)
        SetSliderOptionValue(oid, 10.0, "{0}")
    elseif oid == oidShowDateHeaders
        SetShowDateHeaders(true)
        SetToggleOptionValue(oid, true)
    elseif oid == oidPlayerDiaryBooks
        SetPlayerDiaryBooks(-1)
        SetToggleOptionValue(oid, GetPlayerDiaryBooks())
    elseif oid == oidDebugLog
        SetDebugLog(false)
        SetToggleOptionValue(oid, false)
    elseif oid == oidFontFace
        _fontIndex = 0
        SetFontFace("$HandwrittenFont")
        SetMenuOptionValue(oid, _fontDisplayNames[0])
    elseif oid == oidFontSizeTitle
        SetFontSizeTitle(18)
        SetSliderOptionValue(oid, 18.0, "{0}")
    elseif oid == oidFontSizeDate
        SetFontSizeDate(16)
        SetSliderOptionValue(oid, 16.0, "{0}")
    elseif oid == oidFontSizeContent
        SetFontSizeContent(14)
        SetSliderOptionValue(oid, 14.0, "{0}")
    elseif oid == oidFontSizeSmall
        SetFontSizeSmall(12)
        SetSliderOptionValue(oid, 12.0, "{0}")
    elseif oid == oidDeleteKey
        SetDeleteKey(68)
        SetKeyMapOptionValue(oid, 68)
    elseif oid == oidNewEntryKey
        SetNewEntryKey(66)
        SetKeyMapOptionValue(oid, 66)
    elseif oid == oidNpcEnabled
        SetNpcSetting("Enabled", 0)
        SetToggleOptionValue(oid, false)
    elseif oid == oidNpcDailyRandom
        SetNpcSetting("DailyRandom", 3)
        SetSliderOptionValue(oid, 3.0, "{0}")
    elseif oid == oidNpcRunHour
        SetNpcSetting("RunHour", 22)
        SetSliderOptionValue(oid, 22.0, "{0}:00")
    elseif oid == oidNpcCloseBoost
        SetNpcSetting("CloseBoost", 1)
        SetToggleOptionValue(oid, true)
    elseif oid == oidSkyrimNetDiaries
        SetNpcSetting("SkyrimNetDiaries", 1)
        SetToggleOptionValue(oid, true)
    elseif oid == oidSkyrimNetDayBound
        SetNpcSetting("SkyrimNetDayBoundary", 1)
        SetToggleOptionValue(oid, true)
    endif
endevent

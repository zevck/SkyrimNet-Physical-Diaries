; =============================================================================
; SkyrimNetDiaries_MCM  -  extends SKI_ConfigBase
; =============================================================================

Scriptname SkyrimNetDiaries_MCM extends SKI_ConfigBase

; ============================================================================
; Native functions (registered in PapyrusAPI.cpp on "SkyrimNetDiaries_MCM")
; ============================================================================

bool Function RegenerateTextsOnly() global native
bool Function ResetAllDiaries() global native
bool Function GetDebugLog()                      global native
     Function SetDebugLog(bool value)            global native
bool Function GetShowDateHeaders()               global native
     Function SetShowDateHeaders(bool value)     global native
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
string Function GetFontFace()                    global native
       Function SetFontFace(string value)        global native

; ============================================================================
; Option handles
; ============================================================================
int oidEntriesPerVolume  = -1
int oidShowDateHeaders   = -1
int oidDebugLog          = -1
int oidFontSizeTitle     = -1
int oidFontSizeDate      = -1
int oidFontSizeContent   = -1
int oidFontSizeSmall     = -1
int oidFontFace          = -1
int oidResetAll          = -1

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

; ============================================================================
; Lifecycle
; ============================================================================

event OnConfigInit()
    ModName = "SkyrimNet Physical Diaries"
    Pages   = new string[2]
    Pages[0] = "$SNPD_PageSettings"
    Pages[1] = "$SNPD_PageMaintenance"

    _fontValues = new string[3]
    _fontValues[0] = "$HandwrittenFont"
    _fontValues[1] = "$EverywhereFont"
    _fontValues[2] = "$SkyrimBooks"

    _fontDisplayNames = new string[3]
    _fontDisplayNames[0] = "Handwritten"
    _fontDisplayNames[1] = "Everywhere"
    _fontDisplayNames[2] = "Book"
endevent

event OnConfigOpen()
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

; ============================================================================
; Page rendering
; ============================================================================

event OnPageReset(string page)
    SetCursorFillMode(TOP_TO_BOTTOM)
    SetCursorPosition(0)

    oidEntriesPerVolume = -1
    oidShowDateHeaders  = -1
    oidDebugLog         = -1
    oidFontSizeTitle    = -1
    oidFontSizeDate     = -1
    oidFontSizeContent  = -1
    oidFontSizeSmall    = -1
    oidFontFace         = -1
    oidResetAll         = -1

    if page == Pages[0]
        RenderSettingsPage()
    elseif page == Pages[1]
        RenderMaintenancePage()
    endif
endevent

function RenderSettingsPage()
    AddHeaderOption("$SNPD_HeaderDiaryVolumes")
    oidEntriesPerVolume = AddSliderOption("$SNPD_EntriesPerVolume", GetEntriesPerVolume(), "{0}")
    oidShowDateHeaders  = AddToggleOption("$SNPD_ShowDateHeaders", GetShowDateHeaders())

    AddHeaderOption("$SNPD_HeaderFontSizes")
    UpdateFontIndex()
    oidFontFace        = AddMenuOption("$SNPD_FontFace", _fontDisplayNames[_fontIndex])
    oidFontSizeTitle   = AddSliderOption("$SNPD_TitleFontSize",   GetFontSizeTitle(),   "{0}")
    oidFontSizeDate    = AddSliderOption("$SNPD_DateFontSize",    GetFontSizeDate(),    "{0}")
    oidFontSizeContent = AddSliderOption("$SNPD_ContentFontSize", GetFontSizeContent(), "{0}")
    oidFontSizeSmall   = AddSliderOption("$SNPD_SmallFontSize",   GetFontSizeSmall(),   "{0}")
endfunction

function RenderMaintenancePage()
    AddHeaderOption("$SNPD_HeaderMaintenance")
    oidResetAll = AddTextOption("$SNPD_ResetAllDiaries", "")

    AddHeaderOption("$SNPD_HeaderLogging")
    oidDebugLog = AddToggleOption("$SNPD_DebugLogging", GetDebugLog())
endfunction

; ============================================================================
; Option select (toggles, reset button)
; ============================================================================

event OnOptionSelect(int oid)
    if oid == oidShowDateHeaders
        bool newVal = !GetShowDateHeaders()
        SetShowDateHeaders(newVal)
        SetToggleOptionValue(oid, newVal)
        RegenerateTextsOnly()
    elseif oid == oidDebugLog
        bool newVal = !GetDebugLog()
        SetDebugLog(newVal)
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
    endif
endevent

; ============================================================================
; Menu open (font selector dropdown)
; ============================================================================

event OnOptionMenuOpen(int oid)
    ; Only one menu exists — always populate it
    SetMenuDialogOptions(_fontDisplayNames)
    SetMenuDialogStartIndex(_fontIndex)
    SetMenuDialogDefaultIndex(0)
endevent

; ============================================================================
; Menu accept (font selected from dropdown)
; ============================================================================

event OnOptionMenuAccept(int oid, int idx)
    ; Accept any valid menu selection — only one menu exists on this page
    if idx >= 0 && idx < _fontValues.Length
        _fontIndex = idx
        SetFontFace(_fontValues[idx])
        SetMenuOptionValue(oidFontFace, _fontDisplayNames[idx])
    endif
endevent

; ============================================================================
; Slider open
; ============================================================================

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
    endif
endevent

; ============================================================================
; Slider accept
; ============================================================================

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
    endif
endevent

; ============================================================================
; Highlight / tooltip
; ============================================================================

event OnOptionHighlight(int oid)
    if oid == oidEntriesPerVolume
        SetInfoText("$SNPD_TipEntriesPerVolume")
    elseif oid == oidShowDateHeaders
        SetInfoText("$SNPD_TipShowDateHeaders")
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
    elseif oid == oidResetAll
        SetInfoText("$SNPD_TipResetAll")
    endif
endevent

; ============================================================================
; Default reset
; ============================================================================

event OnOptionDefault(int oid)
    if oid == oidEntriesPerVolume
        SetEntriesPerVolume(10)
        SetSliderOptionValue(oid, 10.0, "{0}")
    elseif oid == oidShowDateHeaders
        SetShowDateHeaders(true)
        SetToggleOptionValue(oid, true)
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
    endif
endevent

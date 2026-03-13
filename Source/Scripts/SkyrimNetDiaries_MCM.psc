; =============================================================================
; SkyrimNetDiaries_MCM  -  extends SKI_ConfigBase
;
; Provides two MCM pages:
;   "Settings"    - EntriesPerVolume slider + four font-size sliders
;   "Maintenance" - Reset All Diaries button
;
; SETUP (Creation Kit):
;   1. Create a new Quest in SkyrimNet Physical Diaries.esp
;      Name: SkyrimNetDiariesMCMQuest, Type: Misc, Start Game Enabled: yes
;   2. Attach this script to the quest.
;   3. Set ModName (below) or leave it to be set in OnConfigInit.
; =============================================================================

Scriptname SkyrimNetDiaries_MCM extends SKI_ConfigBase

; ============================================================================
; Native functions (registered in PapyrusAPI.cpp on "SkyrimNetDiaries_MCM")
; ============================================================================

; Maintenance
bool Function RegenerateTextsOnly() global native
bool Function ResetAllDiaries() global native
; Config getters / setters  (each setter persists the change to the INI file)
bool Function GetDebugLog()                      global native
     Function SetDebugLog(bool value)            global native
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

; ============================================================================
; Option handles  (populated in OnPageReset, used in event callbacks)
; ============================================================================
int oidEntriesPerVolume = -1
int oidDebugLog         = -1
int oidFontSizeTitle    = -1
int oidFontSizeDate     = -1
int oidFontSizeContent  = -1
int oidFontSizeSmall    = -1
int oidResetAll         = -1

; Font size values captured when the MCM opens, used to detect changes on close.
int _fontTitleOnOpen   = 0
int _fontDateOnOpen    = 0
int _fontContentOnOpen = 0
int _fontSmallOnOpen   = 0

; ============================================================================
; SKI_ConfigBase lifecycle
; ============================================================================

event OnConfigOpen()
    _fontTitleOnOpen   = GetFontSizeTitle()
    _fontDateOnOpen    = GetFontSizeDate()
    _fontContentOnOpen = GetFontSizeContent()
    _fontSmallOnOpen   = GetFontSizeSmall()
endevent

event OnConfigClose()
    if _fontTitleOnOpen   != GetFontSizeTitle()   || \
       _fontDateOnOpen    != GetFontSizeDate()    || \
       _fontContentOnOpen != GetFontSizeContent() || \
       _fontSmallOnOpen   != GetFontSizeSmall()
        RegenerateTextsOnly()
    endif
endevent

event OnConfigInit()
    ModName = "SkyrimNet Physical Diaries"
    Pages   = new string[2]
    Pages[0] = "Settings"
    Pages[1] = "Maintenance"
endevent

event OnPageReset(string page)
    SetCursorFillMode(TOP_TO_BOTTOM)
    SetCursorPosition(0)

    ; Reset all option handles so stale IDs cannot be matched
    oidEntriesPerVolume = -1
    oidDebugLog         = -1
    oidFontSizeTitle    = -1
    oidFontSizeDate     = -1
    oidFontSizeContent  = -1
    oidFontSizeSmall    = -1
    oidResetAll         = -1

    if page == "Settings"
        RenderSettingsPage()
    elseif page == "Maintenance"
        RenderMaintenancePage()
    endif
endevent

; ============================================================================
; Settings page
; ============================================================================

function RenderSettingsPage()

    AddHeaderOption("Diary Volumes")
    oidEntriesPerVolume = AddSliderOption("Entries Per Volume", GetEntriesPerVolume(), "{0}")

    AddHeaderOption("Book Font Sizes")
    oidFontSizeTitle   = AddSliderOption("Title Font Size",   GetFontSizeTitle(),   "{0}")
    oidFontSizeDate    = AddSliderOption("Date Font Size",    GetFontSizeDate(),    "{0}")
    oidFontSizeContent = AddSliderOption("Content Font Size", GetFontSizeContent(), "{0}")
    oidFontSizeSmall   = AddSliderOption("Small Font Size",   GetFontSizeSmall(),   "{0}")

endfunction

; ============================================================================
; Maintenance page
; ============================================================================

function RenderMaintenancePage()

    AddHeaderOption("Diary Maintenance")
    oidResetAll = AddTextOption("Reset All Diaries", "")

    AddHeaderOption("Logging")
    oidDebugLog = AddToggleOption("Debug Logging", GetDebugLog())

endfunction

; ============================================================================
; Text option select  (Reset / Force Rebuild)
; ============================================================================

event OnOptionSelect(int oid)

    if oid == oidDebugLog
        bool newVal = !GetDebugLog()
        SetDebugLog(newVal)
        SetToggleOptionValue(oid, newVal)
    elseif oid == oidResetAll
        bool confirmed = ShowMessage( \
            "Remove all physical diary books from NPCs and clear tracking?\n\n" + \
            "Your SkyrimNet diary entries are NOT affected - books can be regenerated ", \
            true, "Confirm", "Cancel")
        if confirmed
            bool ok = ResetAllDiaries()
            if ok
                ShowMessage("Reset complete.\n\nPlease save and restart the game to fully remove books from unloaded containers.", false, "OK")
            else
                ShowMessage("Reset encountered an error. Check SkyrimNetPhysicalDiaries.log for details.", false, "OK")
            endif
            ForcePageReset()
        endif

    endif

endevent

; ============================================================================
; Slider open  (populate the slider dialog values)
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
; Slider accept  (user confirmed a new value)
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
        SetInfoText("How many diary entries fit in one book volume. Higher number means longer book load time. Range 1-50, default 10.")
    elseif oid == oidDebugLog
        SetInfoText("Write verbose debug information to SkyrimNetPhysicalDiaries.log. Disable for normal use.")
    elseif oid == oidFontSizeTitle
        SetInfoText("Font size for the diary title heading. Range 8-24, default 18.")
    elseif oid == oidFontSizeDate
        SetInfoText("Font size for the date header above each entry. Range 8-24, default 16.")
    elseif oid == oidFontSizeContent
        SetInfoText("Font size for the main diary entry body text. Range 8-24, default 14.")
    elseif oid == oidFontSizeSmall
        SetInfoText("Font size for secondary text such as the date range on the title page. Range 8-24, default 12.")
    elseif oid == oidResetAll
        SetInfoText("Removes all physical diary books from NPC inventories and clear tracking. Your SkyrimNet diary entries are preserved. Save and restart afterwards.")
    endif

endevent

; ============================================================================
; Default reset  (user presses the 'default' button on an option)
; ============================================================================

event OnOptionDefault(int oid)

    if oid == oidEntriesPerVolume
        SetEntriesPerVolume(10)
        SetSliderOptionValue(oid, 10.0, "{0}")
    elseif oid == oidDebugLog
        SetDebugLog(false)
        SetToggleOptionValue(oid, false)
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

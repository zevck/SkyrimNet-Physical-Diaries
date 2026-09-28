Scriptname SkyrimNetDiaries_EventListener extends Quest

; Listens for SkyrimNet_DiaryCreated ModEvent and triggers native diary updates

Event OnInit()
    RegisterForModEvent("SkyrimNet_DiaryCreated", "OnDiaryCreated")
    Debug.Trace("[SkyrimNetDiaries] Registered for SkyrimNet_DiaryCreated ModEvent")
    ; The snpd_diary_stolen decorator is registered natively by the DLL.
EndEvent

Event OnDiaryCreated(string eventName, string strArg, float numArg, Form sender)
    ; strArg is JSON: {"actorFormId": ..., "actorName": "...", "content": "...", ...}
    ; Parsed in C++: actorFormId can exceed Papyrus's signed int range (ESL and high
    ; load-order NPCs), which "as int" would clamp to 0x7FFFFFFF.
    ; The native also clears theft tracking for the actor.
    SkyrimNetDiaries_Native.UpdateDiaryFromEvent(strArg)
EndEvent

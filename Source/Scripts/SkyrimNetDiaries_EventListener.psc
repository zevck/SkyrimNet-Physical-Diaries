Scriptname SkyrimNetDiaries_EventListener extends Quest

; Listens for SkyrimNet_DiaryCreated ModEvent and triggers native diary updates

Event OnInit()
    RegisterForModEvent("SkyrimNet_DiaryCreated", "OnDiaryCreated")
    Debug.Trace("[SkyrimNetDiaries] Registered for SkyrimNet_DiaryCreated ModEvent")
    
    ; Register decorator for SkyrimNet prompts.
    ; NOTE: SkyrimNet resets decorators on every load. Re-registration on subsequent
    ; loads is handled by the C++ plugin (kPostLoadGame → Papyrus VM dispatch).
    SkyrimNetApi.RegisterDecorator("snpd_diary_stolen", "SkyrimNetDiaries_Decorators", "IsDiaryStolen")
    Debug.Trace("[SkyrimNetDiaries] Registered snpd_diary_stolen decorator")
EndEvent

Event OnDiaryCreated(string eventName, string strArg, float numArg, Form sender)
    ; strArg is JSON: {"actorFormId": ..., "actorName": "...", "content": "...", ...}
    ; Parsed in C++: actorFormId can exceed Papyrus's signed int range (ESL and high
    ; load-order NPCs), which "as int" would clamp to 0x7FFFFFFF.
    ; The native also clears theft tracking for the actor.
    SkyrimNetDiaries_Native.UpdateDiaryFromEvent(strArg)
EndEvent

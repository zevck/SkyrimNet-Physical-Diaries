Scriptname SkyrimNetDiaries_Native Hidden

; Updates the diary for the actor in a SkyrimNet_DiaryCreated payload (from EventListener).  Parsed in C++ because
; actorFormId can exceed Papyrus's signed int range (ESL and high load-order NPCs).
Function UpdateDiaryFromEvent(string json) global native

; Legacy: kept for older EventListener scripts.  FormIDs >= 0x80000000 can't be
; passed correctly through a Papyrus int; use UpdateDiaryFromEvent.
Function UpdateDiaryForActor(int formId) global native

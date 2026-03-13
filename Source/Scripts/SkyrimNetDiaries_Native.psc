Scriptname SkyrimNetDiaries_Native Hidden

; Native function to update diary for a specific actor
; Called from EventListener when SkyrimNet_DiaryCreated event fires
Function UpdateDiaryForActor(int formId) global native

; Returns the SNPD_DiaryStolenFaction form so Papyrus can call RemoveFaction directly
Form Function GetStolenFaction() global native

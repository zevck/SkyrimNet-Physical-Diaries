Scriptname SkyrimNetDiaries_API Hidden

; Native theft-status API. All three functions are registered from
; SkyrimNetPhysicalDiaries.dll (see PapyrusAPI.cpp RegisterFunctions).
;
; Public API for other mods.  This declaration file must ship alongside the
; .dll, or the VM cannot bind these natives.

; Returns a JSON blob describing which volumes are currently recorded stolen.
String Function GetDiaryTheftStatus(Actor akActor) Global Native

; Returns "true" or "false" (same answer as the snpd_diary_stolen decorator).
String Function IsDiaryStolen(Actor akActor) Global Native

; Clears every stolen-volume record for the actor.
Function SetTheftCleared(Actor akActor) Global Native

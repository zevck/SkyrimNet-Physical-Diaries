Scriptname SkyrimNetDiaries_API Hidden

; Native theft-status API. All three functions are registered from
; SkyrimNetPhysicalDiaries.dll (see PapyrusAPI.cpp RegisterFunctions).
;
; This declaration file must ship alongside the .dll — without it the VM
; cannot resolve the class, SkyrimNetDiaries_Decorators fails to bind, and
; IsDiaryStolen returns None so the snpd_diary_stolen decorator never sees
; a valid theft state.

; Returns a JSON blob describing which volumes are currently recorded stolen.
String Function GetDiaryTheftStatus(Actor akActor) Global Native

; Returns "true" or "false" — consumed by the snpd_diary_stolen decorator.
String Function IsDiaryStolen(Actor akActor) Global Native

; Clears every stolen-volume record for the actor.
Function SetTheftCleared(Actor akActor) Global Native

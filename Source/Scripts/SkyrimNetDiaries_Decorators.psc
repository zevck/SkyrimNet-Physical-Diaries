Scriptname SkyrimNetDiaries_Decorators

; Decorator function for SkyrimNet prompts to check if diary is stolen
; Can be called from prompts like: {% if snpd_diary_stolen(npc.UUID) == "true" %}
; Returns "true" if diary stolen, "false" otherwise
String Function IsDiaryStolen(Actor akActor) Global
    return SkyrimNetDiaries_API.IsDiaryStolen(akActor)
EndFunction

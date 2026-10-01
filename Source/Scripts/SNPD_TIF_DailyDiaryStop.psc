;BEGIN FRAGMENT CODE - Do not edit anything between this and the end comment
;NEXT FRAGMENT INDEX 1
Scriptname SNPD_TIF_DailyDiaryStop Extends TopicInfo Hidden

;BEGIN FRAGMENT Fragment_0
Function Fragment_0(ObjectReference akSpeakerRef)
Actor akSpeaker = akSpeakerRef as Actor
;BEGIN CODE
; NPCs in this faction write a diary entry every day (docs/NPC_DIARIES.md).
akSpeaker.RemoveFromFaction(SNPD_DailyDiaryFaction)
SkyrimNetDiaries_Native.DailyDiaryChanged(akSpeaker, false)
;END CODE
EndFunction
;END FRAGMENT

;END FRAGMENT CODE - Do not edit anything between this and the begin comment

Faction Property SNPD_DailyDiaryFaction Auto

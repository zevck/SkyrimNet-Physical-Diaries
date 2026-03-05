Scriptname SkyrimNetDiaries_EventListener extends Quest

; Listens for SkyrimNet_DiaryCreated ModEvent and triggers native diary updates

Event OnInit()
    RegisterForModEvent("SkyrimNet_DiaryCreated", "OnDiaryCreated")
    Debug.Trace("[SkyrimNetDiaries] Registered for SkyrimNet_DiaryCreated ModEvent")
EndEvent

Event OnDiaryCreated(string eventName, string strArg, float numArg, Form sender)
    ; strArg contains JSON: {"actorFormId": ..., "actorName": "...", "content": "...", ...}
    ; Parse the JSON to extract FormID
    
    ; Extract FormID from JSON string
    ; Format: "actorFormId": 666772
    int formIdStart = StringUtil.Find(strArg, "\"actorFormId\"")
    if formIdStart >= 0
        ; Find the colon after "actorFormId"
        int colonPos = StringUtil.Find(strArg, ":", formIdStart)
        if colonPos >= 0
            ; Find the next comma or closing brace
            int commaPos = StringUtil.Find(strArg, ",", colonPos)
            int bracePos = StringUtil.Find(strArg, "}", colonPos)
            int endPos = commaPos
            if commaPos < 0 || (bracePos >= 0 && bracePos < commaPos)
                endPos = bracePos
            endIf
            
            if endPos > colonPos
                ; Extract the number between colon and comma/brace
                string formIdStr = StringUtil.Substring(strArg, colonPos + 1, endPos - colonPos - 1)
                
                ; Convert to int (Papyrus handles leading/trailing whitespace automatically)
                int formId = formIdStr as int
                
                if formId > 0
                    Debug.Trace("[SkyrimNetDiaries] Diary created for FormID: " + formId)
                    ; Call native C++ function to handle the update
                    SkyrimNetDiaries_Native.UpdateDiaryForActor(formId)
                else
                    Debug.Trace("[SkyrimNetDiaries] Failed to parse FormID from JSON: " + strArg)
                endIf
            endIf
        endIf
    else
        Debug.Trace("[SkyrimNetDiaries] Event payload missing actorFormId: " + strArg)
    endIf
EndEvent

Scriptname SpeedSyncPlayerAlias extends ReferenceAlias
Import CassiopeiaPapyrusExtender

; ------------------------------------------
; Events
; ------------------------------------------

Event OnPlayerLoadGame()
    ; Reload settings from INI file
    SpeedSyncBridge.RefreshINISettings()

    ; Register input event listener
    RegisterForNativeEvent("SpeedSyncScript", "BSInputEvent")

    ; Instantly clear the C++ tracking state
    SpeedSyncBridge.ClearInternalState()
EndEvent
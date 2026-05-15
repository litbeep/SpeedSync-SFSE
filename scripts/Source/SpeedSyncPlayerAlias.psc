Scriptname SpeedSyncPlayerAlias extends ReferenceAlias
Import CassiopeiaPapyrusExtender

ActorValue Property SpeedSyncPenaltyAV Auto Const Mandatory

; ------------------------------------------
; Events
; ------------------------------------------

Event OnPlayerLoadGame()
    ; Reload settings from INI file
    SpeedSyncBridge.RefreshINISettings()

    ; Register input event listener
    RegisterForNativeEvent("SpeedSyncScript", "BSInputEvent")

    Actor PlayerRef = Game.GetPlayer()
    
    ; If a speed penalty was active on save, pass to C++ to restore
    Float penalty = PlayerRef.GetValue(SpeedSyncPenaltyAV)
    SpeedSyncBridge.ClearInternalState(penalty)
EndEvent
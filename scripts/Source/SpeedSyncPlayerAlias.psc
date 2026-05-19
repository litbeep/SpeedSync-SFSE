Scriptname SpeedSyncPlayerAlias extends ReferenceAlias
Import CassiopeiaPapyrusExtender

ActorValue Property SpeedSyncPenaltyAV Auto Const Mandatory

; ------------------------------------------
; Save Load Handler
; ------------------------------------------
Event OnPlayerLoadGame()
    ; Wait for Cassiopeia's internal event wipe to finish
    Utility.Wait(3.0)

    SpeedSyncBridge.RefreshINISettings()

    ; Delegate to the Quest script so the event binds to the correct instance
    SpeedSyncScript QS = (GetOwningQuest() as SpeedSyncScript)
    If QS
        QS.ScheduleInputRegistration()
    EndIf

    ; Clear C++ state — speed restoration is handled by the permanent task
    Actor PlayerRef = Game.GetPlayer()
    Float penalty = PlayerRef.GetValue(SpeedSyncPenaltyAV)
    SpeedSyncBridge.ClearInternalState(penalty)

    Debug.Notification("SpeedSync ready")
EndEvent
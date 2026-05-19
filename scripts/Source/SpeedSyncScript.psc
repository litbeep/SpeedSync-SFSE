Scriptname SpeedSyncScript extends Quest
Import CassiopeiaPapyrusExtender

; ------------------------------------------
; Properties
; ------------------------------------------
ReferenceAlias Property TargetedNPC Auto Const Mandatory

; ------------------------------------------
; Input Registration
; ------------------------------------------

; Registers the BSInputEvent handler on this Quest script instance.
; Must be called from the Quest context — calling from an Alias
; would bind the event to the wrong script instance.
Function RegisterInput()
    RegisterForNativeEvent("SpeedSyncScript", "BSInputEvent")
EndFunction

; Registers immediately, then queues a delayed retry to survive
; Cassiopeia's async event wipe during quit-to-menu → load cycles.
Function ScheduleInputRegistration()
    RegisterInput()
    Self.CancelTimer(99)
    Self.StartTimer(5.0, 99)
EndFunction

Event OnQuestInit()
    SpeedSyncBridge.RefreshINISettings()
    Utility.Wait(1.0)
    RegisterInput()
    Debug.Notification("SpeedSync ready")
EndEvent

; ------------------------------------------
; Input Handler (Cassiopeia BSInputEvent)
; ------------------------------------------
Function BSInputEvent(Int aiKeyCode, String asControlName, String asFriendlyName, bool bPressed, Float afHeldTime) global
    If SpeedSyncBridge.IsInMenuMode()
        Return
    EndIf

    If bPressed && SpeedSyncBridge.IsSyncHotkey(aiKeyCode)
        SpeedSyncScript QS = (Game.GetFormFromFile(0x00000807, "SpeedSync.esm") as Quest) as SpeedSyncScript
        If QS 
            QS.ToggleFollow()
        EndIf
    EndIf
EndFunction

; ------------------------------------------
; Toggle & Follow Logic
; ------------------------------------------
Function ToggleFollow()
    ; Restart the quest if it was shut down by a cleared alias on load
    If !Self.IsRunning()
        Self.Start()
        Utility.Wait(0.1)
    EndIf

    Actor PlayerRef = Game.GetPlayer()
    Actor currentTarget = SpeedSyncBridge.GetEscortTarget()

    If currentTarget != None
        StopFollowing(true)
    Else
        If PlayerRef.GetSitState() != 0
            Debug.Notification("SpeedSync cannot start while seated or piloting")
            Return
        EndIf

        Actor target = GetCrosshairRef() as Actor
        
        If target && target != PlayerRef && !target.IsDead()
            TargetedNPC.ForceRefTo(target)
            SpeedSyncBridge.SetEscortTarget(target)
            Self.StartTimer(0.5, 10)
            Debug.Notification("SpeedSync locked onto " + GetReferenceName(target))
        Else
            Debug.Notification("SpeedSync found no valid NPC in crosshairs")
        EndIf
    EndIf
EndFunction

Function StopFollowing(bool abNotifyPlayer = false)
    Self.CancelTimer(10)
    TargetedNPC.Clear()
    SpeedSyncBridge.SetEscortTarget(None)
    
    If abNotifyPlayer
        Debug.Notification("SpeedSync stopped")
    EndIf
EndFunction

Function ForceStopFollowing()
    StopFollowing(true)
EndFunction

; ------------------------------------------
; Timers
; ------------------------------------------
Event OnTimer(Int aiTimerID)
    ; Safety-net re-registration after save load
    If aiTimerID == 99
        RegisterInput()
        Return
    EndIf

    ; Watcher loop — polls for break conditions while synced
    If aiTimerID == 10
        Actor PlayerRef = Game.GetPlayer()
        Actor currentTarget = SpeedSyncBridge.GetEscortTarget()
        
        If currentTarget == None
            TargetedNPC.Clear()
            Debug.Notification("SpeedSync stopped (Too Far)")
            Return
        EndIf

        If currentTarget.IsDead()
            TargetedNPC.Clear()
            SpeedSyncBridge.SetEscortTarget(None)
            Debug.Notification("SpeedSync stopped (Target Died)")
            Return 
        EndIf

        If PlayerRef.IsInCombat()
            TargetedNPC.Clear()
            SpeedSyncBridge.SetEscortTarget(None)
            Debug.Notification("SpeedSync stopped (In Combat)")
            Return 
        EndIf

        If PlayerRef.IsSprinting()
            TargetedNPC.Clear()
            SpeedSyncBridge.SetEscortTarget(None)
            Debug.Notification("SpeedSync stopped (Sprinting)")
            Return 
        EndIf

        If PlayerRef.GetSitState() != 0
            TargetedNPC.Clear()
            SpeedSyncBridge.SetEscortTarget(None)
            Debug.Notification("SpeedSync stopped (Piloting/Seated)")
            Return 
        EndIf

        Self.StartTimer(0.5, 10)
    EndIf
EndEvent
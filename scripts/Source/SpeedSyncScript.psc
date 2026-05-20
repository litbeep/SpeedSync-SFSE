Scriptname SpeedSyncScript extends Quest
Import CassiopeiaPapyrusExtender

; ------------------------------------------
; Properties
; ------------------------------------------
ReferenceAlias Property TargetedNPC Auto Const Mandatory
Message Property SpeedSync_Msg_Ready Auto Const Mandatory
Message Property SpeedSync_Msg_Seated Auto Const Mandatory
Message Property SpeedSync_Msg_NoTarget Auto Const Mandatory
Message Property SpeedSync_Msg_Stopped Auto Const Mandatory
Message Property SpeedSync_Msg_TooFar Auto Const Mandatory
Message Property SpeedSync_Msg_TargetDied Auto Const Mandatory
Message Property SpeedSync_Msg_InCombat Auto Const Mandatory
Message Property SpeedSync_Msg_Sprinting Auto Const Mandatory
Message Property SpeedSync_Msg_PilotSeated Auto Const Mandatory

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
    SpeedSync_Msg_Ready.Show()
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
            SpeedSync_Msg_Seated.Show()
            Return
        EndIf

        Actor target = GetCrosshairRef() as Actor
        
        If target && target != PlayerRef && !target.IsDead()
            TargetedNPC.ForceRefTo(target)
            SpeedSyncBridge.SetEscortTarget(target)
            Self.StartTimer(0.5, 10)
            Debug.Notification("SpeedSync locked onto " + GetReferenceName(target))
        Else
            SpeedSync_Msg_NoTarget.Show()
        EndIf
    EndIf
EndFunction

Function StopFollowing(bool abNotifyPlayer = false)
    Self.CancelTimer(10)
    TargetedNPC.Clear()
    SpeedSyncBridge.SetEscortTarget(None)
    
    If abNotifyPlayer
        SpeedSync_Msg_Stopped.Show()
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
            SpeedSync_Msg_TooFar.Show()
            Return
        EndIf

        If currentTarget.IsDead()
            TargetedNPC.Clear()
            SpeedSyncBridge.SetEscortTarget(None)
            SpeedSync_Msg_TargetDied.Show()
            Return 
        EndIf

        If PlayerRef.IsInCombat()
            TargetedNPC.Clear()
            SpeedSyncBridge.SetEscortTarget(None)
            SpeedSync_Msg_InCombat.Show()
            Return 
        EndIf

        If SpeedSyncBridge.GetSprintBreaksSync() && PlayerRef.IsSprinting()
            TargetedNPC.Clear()
            SpeedSyncBridge.SetEscortTarget(None)
            SpeedSync_Msg_Sprinting.Show()
            Return 
        EndIf

        If PlayerRef.GetSitState() != 0
            TargetedNPC.Clear()
            SpeedSyncBridge.SetEscortTarget(None)
            SpeedSync_Msg_PilotSeated.Show()
            Return 
        EndIf

        Self.StartTimer(0.5, 10)
    EndIf
EndEvent
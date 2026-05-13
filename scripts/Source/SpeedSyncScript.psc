Scriptname SpeedSyncScript extends Quest
Import CassiopeiaPapyrusExtender

; ------------------------------------------
; Properties
; ------------------------------------------
ReferenceAlias Property TargetedNPC Auto Const Mandatory

; ------------------------------------------
; Initialization & Input
; ------------------------------------------
Event OnQuestInit()
    ; Read settings on brand new game or initial install
    SpeedSyncBridge.RefreshINISettings()
    
    Utility.Wait(1.0)
    RegisterForNativeEvent("SpeedSyncScript", "BSInputEvent")
EndEvent

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
; Core Logic
; ------------------------------------------
Function ToggleFollow()
    Actor PlayerRef = Game.GetPlayer()
    Actor currentTarget = SpeedSyncBridge.GetEscortTarget() ; Ask C++ for the state

    If currentTarget != None
        ; C++ is already tracking someone. The player wants to cancel it.
        StopFollowing(true)
    Else
        ; Prevent starting while piloting/seated
        If PlayerRef.GetSitState() != 0
            Debug.Notification("Speed Sync: Cannot be used while seated or piloting.")
            Return
        EndIf

        ; C++ is NOT tracking anyone. Let's start the sync.
        Actor target = GetCrosshairRef() as Actor
        
        If target && target != PlayerRef && !target.IsDead()
            TargetedNPC.ForceRefTo(target)
            
            ; 1. Tell C++ to start tracking
            SpeedSyncBridge.SetEscortTarget(target)
            
            ; 2. Start the Papyrus Watcher Loop
            Self.StartTimer(0.5, 10)
            
            Debug.Notification("Speed Sync: Locked onto " + GetReferenceName(target))
        Else
            Debug.Notification("Speed Sync: No valid NPC in crosshairs.")
        EndIf
    EndIf
EndFunction

; ------------------------------------------
; Events
; ------------------------------------------
Event OnTimer(Int aiTimerID)
    If aiTimerID == 10
        Actor PlayerRef = Game.GetPlayer()
        Actor currentTarget = SpeedSyncBridge.GetEscortTarget()
        
        ; 1. Check Distance Break
        If currentTarget == None
            TargetedNPC.Clear()
            Debug.Notification("Speed Sync: Broken (Too Far)")
            Return
        EndIf

        ; 2. Check Target Death
        If currentTarget.IsDead()
            TargetedNPC.Clear()
            SpeedSyncBridge.SetEscortTarget(None)
            Debug.Notification("Speed Sync: Broken (Target Died)")
            Return 
        EndIf

        ; 3. Check Player Combat State
        If PlayerRef.IsInCombat()
            TargetedNPC.Clear()
            SpeedSyncBridge.SetEscortTarget(None)
            Debug.Notification("Speed Sync: Broken (Combat Initiated)")
            Return 
        EndIf

        ; 4. Check Player Sprint State
        If PlayerRef.IsSprinting()
            TargetedNPC.Clear()
            SpeedSyncBridge.SetEscortTarget(None)
            Debug.Notification("Speed Sync: Broken (Sprinting)")
            Return 
        EndIf

        ; 5. Check Seated/Piloting State
        If PlayerRef.GetSitState() != 0
            TargetedNPC.Clear()
            SpeedSyncBridge.SetEscortTarget(None)
            Debug.Notification("Speed Sync: Broken (Piloting / Seated)")
            Return 
        EndIf

        ; 6. Loop Timer
        Self.StartTimer(0.5, 10)
    EndIf
EndEvent

; ------------------------------------------
; Helper Functions
; ------------------------------------------
Function ForceStopFollowing()
    StopFollowing(true)
EndFunction

Function StopFollowing(bool abNotifyPlayer = false)
    ; 1. Kill the Watcher Loop so it stops polling
    Self.CancelTimer(10)
    
    ; 2. Clear the Papyrus Alias
    TargetedNPC.Clear()
    
    ; 3. Tell C++ to clear its target
    SpeedSyncBridge.SetEscortTarget(None)
    
    If abNotifyPlayer
        Debug.Notification("Speed Sync: Disabled.")
    EndIf
EndFunction
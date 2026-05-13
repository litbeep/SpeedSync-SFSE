Scriptname SpeedSyncBridge Native Hidden

; ------------------------------------------
; Core Functions
; ------------------------------------------
Function SetEscortTarget(Actor target) global native
Actor Function GetEscortTarget() global native
Function ClearInternalState(Float savedPenalty) global native
Bool Function IsInMenuMode() global native

; ------------------------------------------
; Configuration & Input
; ------------------------------------------
Function RefreshINISettings() global native
Bool Function IsSyncHotkey(Int aiKeyCode) global native
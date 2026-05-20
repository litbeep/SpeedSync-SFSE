Scriptname SpeedSyncBridge Native Hidden

; Core
Function SetEscortTarget(Actor target) global native
Actor Function GetEscortTarget() global native
Function ClearInternalState(Float savedPenalty) global native
Bool Function IsInMenuMode() global native

; Configuration & Input
Function RefreshINISettings() global native
Bool Function IsSyncHotkey(Int aiKeyCode) global native
Bool Function GetSprintBreaksSync() global native
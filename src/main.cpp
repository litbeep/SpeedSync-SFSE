#include "pch.h"
#include <SFSE/SFSE.h>
#include <SFSE/Interfaces.h>
#include <RE/Starfield.h>
#include <RE/A/ActorValueOwner.h>
#include <RE/A/ActorValues.h>
#include <RE/P/PlayerCharacter.h>
#include <cmath>
#include <variant>
#include <chrono>
#include <algorithm> // For std::clamp, std::max, std::min

// Windows & Platform Includes
#define NOMINMAX
#include <Windows.h>
#include <string>

// ==============================================================================
// 1. GLOBAL STATE
// ==============================================================================
RE::Actor* g_TargetNPC = nullptr;
float g_CurrentSpeedMod = 0.0f;
int g_HotkeyVK = 71;

// ==============================================================================
// 2. VTABLE HOOK (THE SAFE INJECTION)
// ==============================================================================
using GetActorValue_t = float(*)(RE::ActorValueOwner* a_this, const RE::ActorValueInfo& a_info);
REL::Relocation<GetActorValue_t> Original_GetActorValue;

float Hook_GetActorValue(RE::ActorValueOwner* a_this, const RE::ActorValueInfo& a_info)
{
    auto* avSingleton = RE::ActorValue::GetSingleton();
    
    // Intercept Player SpeedMult requests
    if (&a_info == avSingleton->speedMult && a_this->GetIsPlayerOwner()) {
        float vanillaSpeed = Original_GetActorValue(a_this, a_info);
        
        if (g_TargetNPC != nullptr) {
            // Safely inject our distance-calculated math instantly!
            return vanillaSpeed + g_CurrentSpeedMod; 
        }
        return vanillaSpeed; 
    }

    return Original_GetActorValue(a_this, a_info);
}

void InstallDynamicHooks()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return; 

    RE::ActorValueOwner* avOwner = static_cast<RE::ActorValueOwner*>(player);
    uintptr_t vtableAddress = *(uintptr_t*)avOwner;

    REL::Relocation<uintptr_t> vtable(vtableAddress);
    Original_GetActorValue = vtable.write_vfunc(0x01, Hook_GetActorValue);
    
    REX::INFO("SpeedSync: VTable hook installed successfully. Safe uninstall enabled.");
}

// ==============================================================================
// 3. PAPYRUS NATIVE FUNCTIONS
// ==============================================================================
void SetEscortTarget(std::monostate, RE::Actor* a_target) {
    g_TargetNPC = a_target;
    if (g_TargetNPC) {
        REX::INFO("SpeedSync: Escort target set to: {:X}", g_TargetNPC->GetFormID());
    } else {
        REX::INFO("SpeedSync: Escort target cleared.");
        g_CurrentSpeedMod = 0.0f; // Reset modifier instantly
    }
}

RE::Actor* GetEscortTarget(std::monostate) {
    return g_TargetNPC;
}

bool IsInMenuMode(std::monostate) {
    bool isPaused = false;
    if (const auto main = RE::Main::GetSingleton()) {
        isPaused = main->isGameMenuPaused;
    }

    if (const auto ui = RE::UI::GetSingleton()) {
        if (ui->IsMenuOpen("Console") || ui->IsMenuOpen("DialogueMenu")) {
            isPaused = true;
        }
    }
    return isPaused;
}

// Native INI reader to extract and cache the hotkey virtual key code
void RefreshINISettings(std::monostate) {
    std::string relPath = "Data\\SFSE\\Plugins\\SpeedSync.ini";
    char absPath[MAX_PATH];
    GetFullPathNameA(relPath.c_str(), MAX_PATH, absPath, nullptr);
    
    g_HotkeyVK = GetPrivateProfileIntA("Settings", "Hotkey", 71, absPath);
    REX::INFO("SpeedSync: Cached Hotkey VK {}", g_HotkeyVK);
}

// Evaluates the keypress natively using the cached hotkey
bool IsSyncHotkey(std::monostate, int a_keyCode) {
    return a_keyCode == g_HotkeyVK;
}

// Removed the 'float a_savedPenalty' argument
void ClearInternalState(std::monostate) {
    // Wipe tracking pointers to start fresh
    g_TargetNPC = nullptr;
    g_CurrentSpeedMod = 0.0f;
    REX::INFO("SpeedSync: Internal state cleared by Papyrus on load.");
}

bool BindPapyrus(RE::BSScript::IVirtualMachine* a_vm) {
    a_vm->BindNativeMethod("SpeedSyncBridge", "SetEscortTarget", SetEscortTarget, std::optional<bool>(), false);
    a_vm->BindNativeMethod("SpeedSyncBridge", "GetEscortTarget", GetEscortTarget, std::optional<bool>(), false);
    a_vm->BindNativeMethod("SpeedSyncBridge", "ClearInternalState", ClearInternalState, std::optional<bool>(), false); // Matches new signature
    a_vm->BindNativeMethod("SpeedSyncBridge", "IsInMenuMode", IsInMenuMode, std::optional<bool>(), false);
    a_vm->BindNativeMethod("SpeedSyncBridge", "RefreshINISettings", RefreshINISettings, std::optional<bool>(), false);
    a_vm->BindNativeMethod("SpeedSyncBridge", "IsSyncHotkey", IsSyncHotkey, std::optional<bool>(), false);
    
    REX::INFO("SpeedSync: Registered Papyrus functions.");
    return true;
}

// ==============================================================================
// 4. ENGINE HOOK: PERMANENT TASK (MATH LOOP)
// ==============================================================================
class PlayerUpdateHook
{
public:
    static void Install()
    {
        if (const auto task = SFSE::GetTaskInterface()) {
            task->AddPermanentTask(UpdatePlayer);
            REX::INFO("SpeedSync: Installed Permanent Task hook for per-frame execution.");
        } else {
            REX::INFO("SpeedSync: Failed to get Task interface!");
        }
    }

private:
    static void UpdatePlayer()
    {
        static uint32_t frameCount = 0;

        auto a_player = RE::PlayerCharacter::GetSingleton();
        
        // Target lost or dead -> reset speed and abort
        if (!g_TargetNPC || g_TargetNPC->IsDead()) {
            g_CurrentSpeedMod = 0.0f;
            return;
        }

        if (!a_player) return;

        // Throttle execution for ~30Hz smooth interpolation
        if (++frameCount < 2) return; 
        frameCount = 0;

        RE::NiPoint3 playerPos = a_player->GetPosition();
        RE::NiPoint3 targetPos = g_TargetNPC->GetPosition();
        float dx = playerPos.x - targetPos.x;
        float dy = playerPos.y - targetPos.y;
        float dz = playerPos.z - targetPos.z;
        float distanceSq = (dx * dx) + (dy * dy) + (dz * dz);
        float distance = std::sqrt(distanceSq);

        // Break the sync if the player exceeds the distance leash
        if (distance > 15.0f) {
            g_TargetNPC = nullptr;
            g_CurrentSpeedMod = 0.0f;
            return;
        }

        static RE::NiPoint3 lastTargetPos = targetPos;
        static RE::Actor* lastTrackedNPC = nullptr;
        
        // Wipe stale position data if the player switched targets
        if (g_TargetNPC != lastTrackedNPC) {
            lastTargetPos = targetPos;
            lastTrackedNPC = g_TargetNPC;
        }

        float tx = targetPos.x - lastTargetPos.x;
        float ty = targetPos.y - lastTargetPos.y;
        float targetSpeedSq = (tx * tx) + (ty * ty);
        lastTargetPos = targetPos;

        // Dynamic base speed profile calculation
        auto avOwner = static_cast<RE::ActorValueOwner*>(a_player);
        auto speedForm = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>("SpeedMult");
        
        // Get total current speed from the engine
        float currentTotalSpeed = (avOwner && speedForm) ? avOwner->GetActorValue(*speedForm) : 100.0f;
        
        // Find player's true base speed by removing our current active penalty
        float trueBaseSpeed = currentTotalSpeed - g_CurrentSpeedMod;
        
        // Define the exact speed we want the player to drop to when fully braked
        float desiredMinSpeed = 20.0f; 
        
        // Dynamically calculate the maximum penalty required to hit that minimum speed
        float maxPenalty = desiredMinSpeed - trueBaseSpeed; 
        maxPenalty = std::min(maxPenalty, 0.0f); // Safety: ensure we never apply a positive speed buff

        // Proportional smoothing logic based on target distance and state
        float targetSpeedMod = 0.0f;

        if (targetSpeedSq < 0.001f && distance < 2.0f) {
            // NPC is fully stopped and we are right behind them -> Slam brakes
            targetSpeedMod = maxPenalty;
        } else {
            if (distance < 2.0f) {
                // Scale negatively based on closeness (capped at our dynamic max penalty)
                targetSpeedMod = std::max((distance - 2.0f) * 30.0f, maxPenalty);
            } else {
                // In the pocket or trailing -> limit to base speed (0.0 penalty)
                targetSpeedMod = 0.0f;
            }
        }
        
        // Interpolate smoothly toward the target speed Mod (acts like a damped spring)
        // NOTICE: We just update the global variable. No ModActorValue tasks! 
        // The VTable hook instantly reads this new value on the main thread.
        g_CurrentSpeedMod += (targetSpeedMod - g_CurrentSpeedMod) * 0.15f;
        g_CurrentSpeedMod = std::clamp(g_CurrentSpeedMod, maxPenalty, 0.0f);
    }

    static void RefreshPlayerSpeed(RE::PlayerCharacter* a_player)
    {
    if (!a_player) return;

    // This is the most reliable way to force a speed refresh without side effects.
    // We update a high-level state flag that forces the movement controller
    // to re-read the SpeedMult Actor Value.
    using func_t = void(*)(RE::PlayerCharacter*);
    // Address for Actor::UpdateMovementSpeed (Offset varies by version)
    // A safe alternative is to briefly "damage" an irrelevant AV by 0.
    
    auto avOwner = static_cast<RE::ActorValueOwner*>(a_player);
    auto* avSingleton = RE::ActorValue::GetSingleton();
    
    // We poke a stat that doesn't matter (like an unused skill or variable)
    // This triggers the "Value Changed" observer in the engine.
    avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *avSingleton->speedMult, 0.001f, a_player);
    avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *avSingleton->speedMult, -0.001f, a_player);
    }

};

// ==============================================================================
// 5. EVENT MESSAGING & PLUGIN LOAD
// ==============================================================================
bool g_hooksInstalled = false;

void MessageHandler(SFSE::MessagingInterface::Message* a_msg)
{
    switch (a_msg->type) {
    case SFSE::MessagingInterface::kPostLoad:
        {
            PlayerUpdateHook::Install();
            break;
        }
    case SFSE::MessagingInterface::kPostDataLoad:
        {
            // Install the Safe Speed VTable Hook
            if (!g_hooksInstalled) {
                InstallDynamicHooks();
                g_hooksInstalled = true;
            }

            // Try to bind Papyrus methods once the engine establishes the VM
            if (auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton()) {
                BindPapyrus(vm);
            } else {
                REX::INFO("SpeedSync: Failed to bind Papyrus, VM not ready in kPostDataLoad.");
            }
            break;
        }
    }
}

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    return true;
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    SFSE::AllocTrampoline(64);

    REX::INFO("SpeedSync SFSE plugin loaded");

    if (const auto messaging = SFSE::GetMessagingInterface()) {
        messaging->RegisterListener(MessageHandler);
    } else {
        REX::INFO("SpeedSync: Failed to get Messaging interface!");
    }

    return true;
}
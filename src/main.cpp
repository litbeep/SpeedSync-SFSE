#include "pch.h"
#include <SFSE/SFSE.h>
#include <SFSE/Interfaces.h>
#include <RE/Starfield.h>
#include <cmath>
#include <variant>
#include <chrono>
#include <algorithm> // For std::clamp, std::max, std::min

// Windows & Platform Includes
#define NOMINMAX
#include <Windows.h>
#include <string>

// Global State
RE::Actor* g_TargetNPC = nullptr;
float g_CurrentSpeedMod = 0.0f;
float g_LastPushedSpeedMod = 0.0f;
int g_HotkeyVK = 71;

// Papyrus Native Functions
void SetEscortTarget(std::monostate, RE::Actor* a_target) {
    g_TargetNPC = a_target;
    if (g_TargetNPC) {
        REX::INFO("Escort target set to: {:X}", g_TargetNPC->GetFormID());
    } else {
        REX::INFO("Escort target cleared.");
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
        // Fallback checks for menus that may not strictly pause the background game 
        // world but should still be treated as "Menu Mode" for hotkeys/logic.
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

// Handles cleanup on load, and restores the SpeedMult via the Shadow AV penalty float
void ClearInternalState(std::monostate, float a_savedPenalty) {
    if (std::abs(a_savedPenalty) > 0.001f) {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            auto avOwner = static_cast<RE::ActorValueOwner*>(player);
            auto speedForm = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>("SpeedMult");
            auto penaltyForm = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>("SpeedSync_Penalty_AV");

            if (avOwner && speedForm && penaltyForm) {
                // Add the positive penalty back to SpeedMult to restore normal speed
                avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *speedForm, a_savedPenalty, player);
                // Zero out the Penalty AV by subtracting itself
                avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *penaltyForm, -a_savedPenalty, player);
                REX::INFO("SpeedSync: Reverted a loaded speed penalty of {}", a_savedPenalty);
            }
        }
    }

    // Wipe tracking pointers to start fresh
    g_TargetNPC = nullptr;
    g_CurrentSpeedMod = 0.0f;
    g_LastPushedSpeedMod = 0.0f;
    REX::INFO("SpeedSync internal state cleared by Papyrus on load.");
}

bool BindPapyrus(RE::BSScript::IVirtualMachine* a_vm) {
    a_vm->BindNativeMethod("SpeedSyncBridge", "SetEscortTarget", SetEscortTarget, std::optional<bool>(), false);
    a_vm->BindNativeMethod("SpeedSyncBridge", "GetEscortTarget", GetEscortTarget, std::optional<bool>(), false);
    a_vm->BindNativeMethod("SpeedSyncBridge", "ClearInternalState", ClearInternalState, std::optional<bool>(), false);
    a_vm->BindNativeMethod("SpeedSyncBridge", "IsInMenuMode", IsInMenuMode, std::optional<bool>(), false);
    
    a_vm->BindNativeMethod("SpeedSyncBridge", "RefreshINISettings", RefreshINISettings, std::optional<bool>(), false);
    a_vm->BindNativeMethod("SpeedSyncBridge", "IsSyncHotkey", IsSyncHotkey, std::optional<bool>(), false);
    
    REX::INFO("Registered Papyrus functions.");
    return true;
}

// 3. Engine Hook: Frame Update
class PlayerUpdateHook
{
public:
    static void Install()
    {
        if (const auto task = SFSE::GetTaskInterface()) {
            task->AddPermanentTask(UpdatePlayer);
            REX::INFO("Installed Permanent Task hook for per-frame execution.");
        } else {
            REX::INFO("Failed to get Task interface!");
        }
    }

private:
    static void UpdatePlayer()
    {
        static uint32_t frameCount = 0;

        auto a_player = RE::PlayerCharacter::GetSingleton();
        
        // Ensure speed is reverted properly using dual-AV logic if the target is invalid
        if (!g_TargetNPC || g_TargetNPC->IsDead()) {
            if (std::abs(g_LastPushedSpeedMod) > 0.001f) {
                float diff = -g_LastPushedSpeedMod; // Invert to restore base speed
                if (const auto task = SFSE::GetTaskInterface()) {
                    task->AddTask([diff]() {
                        auto player = RE::PlayerCharacter::GetSingleton();
                        if (!player) return;
                        auto avOwner = static_cast<RE::ActorValueOwner*>(player);
                        auto speedForm = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>("SpeedMult");
                        auto penaltyForm = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>("SpeedSync_Penalty_AV");
                        
                        if (avOwner && speedForm && penaltyForm) {
                            avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *speedForm, diff, player);
                            avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *penaltyForm, -diff, player);
                        }
                    });
                }
                g_LastPushedSpeedMod = 0.0f;
                g_CurrentSpeedMod = 0.0f;
            }
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
        
        // Get total current speed; default to 100.0 if lookup fails
        float currentTotalSpeed = (avOwner && speedForm) ? avOwner->GetActorValue(*speedForm) : 100.0f;
        
        // Find player's true speed by removing our current active penalty
        float trueBaseSpeed = currentTotalSpeed - g_CurrentSpeedMod;
        
        // Define the exact speed we want the player to drop to when fully braked
        float desiredMinSpeed = 20.0f; 
        
        // Dynamically calculate the maximum penalty required to hit that minimum speed
        float maxPenalty = desiredMinSpeed - trueBaseSpeed; 
        maxPenalty = std::min(maxPenalty, 0.0f); // Safety: ensure we never apply a positive speed buff

        // Proportional smoothing logic based on target distance and state
        float targetSpeedMod = 0.0f;

        if (targetSpeedSq < 0.001f && distance < 2.0f) {
            // NPC is fully stopped and we are right behind them -> Slam brakes to dynamic penalty
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
        g_CurrentSpeedMod += (targetSpeedMod - g_CurrentSpeedMod) * 0.15f;
        g_CurrentSpeedMod = std::clamp(g_CurrentSpeedMod, maxPenalty, 0.0f);

        // Throttle engine updates by applying dual-AV logic only when the diff is > 0.5%
        if (std::abs(g_CurrentSpeedMod - g_LastPushedSpeedMod) > 0.5f) {
            float diff = g_CurrentSpeedMod - g_LastPushedSpeedMod;
            if (const auto task = SFSE::GetTaskInterface()) {
                task->AddTask([diff]() {
                    auto player = RE::PlayerCharacter::GetSingleton();
                    if (!player) return;
                    auto avOwner = static_cast<RE::ActorValueOwner*>(player);
                    auto speedForm = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>("SpeedMult");
                    auto penaltyForm = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>("SpeedSync_Penalty_AV");
                    
                    if (avOwner && speedForm && penaltyForm) {
                        // Mod actual speed
                        avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *speedForm, diff, player);
                        // Inverse diff to perfectly track the penalty for save games
                        avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *penaltyForm, -diff, player);
                    }
                });
            }
            g_LastPushedSpeedMod = g_CurrentSpeedMod;
        }
    }
};

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
            // Try to bind Papyrus methods once the engine establishes the VM
            if (auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton()) {
                BindPapyrus(vm);
            } else {
                REX::INFO("Failed to bind Papyrus, VM not ready in kPostDataLoad.");
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
    REX::INFO("SpeedSyncSFSE plugin loaded");

    // Register Messaging interface for Trampoline hooking and Papyrus initialization post-load
    if (const auto messaging = SFSE::GetMessagingInterface()) {
        messaging->RegisterListener(MessageHandler);
    } else {
        REX::INFO("Failed to get Messaging interface!");
    }

    return true;
}
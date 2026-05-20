#include "pch.h"
#include <SFSE/SFSE.h>
#include <SFSE/Interfaces.h>
#include <RE/Starfield.h>
#include <cmath>
#include <variant>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstdint>

#define NOMINMAX
#include <Windows.h>
#include <string>

// ==============================================================================
// 1. Global State
// ==============================================================================
std::atomic<uint32_t> g_TargetNPC{0};
std::atomic<uint32_t> g_PenaltyFormID{0};

std::atomic<float> g_CurrentSpeedMod{0.0f};
std::atomic<float> g_LastPushedSpeedMod{0.0f};
std::atomic<bool> g_IsRestoringPenalty{false};
std::atomic<bool> g_EnableCatchUp{false};
std::atomic<bool> g_SprintBreaksSync{true};
int g_HotkeyVK = 78;

// Tracks which VM instance we last bound to. When the player quits to menu
// and loads a save, the VM is rebuilt but kPostDataLoad won't fire again.
std::atomic<RE::BSScript::IVirtualMachine*> g_LastBoundVM{nullptr};

// ==============================================================================
// 2. Form Lookups
// ==============================================================================
// All accessors resolve fresh each call — no form pointers are cached across
// frames. Forms are invalidated on game reload (load-order changes, ForceReset).

RE::ActorValueInfo* GetSpeedForm() {
    if (const auto avs = RE::ActorValue::GetSingleton()) {
        return avs->speedMult;
    }
    return nullptr;
}

RE::ActorValueInfo* GetPenaltyForm() {
    uint32_t currentID = g_PenaltyFormID.load();
    RE::ActorValueInfo* form = nullptr;

    if (currentID != 0) {
        form = RE::TESForm::LookupByID<RE::ActorValueInfo>(currentID);
    }

    // Fallback: resolve by EditorID and cache the FormID for subsequent lookups
    if (!form) {
        form = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>("SpeedSyncPenaltyAV");
        if (form) {
            g_PenaltyFormID.store(form->GetFormID());
        }
    }
    
    return form;
}

// ==============================================================================
// 3. Papyrus Native Functions
// ==============================================================================
void SetEscortTarget(std::monostate, RE::Actor* a_target) {
    if (a_target) {
        g_TargetNPC.store(a_target->GetFormID());
        REX::INFO("SpeedSync: Escort target set to {:X}", a_target->GetFormID());
    } else {
        g_TargetNPC.store(0);
        REX::INFO("SpeedSync: Escort target cleared");
    }
}

RE::Actor* GetEscortTarget(std::monostate) {
    uint32_t id = g_TargetNPC.load();
    if (id != 0) {
        return RE::TESForm::LookupByID<RE::Actor>(id);
    }
    return nullptr;
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

void RefreshINISettings(std::monostate) {
    std::string relPath = "Data\\SFSE\\Plugins\\SpeedSync.ini";
    char absPath[MAX_PATH];
    GetFullPathNameA(relPath.c_str(), MAX_PATH, absPath, nullptr);
    
    g_HotkeyVK = GetPrivateProfileIntA("Settings", "Hotkey", 78, absPath);
    g_EnableCatchUp.store(GetPrivateProfileIntA("Settings", "EnableCatchUp", 0, absPath) != 0);
    g_SprintBreaksSync.store(GetPrivateProfileIntA("Settings", "SprintBreaksSync", 1, absPath) != 0);
    REX::INFO("SpeedSync: Hotkey VK = {}, EnableCatchUp = {}, SprintBreaksSync = {}", g_HotkeyVK, g_EnableCatchUp.load(), g_SprintBreaksSync.load());
}

bool IsSyncHotkey(std::monostate, int a_keyCode) {
    return a_keyCode == g_HotkeyVK;
}

bool GetSprintBreaksSync(std::monostate) {
    return g_SprintBreaksSync.load();
}

void ClearInternalState(std::monostate, float) {
    // Speed restoration is handled by the permanent task's self-healing logic
    g_TargetNPC.store(0);
    g_CurrentSpeedMod.store(0.0f);
    g_LastPushedSpeedMod.store(0.0f);
}

bool BindPapyrus(RE::BSScript::IVirtualMachine* a_vm) {
    a_vm->BindNativeMethod("SpeedSyncBridge", "SetEscortTarget", SetEscortTarget, std::optional<bool>(), false);
    a_vm->BindNativeMethod("SpeedSyncBridge", "GetEscortTarget", GetEscortTarget, std::optional<bool>(), false);
    a_vm->BindNativeMethod("SpeedSyncBridge", "ClearInternalState", ClearInternalState, std::optional<bool>(), false);
    a_vm->BindNativeMethod("SpeedSyncBridge", "IsInMenuMode", IsInMenuMode, std::optional<bool>(), false);
    a_vm->BindNativeMethod("SpeedSyncBridge", "RefreshINISettings", RefreshINISettings, std::optional<bool>(), false);
    a_vm->BindNativeMethod("SpeedSyncBridge", "IsSyncHotkey", IsSyncHotkey, std::optional<bool>(), false);
    a_vm->BindNativeMethod("SpeedSyncBridge", "GetSprintBreaksSync", GetSprintBreaksSync, std::optional<bool>(), false);
    
    g_LastBoundVM.store(a_vm);
    REX::INFO("SpeedSync: Papyrus native functions bound");
    return true;
}

// Detects VM rebuilds (quit-to-menu → load) and re-binds native functions
void EnsurePapyrusBound() {
    auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    if (vm && vm != g_LastBoundVM.load()) {
        REX::INFO("SpeedSync: Detected new VM, re-binding native functions");
        BindPapyrus(vm);
        
        // Forms are invalidated on reload — force fresh EditorID lookup
        g_PenaltyFormID.store(0);
    }
}

// ==============================================================================
// 4. Per-Frame Update Task
// ==============================================================================
class PlayerUpdateHook
{
public:
    static void Install()
    {
        if (const auto task = SFSE::GetTaskInterface()) {
            task->AddPermanentTask(UpdatePlayer);
            REX::INFO("SpeedSync: Permanent task installed");
        } else {
            REX::INFO("SpeedSync: Failed to get Task interface!");
        }
    }

private:
    static void UpdatePlayer()
    {
        static uint32_t frameCount = 0;

        // -- Safety Gates --

        // Halt during loading/fade transitions
        if (const auto ui = RE::UI::GetSingleton()) {
            if (ui->IsMenuOpen("LoadingMenu") || ui->IsMenuOpen("FaderMenu")) {
                g_TargetNPC.store(0);
                g_CurrentSpeedMod.store(0.0f);
                g_LastPushedSpeedMod.store(0.0f);
                return;
            }
        }

        auto a_player = RE::PlayerCharacter::GetSingleton();
        
        // No player = not in gameplay (main menu, etc.)
        if (!a_player) {
            g_TargetNPC.store(0);
            g_CurrentSpeedMod.store(0.0f);
            g_LastPushedSpeedMod.store(0.0f);
            return;
        }
        
        // Re-bind native functions if the VM was rebuilt
        EnsurePapyrusBound();

        // Pause during game menus (inventory, map) without wiping state
        if (const auto main = RE::Main::GetSingleton(); main && main->isGameMenuPaused) return;

        // -- Core Logic --
        uint32_t targetID = g_TargetNPC.load();
        RE::Actor* currentTarget = (targetID != 0) ? RE::TESForm::LookupByID<RE::Actor>(targetID) : nullptr;
        
        auto avOwner = static_cast<RE::ActorValueOwner*>(a_player);
        auto speedForm = GetSpeedForm();
        auto penaltyForm = GetPenaltyForm();

        // -- Self-Healing --
        // Reverts speed if target was lost or a save loaded with an orphaned penalty
        if (!currentTarget || currentTarget->IsDead()) {
            
            float lastPushed = g_LastPushedSpeedMod.load();
            
            // Active session: undo the last speed modification we applied
            if (std::abs(lastPushed) > 0.001f) {
                float diff = -lastPushed;
                if (const auto task = SFSE::GetTaskInterface()) {
                    task->AddTask([diff]() {
                        auto player = RE::PlayerCharacter::GetSingleton();
                        if (!player) return;
                        auto avO = static_cast<RE::ActorValueOwner*>(player);
                        auto sF = GetSpeedForm();
                        auto pF = GetPenaltyForm();
                        
                        if (avO && sF && pF) {
                            avO->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, *sF, diff, player);
                            avO->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, *pF, -diff, player);
                        }
                    });
                }
                g_LastPushedSpeedMod.store(0.0f);
                g_CurrentSpeedMod.store(0.0f);
            }
            // Orphaned penalty from a save file: invert and clear it
            else if (avOwner && penaltyForm && speedForm) {
                float orphanedPenalty = avOwner->GetActorValue(*penaltyForm);
                if (std::abs(orphanedPenalty) > 0.001f && !g_IsRestoringPenalty.load()) {
                    g_IsRestoringPenalty.store(true);
                    
                    if (const auto task = SFSE::GetTaskInterface()) {
                        task->AddTask([orphanedPenalty]() {
                            auto player = RE::PlayerCharacter::GetSingleton();
                            if (player) {
                                auto avO = static_cast<RE::ActorValueOwner*>(player);
                                auto sF = GetSpeedForm();
                                auto pF = GetPenaltyForm();
                                if (avO && sF && pF) {
                                    avO->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, *sF, orphanedPenalty, player);
                                    avO->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, *pF, -orphanedPenalty, player);
                                }
                            }
                            g_IsRestoringPenalty.store(false);
                        });
                    }
                }
            }
            
            g_TargetNPC.store(0);
            return;
        }

        // -- Follow Speed Math (runs every 2nd frame) --
        if (++frameCount < 2) return; 
        frameCount = 0;

        RE::NiPoint3 playerPos = a_player->GetPosition();
        RE::NiPoint3 targetPos = currentTarget->GetPosition();

        float dx = playerPos.x - targetPos.x;
        float dy = playerPos.y - targetPos.y;
        float dz = playerPos.z - targetPos.z;
        float distanceSq = (dx * dx) + (dy * dy) + (dz * dz);

        // Break sync if too far (increased leash distance to 25.0m)
        if (distanceSq > 625.0f) {
            g_TargetNPC.store(0);
            return;
        }

        float distance = std::sqrt(distanceSq);

        // Track target movement delta
        static RE::NiPoint3 lastTargetPos = targetPos;
        static uint32_t lastTrackedNPC = 0;
        
        if (targetID != lastTrackedNPC) {
            lastTargetPos = targetPos;
            lastTrackedNPC = targetID;
        }

        float tx = targetPos.x - lastTargetPos.x;
        float ty = targetPos.y - lastTargetPos.y;
        float targetSpeedSq = (tx * tx) + (ty * ty);
        lastTargetPos = targetPos;

        // Calculate speed modifier
        float currentMod = g_CurrentSpeedMod.load();
        float currentTotalSpeed = (avOwner && speedForm) ? avOwner->GetActorValue(*speedForm) : 100.0f;
        float currentEnginePenalty = (avOwner && penaltyForm) ? avOwner->GetActorValue(*penaltyForm) : 0.0f;
        float trueBaseSpeed = currentTotalSpeed + currentEnginePenalty;
        
        float desiredMinSpeed = 20.0f; 
        float maxPenalty = desiredMinSpeed - trueBaseSpeed; 
        maxPenalty = std::min(maxPenalty, 0.0f); 

        float maxBonus = g_EnableCatchUp.load() ? 30.0f : 0.0f;
        float targetSpeedMod = 0.0f;

        if (targetSpeedSq < 0.001f && distance < 2.0f) {
            targetSpeedMod = maxPenalty;
        } else {
            if (distance < 2.0f) {
                targetSpeedMod = std::max((distance - 2.0f) * 30.0f, maxPenalty);
            } else {
                // Slope: hits 30% bonus at 15.0m distance -> 30.0 / (15.0 - 2.0) = 30.0 / 13.0
                float catchUpBonus = (distance - 2.0f) * (30.0f / 13.0f);
                targetSpeedMod = std::min(catchUpBonus, maxBonus);
            }
        }
        
        // Smooth interpolation
        currentMod += (targetSpeedMod - currentMod) * 0.15f;
        currentMod = std::clamp(currentMod, maxPenalty, maxBonus);

        g_CurrentSpeedMod.store(currentMod);

        // Push to engine when change exceeds threshold
        float lastPushed = g_LastPushedSpeedMod.load();
        if (std::abs(currentMod - lastPushed) > 0.5f) {
            float diff = currentMod - lastPushed;
            if (const auto task = SFSE::GetTaskInterface()) {
                task->AddTask([diff]() {
                    auto player = RE::PlayerCharacter::GetSingleton();
                    if (!player) return;
                    auto avO = static_cast<RE::ActorValueOwner*>(player);
                    auto sForm = GetSpeedForm();
                    auto pForm = GetPenaltyForm();
                    
                    if (avO && sForm && pForm) {
                        avO->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, *sForm, diff, player);
                        avO->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, *pForm, -diff, player);
                    }
                });
            }
            g_LastPushedSpeedMod.store(currentMod);
        }
    }
};

// ==============================================================================
// 5. Plugin Entry Points
// ==============================================================================
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
            if (auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton()) {
                BindPapyrus(vm);
            } else {
                REX::INFO("SpeedSync: VM not ready in kPostDataLoad");
            }
            
            // Pre-cache the PenaltyAV FormID for faster lookups
            if (auto pForm = RE::TESForm::LookupByEditorID<RE::ActorValueInfo>("SpeedSyncPenaltyAV")) {
                g_PenaltyFormID.store(pForm->GetFormID());
                REX::INFO("SpeedSync: Pre-cached PenaltyAV FormID");
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

#pragma warning(push)
#pragma warning(disable: 4996)
SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    
    REX::INFO("SpeedSync SFSE plugin loaded");

    if (const auto messaging = SFSE::GetMessagingInterface()) {
        messaging->RegisterListener(MessageHandler);
    } else {
        REX::INFO("SpeedSync: Failed to get Messaging interface!");
    }

    return true;
}
#pragma warning(pop)
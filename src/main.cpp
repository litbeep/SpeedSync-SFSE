#include "pch.h"
#include <chrono>

#include "RE/A/ActorValueOwner.h"
#include "RE/A/ActorValues.h"
#include "RE/P/PlayerCharacter.h"
#include "SFSE/SFSE.h"

// 1. ORIGINAL FUNCTION POINTER
using GetActorValue_t = float(*)(RE::ActorValueOwner* a_this, const RE::ActorValueInfo& a_info);
REL::Relocation<GetActorValue_t> Original_GetActorValue;

// 2. THE HOOKED FUNCTION
float Hook_GetActorValue(RE::ActorValueOwner* a_this, const RE::ActorValueInfo& a_info)
{
    auto* avSingleton = RE::ActorValue::GetSingleton();
    
    if (&a_info == avSingleton->speedMult && a_this->GetIsPlayerOwner()) {
        
        // --------------------------------------------------------
        // SPEEDSYNC DYNAMIC LOGIC GOES HERE
        // --------------------------------------------------------
        float speedSyncValue = 150.0f; // TODO: Replace with dynamic calculation
        
        // Optional Debug Throttle (Logging removed temporarily for clean compile)
        /*
        static auto lastLogTime = std::chrono::steady_clock::now();
        auto currentTime = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastLogTime).count() >= 2) {
            // TODO: Add logging here once your logger is set up
            lastLogTime = currentTime;
        }
        */
        
        return speedSyncValue; 
    }

    return Original_GetActorValue(a_this, a_info);
}

// 3. DYNAMIC HOOK INSTALLATION
void InstallDynamicHooks()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return; // Player not found, abort safely
    }

    // Cast the player to the ActorValueOwner interface
    RE::ActorValueOwner* avOwner = static_cast<RE::ActorValueOwner*>(player);

    // Read the actual VTable address from the object instance
    uintptr_t vtableAddress = *(uintptr_t*)avOwner;

    // Create our Relocation object and inject at index 0x01
    REL::Relocation<uintptr_t> vtable(vtableAddress);
    Original_GetActorValue = vtable.write_vfunc(0x01, Hook_GetActorValue);
}

// Ensure we only install the hook once
bool g_hooksInstalled = false;

// 4. EVENT LISTENER
void OnMessage(SFSE::MessagingInterface::Message* a_msg)
{
    // Using the exact enum defined in your Interfaces.h
    if (a_msg->type == SFSE::MessagingInterface::kPostDataLoad && !g_hooksInstalled) {
        InstallDynamicHooks();
        g_hooksInstalled = true;
    }
}

// 5. MAIN PLUGIN ENTRY POINT 
// Using the exact macro defined at the bottom of your Interfaces.h
SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    
    // Allocate Trampoline memory (You can safely ignore the C4996 compiler warning here)
    SFSE::AllocTrampoline(64);
    
    auto messaging = SFSE::GetMessagingInterface();
    if (messaging) {
        messaging->RegisterListener(OnMessage);
    }
    
    return true;
}
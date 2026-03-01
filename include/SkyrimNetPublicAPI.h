#pragma once
#include <string>
#include <windows.h>
#include <functional>

/**
 * SkyrimNet Public API — loaded at runtime via LoadLibraryA + GetProcAddress.
 *
 * LINKING REQUIREMENT: Several functions return std::string or accept
 * std::string / std::function parameters.  This is safe ONLY when the
 * consuming DLL is built with the same MSVC toolset version and CRT
 * linkage (dynamic /MD) as SkyrimNet.  Mixing CRT versions will cause
 * heap corruption on string destruction.
 *
 * For cross-toolset safety, prefer the const char* overloads where available.
 */
extern "C" {

// ---- v2+: Action registration ----
bool (*PublicRegisterCPPAction)(const std::string name, const std::string description, std::function<bool(RE::Actor *)> eligibleCallback,
                                std::function<bool(RE::Actor *, std::string json_params)> executeCallback,
                                const std::string triggeringEventTypesCSV, std::string categoryStr, int priority,
                                std::string parameterSchemaJSON, std::string customCategory, std::string customParentCategory,
                                std::string tagsCSV) = nullptr;
bool (*PublicRegisterCPPSubCategory)(const std::string name, const std::string description,
                                     std::function<bool(RE::Actor *)> eligibleCallback, const std::string triggeringEventTypesCSV,
                                     int priority, std::string parameterSchemaJSON, std::string customCategory,
                                     std::string customParentCategory, std::string tagsCSV) = nullptr;
int (*PublicGetVersion)() = nullptr;

// ---- v3+: UUID Resolution ----
uint64_t (*PublicFormIDToUUID)(uint32_t formId) = nullptr;
uint32_t (*PublicUUIDToFormID)(uint64_t uuid) = nullptr;
std::string (*PublicGetActorNameByUUID)(uint64_t uuid) = nullptr;

// ---- v3+: Bio template lookup ----
std::string (*PublicGetBioTemplateName)(uint32_t formId) = nullptr;

// ---- v3+: Data API ----

/** Retrieve memories for an actor. contextQuery enables semantic search if non-empty. */
std::string (*PublicGetMemoriesForActor)(uint32_t formId, int maxCount, const char* contextQuery) = nullptr;

/** Retrieve recent events, optionally filtered by actor and event type. */
std::string (*PublicGetRecentEvents)(uint32_t formId, int maxCount, const char* eventTypeFilter) = nullptr;

/** Retrieve recent dialogue between the player and a specific NPC. */
std::string (*PublicGetRecentDialogue)(uint32_t formId, int maxExchanges) = nullptr;

/** Get the most recent NPC who spoke to the player. */
std::string (*PublicGetLatestDialogueInfo)() = nullptr;

/** Check if the memory/database system is ready. */
bool (*PublicIsMemorySystemReady)() = nullptr;

/** Get per-actor engagement statistics (memory + event activity) for caller-side scoring. */
std::string (*PublicGetActorEngagement)(int maxCount, bool excludePlayer, bool playerEventsOnly, double shortWindowSeconds, double mediumWindowSeconds) = nullptr;

/** Get actors related to a given actor via shared event history. */
std::string (*PublicGetRelatedActors)(uint32_t formId, int maxCount, double shortWindowSeconds, double mediumWindowSeconds) = nullptr;

/** Get comprehensive player context: DB time, recent interactions, relationships. */
std::string (*PublicGetPlayerContext)(float withinGameHours) = nullptr;

/** Get NPC-to-NPC event pair counts within a candidate pool. */
std::string (*PublicGetEventPairCounts)(const char* formIdListCSV, int minSharedEvents) = nullptr;

// ---- v4+: Diary API ----

/** Retrieve diary entries for an actor, optionally filtered by time range. */
std::string (*PublicGetDiaryEntries)(uint32_t formId, int maxCount, double startTime, double endTime) = nullptr;

// ---- Plugin Configuration API ----

/** Get the full JSON config for a registered plugin. */
std::string (*PublicGetPluginConfig)(const char* pluginName) = nullptr;

/** Get a single string config value by dot-path from a plugin's settings. */
std::string (*PublicGetPluginConfigValue)(const char* pluginName, const char* path, const char* defaultValue) = nullptr;

inline bool FindFunctions() {
    auto hDLL = LoadLibraryA("SkyrimNet");
    if (hDLL != nullptr) {
        PublicGetVersion = (int (*)()) GetProcAddress(hDLL, "PublicGetVersion");
        if (PublicGetVersion != nullptr) {
            int version = PublicGetVersion();

            // v2+ functions
            if (version >= 2) {
                PublicRegisterCPPAction =
                    (bool (*)(const std::string name, const std::string description, std::function<bool(RE::Actor *)> eligibleCallback,
                    std::function<bool(RE::Actor *, std::string json_params)> executeCallback, const std::string triggeringEventTypesCSV,
                              std::string categoryStr, int priority, std::string parameterSchemaJSON, std::string customCategory,
                              std::string customParentCategory, std::string tagsCSV)) GetProcAddress(hDLL, "PublicRegisterCPPAction");
                PublicRegisterCPPSubCategory = (bool (*)(
                    const std::string name, const std::string description, std::function<bool(RE::Actor *)> eligibleCallback,
                    const std::string triggeringEventTypesCSV, int priority, std::string parameterSchemaJSON, std::string customCategory,
                    std::string customParentCategory, std::string tagsCSV)) GetProcAddress(hDLL, "PublicRegisterCPPSubCategory");
            }

            // v3+ functions
            if (version >= 3) {
                // UUID resolution
                PublicFormIDToUUID = reinterpret_cast<uint64_t(*)(uint32_t)>(
                    GetProcAddress(hDLL, "PublicFormIDToUUID"));
                PublicUUIDToFormID = reinterpret_cast<uint32_t(*)(uint64_t)>(
                    GetProcAddress(hDLL, "PublicUUIDToFormID"));
                PublicGetActorNameByUUID = reinterpret_cast<std::string(*)(uint64_t)>(
                    GetProcAddress(hDLL, "PublicGetActorNameByUUID"));

                // Bio template
                PublicGetBioTemplateName = reinterpret_cast<std::string(*)(uint32_t)>(
                    GetProcAddress(hDLL, "PublicGetBioTemplateName"));

                // Data API
                PublicGetMemoriesForActor = reinterpret_cast<std::string(*)(uint32_t, int, const char*)>(
                    GetProcAddress(hDLL, "PublicGetMemoriesForActor"));
                PublicGetRecentEvents = reinterpret_cast<std::string(*)(uint32_t, int, const char*)>(
                    GetProcAddress(hDLL, "PublicGetRecentEvents"));
                PublicGetRecentDialogue = reinterpret_cast<std::string(*)(uint32_t, int)>(
                    GetProcAddress(hDLL, "PublicGetRecentDialogue"));
                PublicGetLatestDialogueInfo = reinterpret_cast<std::string(*)()>(
                    GetProcAddress(hDLL, "PublicGetLatestDialogueInfo"));
                PublicIsMemorySystemReady = reinterpret_cast<bool(*)()>(
                    GetProcAddress(hDLL, "PublicIsMemorySystemReady"));
                PublicGetActorEngagement = reinterpret_cast<std::string(*)(int, bool, bool, double, double)>(
                    GetProcAddress(hDLL, "PublicGetActorEngagement"));
                PublicGetRelatedActors = reinterpret_cast<std::string(*)(uint32_t, int, double, double)>(
                    GetProcAddress(hDLL, "PublicGetRelatedActors"));
                PublicGetPlayerContext = reinterpret_cast<std::string(*)(float)>(
                    GetProcAddress(hDLL, "PublicGetPlayerContext"));
                PublicGetEventPairCounts = reinterpret_cast<std::string(*)(const char*, int)>(
                    GetProcAddress(hDLL, "PublicGetEventPairCounts"));

                // Plugin config API
                PublicGetPluginConfig = reinterpret_cast<std::string(*)(const char*)>(
                    GetProcAddress(hDLL, "PublicGetPluginConfig"));
                PublicGetPluginConfigValue = reinterpret_cast<std::string(*)(const char*, const char*, const char*)>(
                    GetProcAddress(hDLL, "PublicGetPluginConfigValue"));
            }

            // v4+ functions
            if (version >= 4) {
                PublicGetDiaryEntries = reinterpret_cast<std::string(*)(uint32_t, int, double, double)>(
                    GetProcAddress(hDLL, "PublicGetDiaryEntries"));
            }
        }
        return true;
    }
    return false;
}
}

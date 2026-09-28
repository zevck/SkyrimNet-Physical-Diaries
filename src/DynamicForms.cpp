/*
 * SkyrimNet Physical Diaries - a Skyrim SKSE plugin that turns SkyrimNet NPC
 * diary entries into books you can find and read in the world.
 * Copyright (C) 2026 Zevick
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "DynamicForms.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace DynamicForms {

    namespace {
        std::mutex g_mutex;
        std::unordered_map<RE::FormID, Record> g_tracked;
        std::atomic<bool> g_anyRetired{ false };  // set when a form is retired or loaded retired

        constexpr std::uint32_t kRecordVersion = 2;  // 1 (no flags byte) was never released
        constexpr std::uint32_t kMaxStringLength = 4096;  // keys and names; a longer one is corruption
        constexpr std::uint8_t kFlagRetired = 1;

        void WriteString(SKSE::SerializationInterface* a_intfc, const std::string& a_text)
        {
            const auto length = static_cast<std::uint32_t>(a_text.size());
            a_intfc->WriteRecordData(length);
            if (length > 0) a_intfc->WriteRecordData(a_text.data(), length);
        }

        bool ReadString(SKSE::SerializationInterface* a_intfc, std::string& a_text)
        {
            std::uint32_t length = 0;
            if (a_intfc->ReadRecordData(length) != sizeof(length)) return false;
            if (length > kMaxStringLength) return false;
            a_text.assign(length, '\0');
            return length == 0 || a_intfc->ReadRecordData(a_text.data(), length) == length;
        }
    }

    void MakePersistent(RE::TESForm* a_form)
    {
        // The change record is what puts the form in the save; the gate that adds it
        // turns away temporary forms.
        a_form->formFlags &= ~RE::TESForm::RecordFlags::kTemporary;
        a_form->AddChange(RE::TESForm::ChangeFlags::kFlags);
    }

    void Track(Record a_record)
    {
        std::lock_guard lock{ g_mutex };
        const auto formId = a_record.formId;
        g_tracked.insert_or_assign(formId, std::move(a_record));
    }

    void Retire(RE::FormID a_formId)
    {
        std::lock_guard lock{ g_mutex };
        if (const auto it = g_tracked.find(a_formId); it != g_tracked.end()) {
            it->second.retired = true;
            g_anyRetired = true;
        }
    }

    std::vector<Record> Tracked()
    {
        std::lock_guard lock{ g_mutex };
        std::vector<Record> records;
        records.reserve(g_tracked.size());
        for (const auto& [id, record] : g_tracked) records.push_back(record);
        return records;
    }

    bool AnyRetired()
    {
        return g_anyRetired;
    }

    bool IsRetired(RE::FormID a_formId)
    {
        std::lock_guard lock{ g_mutex };
        const auto it = g_tracked.find(a_formId);
        return it != g_tracked.end() && it->second.retired;
    }

    void Save(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type)
    {
        const auto records = Tracked();
        if (!a_intfc->OpenRecord(a_type, kRecordVersion)) {
            SKSE::log::error("[DynamicForms] Couldn't open the co-save record: {} form(s) will be blank shells on load",
                             records.size());
            return;
        }
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(records.size()));
        for (const auto& r : records) {
            a_intfc->WriteRecordData(r.formId);
            a_intfc->WriteRecordData(static_cast<std::uint8_t>(r.formType));
            a_intfc->WriteRecordData(static_cast<std::uint8_t>(r.retired ? kFlagRetired : 0));
            WriteString(a_intfc, r.key);
            WriteString(a_intfc, r.templateEditorId);
            WriteString(a_intfc, r.displayName);
        }
        SKSE::log::debug("[DynamicForms] Saved {} form record(s)", records.size());
    }

    void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        if (a_version != kRecordVersion) {
            SKSE::log::error("[DynamicForms] Co-save record version {} is unknown — skipped", a_version);
            return;
        }
        std::uint32_t count = 0;
        a_intfc->ReadRecordData(count);
        int kept = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            Record r;
            std::uint8_t type = 0, flags = 0;
            if (a_intfc->ReadRecordData(r.formId) != sizeof(r.formId) || a_intfc->ReadRecordData(type) != sizeof(type) ||
                a_intfc->ReadRecordData(flags) != sizeof(flags) || !ReadString(a_intfc, r.key) || !ReadString(a_intfc, r.templateEditorId) ||
                !ReadString(a_intfc, r.displayName)) {
                SKSE::log::error("[DynamicForms] Co-save record is truncated after {} of {} form(s)", i, count);
                break;
            }
            r.formType = static_cast<RE::FormType>(type);
            r.retired = (flags & kFlagRetired) != 0;
            if (r.retired) g_anyRetired = true;

            RE::FormID resolved = 0;
            auto* form = a_intfc->ResolveFormID(r.formId, resolved) ? RE::TESForm::LookupByID(resolved) : nullptr;
            if (!form || form->GetFormType() != r.formType) {
                // The engine gave this ID to another form in this save and renumbered
                // ours, or the form wasn't saved.
                SKSE::log::info("[DynamicForms] '{}' (0x{:X}) isn't in this save — dropped", r.key, r.formId);
                continue;
            }
            r.formId = resolved;
            Track(std::move(r));
            ++kept;
        }
        SKSE::log::info("[DynamicForms] Loaded {} of {} form record(s)", kept, count);
    }

    void Revert()
    {
        std::lock_guard lock{ g_mutex };
        g_tracked.clear();
        g_anyRetired = false;
    }

    void RebuildLoadedWorldCopies()
    {
        std::unordered_set<RE::FormID> ours;
        for (const auto& r : Tracked()) ours.insert(r.formId);
        auto* tes = RE::TES::GetSingleton();
        if (ours.empty() || !tes) return;

        int rebuilt = 0;
        tes->ForEachReference([&](RE::TESObjectREFR* ref) {
            const auto* base = ref ? ref->GetBaseObject() : nullptr;
            if (base && !ref->IsDeleted() && !ref->IsDisabled() && ours.contains(base->GetFormID())) {
                ref->Disable();
                ref->Enable(false);
                ++rebuilt;
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        if (rebuilt > 0) SKSE::log::info("[DynamicForms] Rebuilt the 3D of {} world cop(ies)", rebuilt);
    }

} // namespace DynamicForms

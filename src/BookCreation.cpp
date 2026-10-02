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

#include "BookCreation.h"
#include "ActorLookup.h"
#include "BookManager.h"
#include "Database.h"
#include "DynamicForms.h"
#include "Localization.h"
#include <algorithm>
#include <charconv>

namespace SkyrimNetDiaries {

    std::string VolumeKey(const std::string& actorUuid, VolumeKind kind, int volumeNumber) {
        return std::format("{}|{}{}", actorUuid, kind == VolumeKind::Written ? 'j' : 'v', volumeNumber);
    }

    bool ParseVolumeKey(const std::string& key, std::string& actorUuid, VolumeKind& kind, int& volumeNumber) {
        const auto bar = key.rfind('|');
        if (bar == std::string::npos || bar == 0 || bar + 1 >= key.size()) return false;
        if (key[bar + 1] == 'v') {
            kind = VolumeKind::Generated;
        } else if (key[bar + 1] == 'j') {
            kind = VolumeKind::Written;
        } else {
            return false;
        }
        const char* first = key.data() + bar + 2;
        const char* last = key.data() + key.size();
        const auto [end, ec] = std::from_chars(first, last, volumeNumber);
        if (ec != std::errc{} || end != last || volumeNumber < 1) return false;
        actorUuid = key.substr(0, bar);
        return true;
    }

    bool ConfigureDiaryForm(RE::TESObjectBOOK* book, const RE::TESObjectBOOK* templateBook,
                            const std::string& name) {
        // From the template: type (a tome: a note scroll ignores [pagebreak]), models, bounds, sounds, keywords,
        // item card, not flags.  A factory form has none: without model and bounds a dropped book vanishes.
        if (templateBook) {
            book->data.type = templateBook->data.type;
            book->inventoryModel = templateBook->inventoryModel;
            book->itemCardDescription = templateBook->itemCardDescription;
            if (const char* model = templateBook->GetModel(); model && *model) book->SetModel(model);
            book->boundData = templateBook->boundData;
            book->pickupSound = templateBook->pickupSound;
            book->putdownSound = templateBook->putdownSound;
            templateBook->ForEachKeyword([book](RE::BGSKeyword* keyword) {
                if (keyword && !book->HasKeyword(keyword)) book->AddKeyword(keyword);
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        book->weight = 0.5f;
        book->value = 0;
        book->data.flags = static_cast<RE::OBJ_BOOK::Flag>(0);

        const char* current = book->GetFullName();
        const bool renamed = !current || name != current;
        if (renamed) book->SetFullName(name.c_str());
        return renamed;
    }

    void ConfigureLoadedBooks() {
        int configured = 0;
        for (const auto& record : DynamicForms::Tracked()) {
            auto* book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(record.formId);
            if (!book) continue;
            auto* templateBook = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>(record.templateEditorId);
            if (!templateBook) {
                SKSE::log::warn("[BookForms] Book 0x{:X} ('{}'): template '{}' not found", record.formId, record.displayName,
                                record.templateEditorId);
            }
            ConfigureDiaryForm(book, templateBook, record.displayName);
            ++configured;
        }
        SKSE::log::info("[BookForms] Configured {} diary book(s) from this save", configured);
    }

    namespace {
        struct SweepCount { int inventory = 0; int world = 0; };

        // Only these can be, or hold, a book.
        bool MayHoldBook(const RE::TESObjectREFR* ref) {
            const auto* base = ref ? ref->GetBaseObject() : nullptr;
            const auto type = base ? base->GetFormType() : RE::FormType::None;
            return type == RE::FormType::Book || type == RE::FormType::Container || type == RE::FormType::NPC;
        }

        // Game thread: deletes a retired book's world copy, or removes retired books from ref's inventory.
        // noInit: a never-opened inventory can't hold one, and initializing it would roll leveled loot early.
        void RemoveRetiredBooksFrom(RE::TESObjectREFR* ref, SweepCount& count) {
            if (!ref || ref->IsDeleted()) return;
            const auto* base = ref->GetBaseObject();
            if (!base) return;
            if (base->GetFormType() == RE::FormType::Book) {
                if (DynamicForms::IsRetired(base->GetFormID())) {
                    ref->Disable();
                    ref->SetDelete(true);
                    ++count.world;
                    SKSE::log::debug("[BookForms] Deleted a copy of retired book 0x{:X} (ref 0x{:X})",
                                     base->GetFormID(), ref->GetFormID());
                }
                return;
            }
            const auto inventory = ref->GetInventory([](RE::TESBoundObject& item) {
                return item.GetFormType() == RE::FormType::Book && DynamicForms::IsRetired(item.GetFormID());
            }, true);
            for (const auto& [item, data] : inventory) {
                if (data.first <= 0) continue;
                ref->RemoveItem(item, data.first, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                ++count.inventory;
                SKSE::log::debug("[BookForms] Removed retired book 0x{:X} from ref 0x{:X}", item->GetFormID(),
                                 ref->GetFormID());
            }
        }

        // Removes retired books from references as their cell attaches.
        class RetiredBookSweeper : public RE::BSTEventSink<RE::TESCellAttachDetachEvent> {
        public:
            static RetiredBookSweeper* GetSingleton() {
                static RetiredBookSweeper singleton;
                return &singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event,
                                                  RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override {
                if (a_event && a_event->attached && DynamicForms::AnyRetired() && MayHoldBook(a_event->reference.get())) {
                    SKSE::GetTaskInterface()->AddTask([refId = a_event->reference->GetFormID()]() {
                        SweepCount count;
                        RemoveRetiredBooksFrom(RE::TESForm::LookupByID<RE::TESObjectREFR>(refId), count);
                        if (count.inventory + count.world > 0) {
                            SKSE::log::info("[BookForms] Swept ref 0x{:X} as it loaded: {} inventory cop(ies), {} world cop(ies)",
                                            refId, count.inventory, count.world);
                        }
                    });
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void SweepRetiredBooks() {
        if (!DynamicForms::AnyRetired()) return;
        SweepCount count;
        if (auto* tes = RE::TES::GetSingleton()) {
            tes->ForEachReference([&](RE::TESObjectREFR* ref) {
                if (MayHoldBook(ref)) RemoveRetiredBooksFrom(ref, count);
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        // Outside the loaded cells: the owners (persistent NPCs are in memory wherever
        // they are) and merchant chests (barter reads them without loading their cell).
        std::vector<RE::TESObjectREFR*> holders;
        for (const auto& record : DynamicForms::Tracked()) {
            std::string uuid;
            VolumeKind kind{};
            int volume = 0;
            if (record.retired && ParseVolumeKey(record.key, uuid, kind, volume)) {
                if (auto* owner = RE::TESForm::LookupByID<RE::Actor>(Database::GetFormIDForUUID(uuid))) holders.push_back(owner);
            }
        }
        if (auto* data = RE::TESDataHandler::GetSingleton()) {
            for (auto* faction : data->GetFormArray<RE::TESFaction>()) {
                if (faction && faction->vendorData.merchantContainer) holders.push_back(faction->vendorData.merchantContainer);
            }
        }
        for (auto* holder : holders) RemoveRetiredBooksFrom(holder, count);
        if (count.inventory + count.world > 0) {
            SKSE::log::info("[BookForms] Swept retired books: {} inventory cop(ies) removed, {} world cop(ies) deleted",
                            count.inventory, count.world);
        }
    }

    void RegisterRetiredBookSweeper() {
        if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
            events->AddEventSink<RE::TESCellAttachDetachEvent>(RetiredBookSweeper::GetSingleton());
            SKSE::log::info("[BookForms] Registered the retired-book sweeper (TESCellAttachDetachEvent)");
        }
    }

    void BookManager::CreateDiaryBook(const std::string& actorUuid, const std::string& actorName, int afterId,
                                      int volumeNumber, RE::FormID targetActorFormID,
                                      const std::vector<DiaryEntry>& entries, const std::string& bioTemplateName) {
        if (entries.empty()) return;
        CreateVolumeBook(actorUuid, actorName, VolumeKind::Generated, afterId, volumeNumber, targetActorFormID,
                         entries, bioTemplateName);
    }

    RE::FormID BookManager::CreateEmptyVolume(const std::string& actorUuid, const std::string& actorName,
                                              int volumeNumber, RE::FormID targetActorFormID,
                                              const std::string& bioTemplateName, const std::string& look) {
        return CreateVolumeBook(actorUuid, actorName, VolumeKind::Written, 0, volumeNumber, targetActorFormID, {},
                                bioTemplateName, look);
    }

    RE::FormID BookManager::CreateVolumeBook(const std::string& actorUuid, const std::string& actorName,
                                             VolumeKind kind, int afterId, int volumeNumber,
                                             RE::FormID targetActorFormID, const std::vector<DiaryEntry>& entries,
                                             const std::string& bioTemplateName, const std::string& look) {
        const std::string templateToUse = look.empty() ? SelectJournalTemplate(actorUuid, actorName, targetActorFormID)
                                                       : look;

        // LookupByEditorID works with po3's Tweaks or Native EditorID Fix; a GetFormEditorID() scan doesn't
        // (it returns "" for books without Native EditorID Fix).  See docs/BOOK_FORMS.md#templates.
        auto* templateBook = RE::TESForm::LookupByEditorID<RE::TESObjectBOOK>(templateToUse);
        if (!templateBook) {
            SKSE::log::error("Template book not found with Editor ID: '{}'", templateToUse);
            return 0;
        }

        // This save may already have a book for this volume that nothing claims (see
        // LoadFromDB): reuse it, wherever it is.  Otherwise make one.
        RE::TESObjectBOOK* book = nullptr;
        if (const auto it = unclaimed_.find(VolumeKey(actorUuid, kind, volumeNumber)); it != unclaimed_.end()) {
            book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(it->second);
            unclaimed_.erase(it);
        }
        const bool reused = book != nullptr;
        if (!book) book = DynamicForms::Create<RE::TESObjectBOOK>();
        if (!book) {
            SKSE::log::error("Couldn't create a book form for {} vol {}: it is created the next time it is needed",
                             actorName, volumeNumber);
            return 0;
        }
        const RE::FormID bookId = book->GetFormID();
        const std::string bookName = Localization::GetSingleton()->FormatBookName(actorName, volumeNumber, kind);
        ConfigureDiaryForm(book, templateBook, bookName);
        DynamicForms::Track({ .formId = bookId,
                              .formType = RE::FormType::Book,
                              .key = VolumeKey(actorUuid, kind, volumeNumber),
                              .templateEditorId = templateToUse,
                              .displayName = bookName });

        DiaryBookData data;
        data.actorUuid = actorUuid;
        data.actorName = actorName;
        data.bookFormId = bookId;
        data.afterId = afterId;
        data.lastId = entries.empty() ? afterId : entries.back().id;
        data.volumeNumber = volumeNumber;
        data.kind = kind;
        data.journalTemplate = templateToUse;
        data.bioTemplateName = bioTemplateName;
        data.actorFormId = targetActorFormID;
        auto& registered = RegisterBook(std::move(data));
        SetVolumeText(registered, entries);

        // FindActorForBook: player special-case, UUID-keyed cache, then
        // UUID → live FormID, then stored FormID with a UUID back-check.
        if (reused) {
            // Not given again: the NPC still has it, or it was taken (a theft stands).
            SKSE::log::info("✓ Reused this save's book 0x{:X} for '{}'", bookId, bookName);
            return bookId;
        }
        RE::Actor* targetActor = FindActorForBook(targetActorFormID, actorName, bioTemplateName, actorUuid);
        if (!targetActor) {
            SKSE::log::error("Failed to find target actor 0x{:X} ({}) for '{}' - book created but not added to inventory",
                             targetActorFormID, actorName, bookName);
            return bookId;
        }
        targetActor->AddObjectToContainer(book, nullptr, 1, nullptr);
        SKSE::log::info("✓ Added '{}' (0x{:X}) to {}'s inventory", bookName, bookId, actorName);
        return bookId;
    }

} // namespace SkyrimNetDiaries

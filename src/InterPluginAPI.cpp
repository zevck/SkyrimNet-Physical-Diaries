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

#include "InterPluginAPI.h"
#include "BookManager.h"
#include "BookText.h"
#include "Localization.h"
#include "SkyrimNetPhysicalDiariesAPI.h"
#include <cstring>

namespace SkyrimNetDiaries::InterPluginAPI {

    namespace {

        // Splits rendered book text on kPageBreak.
        std::vector<std::string_view> SplitPages(std::string_view text) {
            std::vector<std::string_view> pages;
            std::size_t pos = 0;
            while (true) {
                auto found = text.find(kPageBreak, pos);
                if (found == std::string_view::npos) { pages.push_back(text.substr(pos)); break; }
                pages.push_back(text.substr(pos, found - pos));
                pos = found + kPageBreak.size();
            }
            return pages;
        }

        // A rendered volume's entry pages (page 0 is blank, page 1 the title page),
        // trailing newlines trimmed.  Views into `text`.
        constexpr std::size_t kFirstEntryPage = 2;
        std::vector<std::string_view> EntryPages(std::string_view text) {
            const auto pages = SplitPages(text);
            std::vector<std::string_view> entries;
            for (std::size_t i = kFirstEntryPage; i < pages.size(); ++i) {
                std::string_view page = pages[i];
                while (!page.empty() && (page.back() == '\n' || page.back() == '\r'))
                    page.remove_suffix(1);
                entries.push_back(page);
            }
            return entries;
        }

        // The book text escapes "&", "<" and ">" in diary prose for Scaleform.  Callers
        // got the raw characters before that, so hand them back unescaped.
        std::string UnescapeMarkup(std::string_view text) {
            std::string out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '&') {
                    if (text.substr(i, 5) == "&amp;") { out += '&'; i += 4; continue; }
                    if (text.substr(i, 4) == "&lt;")  { out += '<'; i += 3; continue; }
                    if (text.substr(i, 4) == "&gt;")  { out += '>'; i += 3; continue; }
                }
                out += text[i];
            }
            return out;
        }

        bool IsEmptyVolume(const std::string& cachedText) {
            return cachedText.find(Localization::kEmptySentinel) != std::string::npos;
        }

        void HandleBookQuery(SKSE::MessagingInterface::Message* msg) {
            if (!msg->data || msg->dataLen < sizeof(SkyrimNetPhysicalDiaries_API::SNPDBookQuery)) {
                SKSE::log::warn("SNPD_QUERY_BOOK: invalid message size {} from '{}'",
                               msg->dataLen, msg->sender ? msg->sender : "unknown");
                return;
            }
            auto* query = static_cast<SkyrimNetPhysicalDiaries_API::SNPDBookQuery*>(msg->data);
            query->isDiaryBook  = false;
            query->resultCode   = SkyrimNetPhysicalDiaries_API::SNPDResultCode::NotADiary;
            query->entryCount   = 0;
            query->volumeNumber = 0;
            query->totalVolumes = 0;
            query->text[0] = '\0';

            auto* bm       = SkyrimNetDiaries::BookManager::GetSingleton();
            auto* bookData = bm->GetBookForFormID(static_cast<RE::FormID>(query->bookFormId));
            if (bookData && !bookData->cachedBookText.empty()) {
                query->isDiaryBook   = true;
                query->entryCount    = bookData->lastKnownEntryCount;
                query->volumeNumber  = bookData->volumeNumber;
                auto* allVols        = bm->GetAllVolumesForActor(bookData->actorUuid, bookData->kind);
                query->totalVolumes  = allVols ? static_cast<std::int32_t>(allVols->size()) : 1;

                // Don't expose the "entries removed" placeholder — it would be read aloud by TTS mods.
                // Leave text empty so callers know there's nothing to read for this volume.
                if (!IsEmptyVolume(bookData->cachedBookText)) {
                    const std::string t = UnescapeMarkup(StripBookmarks(bookData->cachedBookText));
                    query->resultCode = SkyrimNetPhysicalDiaries_API::SNPDResultCode::Success;
                    size_t copyLen = std::min(t.size(), sizeof(query->text) - 1);
                    std::memcpy(query->text, t.c_str(), copyLen);
                    query->text[copyLen] = '\0';
                    SKSE::log::debug("SNPD_QUERY_BOOK: returned {} chars for FormID 0x{:X} (vol {}/{}, {} entries)",
                                   copyLen, query->bookFormId, query->volumeNumber, query->totalVolumes, query->entryCount);
                } else {
                    query->resultCode = SkyrimNetPhysicalDiaries_API::SNPDResultCode::NoEntries;
                    SKSE::log::debug("SNPD_QUERY_BOOK: FormID 0x{:X} is a diary but has no readable entries (all removed)", query->bookFormId);
                }
            } else if (bookData) {
                SKSE::log::warn("SNPD_QUERY_BOOK: FormID 0x{:X} found but cachedBookText is empty", query->bookFormId);
            }
        }

        void HandleEntryQuery(SKSE::MessagingInterface::Message* msg) {
            if (!msg->data || msg->dataLen < sizeof(SkyrimNetPhysicalDiaries_API::SNPDEntryQuery)) {
                SKSE::log::warn("SNPD_QUERY_ENTRY: invalid message size {} from '{}'",
                               msg->dataLen, msg->sender ? msg->sender : "unknown");
                return;
            }
            auto* query = static_cast<SkyrimNetPhysicalDiaries_API::SNPDEntryQuery*>(msg->data);
            query->isValid       = false;
            query->resultCode    = SkyrimNetPhysicalDiaries_API::SNPDResultCode::NotADiary;
            query->totalEntries  = 0;
            query->returnedIndex = -1;
            query->content[0]    = '\0';

            auto* bookData = SkyrimNetDiaries::BookManager::GetSingleton()
                                 ->GetBookForFormID(static_cast<RE::FormID>(query->bookFormId));
            if (!bookData || bookData->cachedBookText.empty()) {
                SKSE::log::warn("SNPD_QUERY_ENTRY: FormID 0x{:X} not found or empty", query->bookFormId);
                return;
            }

            const std::string text = UnescapeMarkup(StripBookmarks(bookData->cachedBookText));
            const auto entries = EntryPages(text);
            const int entryPageCount = static_cast<int>(entries.size());
            if (entryPageCount <= 0 || IsEmptyVolume(bookData->cachedBookText)) {
                query->resultCode = SkyrimNetPhysicalDiaries_API::SNPDResultCode::NoEntries;
                SKSE::log::debug("SNPD_QUERY_ENTRY: FormID 0x{:X} has no entries", query->bookFormId);
                return;
            }

            query->totalEntries = entryPageCount;

            std::int32_t idx = query->entryIndex;
            if (idx < 0) idx = entryPageCount - 1;
            if (idx >= entryPageCount) {
                query->resultCode = SkyrimNetPhysicalDiaries_API::SNPDResultCode::IndexOutOfRange;
                SKSE::log::warn("SNPD_QUERY_ENTRY: entryIndex {} out of range (0-{})",
                               query->entryIndex, entryPageCount - 1);
                return;
            }

            const std::string_view page = entries[static_cast<std::size_t>(idx)];
            query->isValid       = true;
            query->resultCode    = SkyrimNetPhysicalDiaries_API::SNPDResultCode::Success;
            query->returnedIndex = idx;

            std::size_t copyLen = std::min(page.size(), sizeof(query->content) - 1);
            std::memcpy(query->content, page.data(), copyLen);
            query->content[copyLen] = '\0';

            SKSE::log::debug("SNPD_QUERY_ENTRY: returned entry {}/{} for FormID 0x{:X} ({} chars)",
                           idx, entryPageCount - 1, query->bookFormId, copyLen);
        }

        void HandleAllEntriesQuery(SKSE::MessagingInterface::Message* msg) {
            if (!msg->data || msg->dataLen < sizeof(SkyrimNetPhysicalDiaries_API::SNPDAllEntriesQuery)) {
                SKSE::log::warn("SNPD_QUERY_ALL_ENTRIES: invalid message size {} from '{}'",
                               msg->dataLen, msg->sender ? msg->sender : "unknown");
                return;
            }
            auto* query = static_cast<SkyrimNetPhysicalDiaries_API::SNPDAllEntriesQuery*>(msg->data);
            query->isValid        = false;
            query->resultCode     = SkyrimNetPhysicalDiaries_API::SNPDResultCode::NotADiary;
            query->entryCount     = 0;
            query->truncatedCount = 0;
            query->content[0]     = '\0';

            auto* bookData = SkyrimNetDiaries::BookManager::GetSingleton()
                                 ->GetBookForFormID(static_cast<RE::FormID>(query->bookFormId));
            if (!bookData || bookData->cachedBookText.empty()) {
                SKSE::log::warn("SNPD_QUERY_ALL_ENTRIES: FormID 0x{:X} not found or empty", query->bookFormId);
                return;
            }

            const std::string text = UnescapeMarkup(StripBookmarks(bookData->cachedBookText));
            const auto entries = EntryPages(text);

            query->isValid    = true;
            query->resultCode = SkyrimNetPhysicalDiaries_API::SNPDResultCode::Success;

            const int entryPageCount = static_cast<int>(entries.size());
            if (entryPageCount <= 0 || IsEmptyVolume(bookData->cachedBookText)) {
                SKSE::log::debug("SNPD_QUERY_ALL_ENTRIES: FormID 0x{:X} has no entries", query->bookFormId);
                return;
            }

            // Pack null-terminated page strings back-to-back into the buffer.
            constexpr std::size_t kBufSize = sizeof(query->content) - 1;
            std::size_t writePos = 0;
            int packed = 0, dropped = 0;
            for (int i = 0; i < entryPageCount; ++i) {
                const std::string_view page = entries[static_cast<std::size_t>(i)];
                if (writePos + page.size() + 1 > kBufSize) { ++dropped; continue; }
                std::memcpy(query->content + writePos, page.data(), page.size());
                writePos += page.size();
                query->content[writePos++] = '\0';
                ++packed;
            }
            query->content[writePos] = '\0';
            query->entryCount     = packed;
            query->truncatedCount = dropped;

            SKSE::log::debug("SNPD_QUERY_ALL_ENTRIES: packed {}/{} entries for FormID 0x{:X} ({} bytes, {} truncated)",
                           packed, entryPageCount, query->bookFormId, writePos, dropped);
        }

    } // namespace

    bool HandleMessage(SKSE::MessagingInterface::Message* msg) {
        try {
            switch (msg->type) {
            case SkyrimNetPhysicalDiaries_API::SNPD_QUERY_BOOK:        HandleBookQuery(msg);       return true;
            case SkyrimNetPhysicalDiaries_API::SNPD_QUERY_ENTRY:       HandleEntryQuery(msg);      return true;
            case SkyrimNetPhysicalDiaries_API::SNPD_QUERY_ALL_ENTRIES: HandleAllEntriesQuery(msg); return true;
            default:                                                   return false;
            }
        } catch (const std::exception& e) {
            SKSE::log::error("[InterPluginAPI] HandleMessage exception: {}", e.what());
        } catch (...) {
            SKSE::log::error("[InterPluginAPI] HandleMessage: unknown exception");
        }
        return true;
    }

} // namespace SkyrimNetDiaries::InterPluginAPI

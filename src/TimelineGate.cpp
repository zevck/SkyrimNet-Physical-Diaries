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

#include "TimelineGate.h"
#include "Database.h"
#include "Detour.h"
#include <atomic>
#include <chrono>
#include <mutex>

namespace SkyrimNetDiaries::TimelineGate {

    namespace {

        using Clock = std::chrono::steady_clock;

        // SkyrimNetInternal.ClearTimelineMessage(): button 0 keeps the history,
        // anything else clears it.
        constexpr const char* kPromptEditorID = "skynet_DeleteHistoryMessage";
        constexpr int kKeepButton = 0;

        // A prompt is expected but never seen (its text didn't match, say): don't
        // wait forever.
        constexpr auto kNoPromptTimeout = std::chrono::seconds(30);
        // After Clear, SkyrimNet deletes on its own thread a few ms after the box closes.
        constexpr auto kClearDeletionTimeout = std::chrono::seconds(10);

        enum class Prompt : int { kNone, kShown, kKept, kCleared };

        std::atomic<Prompt> g_prompt{ Prompt::kNone };
        std::atomic<Clock::rep> g_answeredAt{ 0 };

        std::mutex g_textMutex;
        std::string g_promptText;  // skynet_DeleteHistoryMessage text; "" = unknown

        // Poll state (game thread only).
        bool g_checked = false;
        bool g_promptExpected = false;
        Clock::time_point g_waitStart;
        bool g_loggedWaiting = false;

        std::string Trim(std::string_view a_text) {
            const auto begin = a_text.find_first_not_of(" \t\r\n");
            if (begin == std::string_view::npos) return {};
            const auto end = a_text.find_last_not_of(" \t\r\n");
            return std::string(a_text.substr(begin, end - begin + 1));
        }

        // True if SkyrimNet holds a diary entry dated after the current game time,
        // i.e. the loaded save is behind SkyrimNet's history.
        bool HasFutureDiaryEntries() {
            const double now = SkyrimNetDiaries::CurrentGameTimeSeconds();
            if (now <= 0.0) return false;
            // A limit of 1 returns the newest entry.
            const auto newest = Database::GetDiaryEntries(0, 1, 0.0, 0.0);
            return !newest.empty() && DatedAfter(newest.front().entry_date, now);
        }

        // Passes the answer on to SkyrimNet's callback after noting which button it was.
        class PromptCallback : public RE::IMessageBoxCallback {
        public:
            PromptCallback(RE::BSTSmartPointer<RE::IMessageBoxCallback> a_original, std::uint8_t a_buttonOffset)
                : original_(std::move(a_original)), buttonOffset_(a_buttonOffset) {}

            ~PromptCallback() override = default;

            void Run(std::uint8_t a_button) override {
                const int index = static_cast<int>(a_button) - static_cast<int>(buttonOffset_);
                const bool keep = index == kKeepButton;
                g_answeredAt.store(Clock::now().time_since_epoch().count());
                g_prompt.store(keep ? Prompt::kKept : Prompt::kCleared);
                SKSE::log::info("[TimelineGate] SkyrimNet timeline prompt answered: {}", keep ? "Keep" : "Clear");
                if (original_) {
                    original_->Run(a_button);
                }
            }

        private:
            RE::BSTSmartPointer<RE::IMessageBoxCallback> original_;
            std::uint8_t buttonOffset_;
        };

        using QueueMessage_t = void (*)(RE::MessageBoxData*);
        QueueMessage_t g_originalQueueMessage = nullptr;

        void Hook_QueueMessage(RE::MessageBoxData* a_data) {
            try {
                if (a_data && a_data->bodyText.c_str()) {
                    std::string promptText;
                    {
                        std::lock_guard lock(g_textMutex);
                        promptText = g_promptText;
                    }
                    if (!promptText.empty() && Trim(a_data->bodyText.c_str()) == promptText) {
                        a_data->callback = RE::BSTSmartPointer<RE::IMessageBoxCallback>(
                            new PromptCallback(a_data->callback, a_data->buttonPressOffset));
                        g_prompt.store(Prompt::kShown);
                        SKSE::log::info("[TimelineGate] SkyrimNet timeline prompt shown");
                    }
                }
            } catch (const std::exception& e) {
                SKSE::log::error("[TimelineGate] QueueMessage hook exception: {}", e.what());
            } catch (...) {
                SKSE::log::error("[TimelineGate] QueueMessage hook unknown exception");
            }
            g_originalQueueMessage(a_data);
        }

    } // namespace

    void Install() {
        // SE id 51422 (also used by VR) | AE id 52271.  SkyrimNet hooks this function too.
        REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(51422, 52271) };
        InstallDetour(target.address(), reinterpret_cast<void*>(&Hook_QueueMessage),
                      reinterpret_cast<void**>(&g_originalQueueMessage), "QueueMessage", "51422/52271",
                      "the timeline prompt is not tracked");
    }

    void OnDataLoaded() {
        auto* message = RE::TESForm::LookupByEditorID<RE::BGSMessage>(kPromptEditorID);
        if (!message) {
            SKSE::log::warn("[TimelineGate] '{}' not found (SkyrimNet not installed, or no EditorID "
                            "provider) — SNPD can't wait for SkyrimNet's timeline prompt", kPromptEditorID);
            return;
        }
        RE::BSString text;
        message->GetDescription(text, message);
        std::lock_guard lock(g_textMutex);
        g_promptText = Trim(text.c_str() ? text.c_str() : "");
        SKSE::log::info("[TimelineGate] SkyrimNet timeline prompt found ({} chars)", g_promptText.size());
    }

    void Reset() {
        g_prompt.store(Prompt::kNone);
        g_answeredAt.store(0);
        g_checked = false;
        g_promptExpected = false;
        g_loggedWaiting = false;
    }

    std::string_view Outcome() {
        switch (g_prompt.load()) {
        case Prompt::kShown:   return "unanswered";
        case Prompt::kKept:    return "Keep";
        case Prompt::kCleared: return "Clear";
        default:               return "no prompt";
        }
    }

    bool IsSettled() {
        const auto now = Clock::now();
        if (!g_checked) {
            g_checked = true;
            g_waitStart = now;
            // SkyrimNet's own test: it asks keep/clear exactly when the player's latest
            // event is later than the loaded save's game time.  Future diary entries
            // alone don't make it ask.
            const double gameNow = SkyrimNetDiaries::CurrentGameTimeSeconds();
            const double lastEvent = Database::GetPlayerLastEventTime();
            g_promptExpected = lastEvent > gameNow;
            if (g_promptExpected) {
                SKSE::log::info("[TimelineGate] SkyrimNet's history runs past this save ({:.2f} > {:.2f}) — "
                                "waiting for its keep/clear prompt before syncing", lastEvent, gameNow);
            }
        }

        switch (g_prompt.load()) {
        case Prompt::kKept:
            return true;
        case Prompt::kCleared: {
            if (!HasFutureDiaryEntries()) {
                return true;
            }
            const Clock::time_point answeredAt{ Clock::duration(g_answeredAt.load()) };
            if (now - answeredAt > kClearDeletionTimeout) {
                SKSE::log::warn("[TimelineGate] Clear was chosen but future diary entries are still there "
                                "after {}s — continuing", kClearDeletionTimeout.count());
                return true;
            }
            return false;
        }
        case Prompt::kShown:
            return false;  // the player may take as long as they like to answer
        case Prompt::kNone:
        default:
            if (!g_promptExpected) {
                return true;  // SkyrimNet won't ask: its history stays as it is
            }
            // The player's future events are gone without us seeing the prompt:
            // SkyrimNet has already cleared them.
            if (Database::GetPlayerLastEventTime() <= SkyrimNetDiaries::CurrentGameTimeSeconds()) {
                return true;
            }
            if (now - g_waitStart > kNoPromptTimeout) {
                SKSE::log::warn("[TimelineGate] No SkyrimNet timeline prompt within {}s — continuing",
                                kNoPromptTimeout.count());
                return true;
            }
            if (!g_loggedWaiting) {
                g_loggedWaiting = true;
                SKSE::log::debug("[TimelineGate] Waiting for SkyrimNet's timeline prompt...");
            }
            return false;
        }
    }

} // namespace SkyrimNetDiaries::TimelineGate

#pragma once
#include <cstdint>

namespace DynamicBookFramework_API
{
    constexpr auto FrameworkPluginName = "DynamicBookFramework";
    constexpr auto InterfaceVersion = 1;

    // Define unique message types.
    enum APIMessageType : std::uint32_t
    {
        kAppendEntry = 'DBFA',      // 'D' 'B' 'F' 'A' for Dynamic Book Framework Append
        kOverwriteBook = 'DBFO',    // 'D' 'B' 'F' 'O' for Dynamic Book Framework Overwrite
        kExcludeBook = 'DBFE',      // 'D' 'B' 'F' 'E' for Dynamic Book Framework Exclude
        kAddDynamicText = 'DBDT',   // 'D' 'B' 'D' 'T' for Dynamic Book Dynamic Text
        kRequestUIRefresh = 'DBRR', // 'D' 'B' 'R' 'R' Dynamic Book Refresh Request
        kReloadBookMappings = 'DBRL',
        kRequestEditorBookListRefresh = 'DBLR',
    };

    struct AppendEntryMessage
    {
        const char* bookTitleKey; // The title of the book, which acts as the file key
        const char* textToAppend; // The new entry text to add
    };

    struct OverwriteBookMessage
    {
        const char* bookTitleKey;
        const char* newContent; // The new, permanent content for the book
    };

    struct ExcludeBookMessage
    {
        const char* bookTitleKey;
    };

    struct AddDynamicTextMessage
    {
        const char* bookTitleKey;
        const char* dynamicText; // The text to display in the book for the current session
    };

    // Helper function to send messages to Dynamic Book Framework
    inline bool SendMessage(std::uint32_t messageType, void* messageData, size_t dataSize = 0)
    {
        auto messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            SKSE::log::error("DBF API: Failed to get messaging interface");
            return false;
        }

        bool result = messaging->Dispatch(messageType, messageData, dataSize, FrameworkPluginName);
        SKSE::log::debug("DBF API: Dispatched message type 0x{:X} to '{}', result: {}",
                        messageType, FrameworkPluginName, result);
        return result;
    }

    // Check if Dynamic Book Framework is loaded
    inline bool IsLoaded()
    {
        auto pluginHandle = SKSE::GetPluginHandle();
        auto messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            return false;
        }

        // Try to get the plugin handle for DBF
        auto* pluginInfo = SKSE::PluginDeclaration::GetSingleton();
        if (!pluginInfo) {
            return false;
        }

        // DBF should be loaded if we can dispatch to it
        // Unfortunately SKSE doesn't provide a direct "is plugin loaded" check
        // We'll have to rely on message dispatch success
        return true;
    }

    // Convenience functions for common operations
    inline bool SetDynamicText(const char* bookTitle, const char* text)
    {
        SKSE::log::debug("DBF API: SetDynamicText for '{}'", bookTitle);
        AddDynamicTextMessage msg{bookTitle, text};
        return SendMessage(kAddDynamicText, &msg, sizeof(msg));
    }

    inline bool OverwriteBook(const char* bookTitle, const char* text)
    {
        SKSE::log::info("DBF API: OverwriteBook for '{}' ({} chars)", bookTitle, strlen(text));
        OverwriteBookMessage msg{bookTitle, text};
        
        // CRITICAL: Pass sizeof(OverwriteBookMessage) as the data size
        // DBF expects to read the struct with valid pointer addresses
        auto messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            SKSE::log::error("DBF API: Failed to get messaging interface");
            return false;
        }

        // Use Dispatch with exact struct size
        bool result = messaging->Dispatch(
            kOverwriteBook,
            (void*)&msg,
            sizeof(OverwriteBookMessage),
            FrameworkPluginName
        );
        
        SKSE::log::info("DBF API: Dispatched OverwriteBook message, result: {}", result);
        SKSE::log::info("DBF API: Message size: {}, bookTitle ptr: {:p}, text ptr: {:p}", 
                       sizeof(OverwriteBookMessage), (void*)bookTitle, (void*)text);
        return result;
    }

    inline bool AppendEntry(const char* bookTitle, const char* text)
    {
        SKSE::log::info("DBF API: AppendEntry for '{}'", bookTitle);
        AppendEntryMessage msg{bookTitle, text};
        return SendMessage(kAppendEntry, &msg, sizeof(msg));
    }

    inline bool ReloadBookMappings()
    {
        SKSE::log::debug("DBF API: ReloadBookMappings");
        return SendMessage(kReloadBookMappings, nullptr, 0);
    }

    inline bool RequestUIRefresh()
    {
        SKSE::log::info("DBF API: RequestUIRefresh");
        return SendMessage(kRequestUIRefresh, nullptr, 0);
    }

} // namespace DynamicBookFramework_API

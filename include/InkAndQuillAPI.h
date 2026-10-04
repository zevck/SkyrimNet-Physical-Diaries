/*
 * Ink & Quill - a Skyrim SKSE writing framework: the player writes in books in the
 * book menu, with quill, ink or blood, for any mod that gives the text a meaning.
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
 *
 * For client mods: copy this header into your project.  It is plain C, so any compiler and CRT work.
 */

/*
 * Ink & Quill's C API.  docs/API.md has the full contract; in short:
 *
 *  - Get it at kPostLoad or later: GetModuleHandleA("InkAndQuill.dll"), GetProcAddress(module, "IQ_GetAPI"), then
 *    IQ_GetAPI(IQ_API_VERSION).  NULL: Ink & Quill is older than this header.
 *  - Every call is on the game's main thread (where SKSE runs UI tasks and where the paused book menu's input
 *    arrives), and every callback comes there.
 *  - Strings are UTF-8.  Ink & Quill copies every string it is given before the call returns; strings it passes
 *    to a callback are valid until the callback returns.
 *  - Text is "marked text": the book's text as the client renders it for reading, with what the player can't
 *    change between U+E002 and U+E003.  The runs between locks are what the player writes; blood text in a run
 *    is between U+E000 and U+E001.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define IQ_API_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

/* The answer to a save, given from inside onSave through IQ_API::ReplySave or ReplySaveAsBook. */
typedef struct IQ_SaveReply IQ_SaveReply;

typedef enum IQ_Ink
{
    IQ_INK_NONE = 0,     /* no inkwell */
    IQ_INK_USED = 1,     /* one use taken */
    IQ_INK_RAN_DRY = 2,  /* that was the inkwell's last use: it's gone */
} IQ_Ink;

/* One book open for writing.  Ink & Quill copies it; user is handed back to every callback. */
typedef struct IQ_Session
{
    uint32_t size;           /* sizeof(IQ_Session) */
    const char* markedText;  /* required */
    const char* runFont;     /* optional: typed text's font; with runSize, paragraph breaks at the page's size */
    int32_t runSize;         /* optional: 0 for none */
    int32_t caretRun;        /* the run the caret starts at the end of; -1: the page being read */
    void* user;

    /* Required.  The runs in order; answer with ReplySave (accepted with the reading text, or refused with a message).
       No answer is a refusal.  docs/API.md#saving */
    void (*onSave)(void* user, const char* const* runs, int32_t count, IQ_SaveReply* reply);
    /* Optional.  The player discarded their changes. */
    void (*onDiscard)(void* user);
    /* Optional.  The session is over (closed, discarded, never started, a load): once, always last. */
    void (*onEnd)(void* user);
    /* Optional.  The player changed run's text (typed, erased, pasted); the caret is caretOffset characters into it.
       In the key's own UI task: a Reload from here shows with the keystroke.  docs/API.md#reacting-to-typing */
    void (*onChange)(void* user, int32_t run, int32_t caretOffset);
} IQ_Session;

/* The edit key in an open book no session is writing in.  Return true if this book is yours and you called
   BeginSession for it; false passes it to the next owner. */
typedef bool (*IQ_Owner)(void* user, uint32_t bookFormId);

typedef void (*IQ_RunVisitor)(void* user, int32_t index, const char* text);

/* A client prompt's answer: the button's index, once the prompt has closed and the keys are the editor's again. */
typedef void (*IQ_PromptDone)(void* user, int32_t button);

typedef struct IQ_API
{
    uint32_t size;     /* sizeof(IQ_API) in the DLL: newer versions only add at the end */
    uint32_t version;  /* the DLL's IQ_API_VERSION */

    /* Writing is on (Ink & Quill's book.swf is the one the game loads). */
    bool (*IsWritingOn)(void);
    /* The player is writing now, and in blood (decided when writing starts, so a new heading can be red). */
    bool (*IsWriting)(void);
    bool (*InBlood)(void);

    /* Owners are asked in the order they were added.  Add at kDataLoaded or later. */
    bool (*AddOwner)(IQ_Owner owner, void* user);

    /* The open book: quill and ink checked (or blood offered), then writing.  False: not started, and onEnd has
       run.  docs/API.md#starting */
    bool (*BeginSession)(const IQ_Session* session);
    /* As BeginSession, once bookFormId's book menu is open and has its text.  The client opens the menu. */
    bool (*BeginSessionOnOpen)(uint32_t bookFormId, const IQ_Session* session);

    /* While writing: the runs as the player has them, unsaved text included.  Returns the count, or -1. */
    int32_t (*CurrentRuns)(IQ_RunVisitor visit, void* user);
    /* While writing: the text rendered again (marked, and as it now reads); from[i] is the run new run i was (-1: new;
       -2 - k: saved as run k was when the session began), count the new runs'.  The caret: caretRun at caretOffset. */
    bool (*Reload)(const char* markedText, const char* readingText, const int32_t* from, int32_t count, int32_t caretRun,
                   int32_t caretOffset);

    void (*ReplySave)(IQ_SaveReply* reply, bool accepted, const char* message, const char* readingText);

    /* Writing materials, for a client with its own writing UI. */
    bool (*HasQuill)(void);
    bool (*HasInk)(void);
    IQ_Ink (*UseInk)(void);
    bool (*CanBleed)(void);
    bool (*Bleed)(void);

    /* Reading blankFormId from the player's inventory calls onOpen; the client replaces it now (ReplaceBlank) or
       on its first save (ReplySaveAsBook).  docs/API.md#blanks */
    bool (*RegisterBlank)(uint32_t blankFormId, IQ_Owner onOpen, void* user);
    /* A blank's save, accepted: one blank is removed and the open menu shows bookFormId. */
    void (*ReplySaveAsBook)(IQ_SaveReply* reply, uint32_t bookFormId, const char* readingText);
    /* In onOpen: the blank is replaced now by bookFormId, shown with readingText; a session begun after is the
       book's. */
    bool (*ReplaceBlank)(uint32_t bookFormId, const char* readingText);

    /* Optional: the name Ink & Quill's MCM lists the calling mod under (else its DLL's file name). */
    void (*SetClientName)(const char* name);

    /* A session begun now would be in blood (unless the player declines): for a new heading before BeginSession.
       InBlood once it has begun. */
    bool (*WouldBeInBlood)(void);

    /* While writing, the calling client's own keys still reach the game: this set replaces its last.  Keys that type
       or edit, and the edit key, are refused; returns how many were kept.  docs/API.md#clients-keys */
    int32_t (*RegisterKeys)(const uint32_t* keyCodes, int32_t count);

    /* While writing: the run the caret is in, or -1 (not writing, or the caret is in no run). */
    int32_t (*CaretRun)(void);
    /* While writing: a message box over the book; done once it has closed, never if the session ended first.
       False: not writing, a prompt open, or no buttons.  docs/API.md#asking-the-player */
    bool (*Prompt)(const char* text, const char* const* buttons, int32_t count, int32_t cancelButton, IQ_PromptDone done,
                   void* user);
    /* While writing: candidates for the text at the caret, each the text that would follow it ("ia", ", 6391 Whiterun");
       shown one at a time, faded.  count 0 clears them.  False: not writing, or a prompt open.  docs/API.md#suggestions */
    bool (*Suggest)(const char* const* completions, int32_t count);
    /* Whether a key works as a client's key while writing (RegisterKeys): an IQ_KEY_ value; message (optional) gets why
       not, translated, for the client's MCM to show.  docs/API.md#clients-keys */
    int32_t (*CheckKey)(uint32_t keyCode, const char** message);
} IQ_API;

/* CheckKey's answers. */
#define IQ_KEY_OK 0           /* works while writing */
#define IQ_KEY_NOT_KEYBOARD 1 /* a mouse or gamepad code */
#define IQ_KEY_TYPES 2        /* it types or edits while writing */
#define IQ_KEY_EDIT_KEY 3     /* Ink & Quill's edit key */

/* Exported by InkAndQuill.dll as "IQ_GetAPI". */
typedef const IQ_API* (*IQ_GetAPI_t)(uint32_t version);

/* Exported as "IQ_IsWriting": the player is writing (any client's session), for mods that only ask.  No API to get,
   not listed as a client; any thread.  docs/API.md#is-the-player-writing */
typedef bool (*IQ_IsWriting_t)(void);

#ifdef __cplusplus
}
#endif

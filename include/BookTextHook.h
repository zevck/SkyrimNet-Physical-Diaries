#pragma once

// Hooks BookMenu::OpenBookMenu (RELOCATION_ID 50122/51053) to inject our
// diary text at book-open time without relying on Dynamic Book Framework.
// Works on both SSE and VR because both RELOCATION IDs are present in the
// CommonLibSSE-NG address library.
namespace BookTextHook
{
    void Install();
}

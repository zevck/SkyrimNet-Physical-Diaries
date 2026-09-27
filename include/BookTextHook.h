#pragma once

// Hooks BookMenu::OpenBookMenu to inject our diary text at book-open time,
// without Dynamic Book Framework.  RELOCATION_ID(50122, 51053) is (SE, AE);
// VR reuses the SE id via the VR Address Library.
namespace BookTextHook
{
    void Install();
}

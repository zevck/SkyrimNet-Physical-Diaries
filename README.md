# SkyrimNet Physical Diaries

A companion mod for [SkyrimNet](https://github.com/MinLL/SkyrimNet-GamePlugin) that turns the AI-generated diary entries written by NPCs into actual books you can find and read in the world.

<p align="center">
  <img src="images/diaryexample.jpg">
</p>

---

## Features

### Physical Diary Books
Each NPC that writes diary entries will have a physical diary book in their inventory. The entries are formatted by in-game date and use a handwriting font.

### Multi-Volume Support
When a diary fills up, it seals itself and a fresh volume begins. Sealed volumes stay in the NPC's inventory alongside newer ones, giving you a complete history to read. The number of entries per volume can be configured in the MCM.

### Theft & Return System
Stealing an NPC's diary has consequences. The NPC will be aware their diary is missing when writing a new entry. Returning the diary before their next entry clears the record entirely — they'll never know it was taken. Taking a followers diary by trading items will trigger a response making the NPC aware that you took it.

### Automatic Updates
When an NPC writes a new entry, their physical diary updates to include it. If they fill the current volume, a new one is created automatically. This happens in the background without any player action needed.

---

## MCM Settings

Found under **SkyrimNet Physical Diaries** in the Mod Configuration Menu.

**Settings**
- **Entries Per Volume** — How many diary entries fit in one book before a new volume begins (default: 10, range: 1–50)
- **Font Sizes** — Separate sliders for title, date, body text, and small text in the diary books

**Maintenance**
- **Reset All Diaries** — Removes all physical diary books from NPCs and clears all tracking. Your SkyrimNet diary entries are untouched; books will regenerate automatically on next load.
- **Debug Logging** — Toggle verbose logging for troubleshooting.

---

## Requirements

- [SkyrimNet](https://github.com/MinLL/SkyrimNet-GamePlugin)
- [Dynamic Persistent Forms](https://www.nexusmods.com/skyrimspecialedition/mods/116001)
- [SkyUI](https://www.nexusmods.com/skyrimspecialedition/mods/12604) (for MCM)
- [SKSE](https://skse.silverlock.org/)
- [Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/32444) or [VR Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/58101)

---

## Installation

Install with a mod manager as normal. Load order: place after SkyrimNet and Dynamic Persistent Forms.

Currently in order to enable diary theft awareness you must add the following to your `diary_entry.prompt` underneath `## Instructions`:

```
{% if snpd_diary_stolen(npc.UUID) == "true" %}
    **IMPORTANT**: You notice that your diary has been stolen! Write about your reaction.
{% endif %}
```

---

## Notes

- Diary books appear in NPC inventories after SkyrimNet generates the NPC's first diary entry. NPCs without any diary entries will have no books. You must generate SkyrimNet's diary entries yourself.
- Player character diaries are supported and will appear in the player's inventory.
- If books are missing after installing on an existing save, use **Reset All Diaries** followed by saving and reloading.

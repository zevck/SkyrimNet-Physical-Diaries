#pragma once

namespace DiaryTheftHandler {
    // Register the event handler for diary theft detection
    void Register();

    // Registers the snpd_diary_stolen decorator with SkyrimNet.  SkyrimNet clears
    // decorator registrations on every load, so call this on every kPostLoadGame.
    void RegisterStolenDecorator();

    // kPostLoadGame: if this save is earlier in game time than the last session,
    // clears each actor's stolen volumes (the theft belongs to an abandoned
    // timeline), then records the current game time.  See docs/THEFT.md.
    void ReconcileAfterLoad();
}

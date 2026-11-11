// ============================================================================
//  Spore AutoSave On Event
//
//  SaveNotification.hpp - replaces the game's blocking "your game has been
//  saved" message box with a small self-drawn notice.
// ============================================================================
#pragma once

#include <cstdint>

/// Turns the save confirmation dialog into a quiet, non-blocking notice.
///
/// The game reports the result of a save by showing a modal message box from
/// inside its own save message handler:
///
///     cSPUIMessageBox::ShowDialog(callback,
///         ResourceKey(0x478A48AB, ..))   // success
///     cSPUIMessageBox::ShowDialog(callback,
///         ResourceKey(0x1BF88D5E, ..))   // failure
///
/// That box has to be clicked away, which is exactly what an automatic save
/// should not require. So ShowDialog is detoured and the success box is
/// answered as if the player had clicked it; instead a short line of text fades
/// in at the top right of the screen and fades out again on its own. The line is
/// drawn by a child window of the main interface that carries
/// kWinFlagAlwaysInFront, so the game's own interface draws it on top of
/// everything and this mod never has to reason about paint order.
///
/// The suppression is not restricted to saves this mod started. A measured run
/// showed the box arriving outside the save call entirely - the save files were
/// rewritten while no box appeared at all, and the box turned up afterwards - so
/// any window around the call would let the box through. The success instance id
/// is referenced nowhere else in the executable, so suppressing it always is
/// safe, and it means the game's own save button gets the same quiet report.
///
/// The failure box is left alone: if the save could not be written the player
/// must be told.
namespace SaveNotification
{
    /// Detours ShowDialog(). Must be called between PrepareDetours() and
    /// CommitDetours(), i.e. from the same transaction that installs the editor
    /// detours.
    void AttachDetours();

    /// Restores the detour and takes the notice window out of the interface.
    ///
    /// Removing the window is not optional: its vtable lives in this DLL, so
    /// leaving it as a child of the main window would have the game calling into
    /// unmapped memory once the module is unloaded. Must be called between
    /// PrepareDetours() and CommitDetours(), before this DLL is unloaded.
    void DetachDetours();

    /// Logs the result of the attach call.
    void LogDetourStatus();

    /// Records whether the fading line should be shown.
    /// @param  showNotice  When false the save is silent: the dialog is still
    ///                     suppressed, but no notice is drawn.
    void BeginSave(bool showNotice);

    /// Closes the preference window opened by BeginSave(). Whether the notice
    /// appears is decided by the save's own result box, not here.
    void EndSave();

    /// Advances the notice's fade timer. Called once per frame; does nothing
    /// while no notice is on screen.
    void Tick();
}

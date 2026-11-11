// ============================================================================
//  Spore AutoSave On Event
//
//  SaveNotification.cpp
// ============================================================================
#include "stdafx.h"
#include "SaveNotification.hpp"
#include "Log.hpp"

#include "NoticeGlyphData.hpp"

// ----------------------------------------------------------------------------
//  Why this file exists
//
//  Saving from the Simulator::kMsgSaveGame handler always ends with a modal
//  message box (cSPUIMessageBox::ShowDialog) that the player has to dismiss.
//  For a save the player did not ask for, that is pure interruption.
//
//  The handler was disassembled to find the exact call:
//
//      ...save the world, bl = whether it worked...
//      mov  byte ptr [edi+0x46], 0 / 1        ; which box to show
//      push 0x156CA4C   ; success box      (passed as ResourceKey&)
//      push <callback>
//      call 0x40A3B0    ; cSPUIMessageBox::ShowDialog
//      add  esp, 8
//
//  So the box cannot be avoided by calling the save function directly - that
//  function is nothing but MessageSend(kMsgSaveGame), and the box is raised by
//  its handler. Suppressing the call is therefore the only way in, and that is
//  what the detour below does: the *success* box is answered as if the player
//  clicked OK, and the fading line takes its place.
//
//  Two measured facts shaped the final shape of the detour:
//
//    * The box does not arrive inside the save call. One run had the save files
//      rewritten while no box was seen at all, and the box appeared afterwards,
//      outside any window that could be opened around the call. So the detour
//      cannot be gated on "a save of ours is running" - it would let exactly the
//      box this mod exists to remove straight through. The success box is
//      therefore suppressed whenever it is shown, whoever started the save.
//
//    * The success instance id is used by nothing else in the executable: both
//      references to it sit inside the save handler, next to the failure box.
//      There is no other dialog that this could swallow by mistake.
//
//  The failure box is deliberately left alone: if the save could not be
//  written, the player must be told. When it appears a notice is suppressed
//  afterwards so the two never contradict each other.
//
//  The replacement notice is a small window of its own, carrying the
//  kWinFlagAlwaysInFront flag. Drawing it from a paint hook was rejected after
//  disassembly turned up nothing that answers the one question that matters:
//
//    * Window::OnPaint only draws the fill colour and the drawable, it never
//      paints the caption - so a plain Window with SetCaption() shows nothing.
//    * AddWinProc(..., kEventFlagPaint) is never called for kMsgPaint either;
//      the message goes straight to Window::DoMessage, which tail-jumps into the
//      OnPaint vtable slot.
//    * OnPaint itself is virtual, so hooking it misses any window class that
//      overrides it without chaining to the base.
//    * Window::DoMessage is reached by a direct call from the dispatcher and does
//      see every paint - but it runs for every window, and whether the main
//      window is painted before or after its children cannot be determined from
//      the executable. A notice drawn there might end up under the interface.
//
//  A window with kWinFlagAlwaysInFront has none of that uncertainty. The SDK
//  documents the flag as being rendered after all ordinary siblings ("the window
//  will remain behind of windows with the flag kWinFlagAlwaysInFront set to
//  true", IWindow::BringToFront), so the game's own interface composes it on
//  top and this file never has to reason about paint order at all.
// ----------------------------------------------------------------------------

namespace
{
    /// Instance id shared by the two save result boxes. The key is stored in
    /// the executable as the three dwords 0x478A48AB / 0x00B1B104 / 0xF5F6FDE8,
    /// and only the first of them differs between the success and the failure
    /// box (the failure one is 0x1BF88D5E). Comparing the instance alone is
    /// enough, and it avoids depending on the order of typeID/groupID inside
    /// ResourceKey - the bytes in the image suggest those two are not stored in
    /// the order the header declares.
    const uint32_t kSaveSucceededDialogInstance = 0x478A48AB;
    const uint32_t kSaveFailedDialogInstance    = 0x1BF88D5E;

    /// How long the notice stays on screen, and how long it takes to fade in
    /// and out of it.
    const float kNoticeSeconds     = 3.2f;
    const float kNoticeFadeInSecs  = 0.25f;
    const float kNoticeFadeOutSecs = 1.0f;

    /// Layout of the notice box, in pixels, measured from the top right corner.
    const float kNoticeMarginX = 24.0f;
    const float kNoticeMarginY = 14.0f;
    const float kNoticePaddingX = 12.0f;
    const float kNoticeHeight = 30.0f;

    /// Width of the band over which the plate fades out into the game instead of
    /// ending on a hard edge. The renderer has no blur to sample the screen with,
    /// so the transition is made with alpha gradients: the plate is built out of
    /// a solid core surrounded by four fading bands, which reads as a soft,
    /// out-of-focus frame around the line.
    const float kNoticeFeather = 9.0f;

    /// The save icon drawn left of the text, and the gap between the two.
    const float kNoticeIconSize = 13.0f;
    const float kNoticeIconGap = 8.0f;

    /// Whether the player wants the fading line at all. Comes from the
    /// configuration, and is deliberately sticky: it is a preference, not a
    /// per-save request. See the note on BeginSave() for why that distinction
    /// matters here.
    ///
    /// Starts at 1 so that a save started by the game itself - its own save
    /// button - is reported the same quiet way before this mod has saved
    /// anything. BeginSave() overwrites it from the configuration.
    volatile LONG gNoticeWanted = 1;

    /// Set once the notice has actually been painted. Lets Tick() tell "never
    /// drawn" (the notice window never reached the screen) apart from "drawn
    /// and faded out".
    volatile LONG gNoticeDrawn = 0;

    /// Set once the first paint has been traced into the log. The trace exists
    /// because a crash reported eip=0 on the very first time the notice was
    /// drawn, and the exception report cannot say which call did it.
    volatile LONG gNoticeTraced = 0;

    /// Whether the notice is currently being drawn, and when it started.
    /// The tick is written by the dialog detour and read from Paint(), which is
    /// the one callback that can come from the renderer rather than the main
    /// thread, so it follows the same rule as its neighbours.
    volatile LONG gNoticeActive = 0;
    volatile uint64_t gNoticeStartTick = 0;

    /// Result of the attach call, logged from Initialize().
    LONG gDialogDetourResult = -1;

    /// Guard against detaching hooks that are not there, since Dispose() can run
    /// more than once.
    bool gDetoursAttached = false;

    // -- what the notice says ------------------------------------------------
    //
    // One line, in English, for every language the game can run in.
    //
    // An earlier revision translated it, with a table of nineteen languages and
    // three places to look the language up (an ini override, the game's own
    // registry key, the locale manager). All of that was dropped: the renderer's
    // text call cannot draw a line this small sharply, and with its default face
    // it draws nothing at all for Chinese, so the wording was never the problem
    // worth solving. The line is a picture now - see NoticeGlyphData.hpp - and a
    // picture has to be baked at build time, so it is baked once, in one
    // language, at a size and sharpness this mod chooses.

    float ElapsedNoticeSeconds()
    {
        const uint64_t now = GetTickCount64();
        return float(now - gNoticeStartTick) / 1000.0f;
    }

    float ComputeNoticeAlpha()
    {
        const float elapsed = ElapsedNoticeSeconds();

        if (elapsed >= kNoticeSeconds)
        {
            return 0.0f;
        }

        float alpha = 1.0f;

        if (elapsed < kNoticeFadeInSecs)
        {
            alpha = elapsed / kNoticeFadeInSecs;
        }
        else
        {
            const float remaining = kNoticeSeconds - elapsed;
            if (remaining < kNoticeFadeOutSecs)
            {
                alpha = remaining / kNoticeFadeOutSecs;
            }
        }

        if (alpha < 0.0f) { alpha = 0.0f; }
        if (alpha > 1.0f) { alpha = 1.0f; }
        return alpha;
    }

    // -- the notice window ---------------------------------------------------
    //
    // The notice is drawn by a child window of the main window that carries the
    // kWinFlagAlwaysInFront flag, not by a paint hook. The SDK is explicit about
    // why that flag is the right tool:
    //
    //   "Moves the specified child window at the front of the rest of its
    //    siblings, so it is the last one to be rendered. ... Note that the
    //    window will remain behind of windows with the flag
    //    kWinFlagAlwaysInFront set to true."
    //                                            -- IWindow::BringToFront docs
    //
    // So a window with that flag is rendered after every ordinary sibling, on
    // top of everything, and the ordering is the game's own business rather than
    // something this mod has to reason about. Drawing from a paint hook instead
    // would have left the one question nobody can answer without running the
    // game: whether the main window is painted before or after its children.

    /// Which engine call the guarded paint is currently on, in PaintNotice().
    /// Plain data, because the guard cannot sit in a function that needs
    /// unwinding. 1 = GetGraphics2D, 2 = the plate, 3 = the icon, 4 = the text.
    volatile LONG gNoticeStage = 0;

    /// Draws the notice, behind a guard. Defined after NoticeDrawable so it can
    /// stay a free function: a class method is not allowed to hold a __try.
    bool PaintNotice(UTFWin::UIRenderer* pRenderer, const Math::Rectangle& area,
        float alpha);

    /// Paints the notice. Owned by the notice window, so it is only ever called
    /// when that window is painted, and only when it is actually visible.
    class NoticeDrawable : public UTFWin::DefaultDrawable
    {
    public:
        NoticeDrawable()
        {
            // DefaultDrawable::Release() deletes the object once the count hits
            // zero, and its constructor does not initialise the counter.
            mnRefCount = 1;
        }

        void Paint(UTFWin::UIRenderer* pRenderer, const Math::Rectangle& area,
            const UTFWin::RenderParams& params) override
        {
            (void)params;

            if (pRenderer == nullptr)
            {
                return;
            }

            const float alpha = ComputeNoticeAlpha();
            if (alpha <= 0.0f)
            {
                return;
            }

            // This rectangle is local to the window, not the window's place in
            // the interface: Window::OnPaint hands the drawable (0, 0, width,
            // height), the same way it fills its own background from 0, 0. So
            // the box is simply this rectangle - the window is sized to the
            // notice in UpdateNoticeWindow(), which is what puts it on screen.
            if (area.GetWidth() <= 0.0f || area.GetHeight() <= 0.0f)
            {
                return;
            }

            // Only now is it certain the notice reaches the screen.
            gNoticeDrawn = 1;

            // Every engine call below is logged, but only on this first pass.
            // A crash reported the player's session with eip=0 right after the
            // dialog was suppressed, i.e. on the first time this function ever
            // ran, and the exception report cannot say which call it was. One
            // trace of the first paint names the exact step.
            const bool trace = (gNoticeTraced == 0);
            if (trace)
            {
                gNoticeTraced = 1;
                MOD_LOG("Notice paint: renderer 0x%08X, area %d x %d.",
                    uint32_t(uintptr_t(pRenderer)),
                    int(area.GetWidth()), int(area.GetHeight()));
            }

            if (!PaintNotice(pRenderer, area, alpha) && trace)
            {
                MOD_LOG("Notice paint failed inside the renderer; the notice is off until the next save.");
            }

            if (trace)
            {
                MOD_LOG("Notice paint done.");
            }
        }
    };

    /// True when the vtable slot a Graphics2D call would go through is actually
    /// populated.
    ///
    /// The SDK declares every Graphics2D method as pure virtual and exports no
    /// address for any of them, so a slot is the only way in - and a slot the
    /// game left empty cannot be told apart from a working one until it is
    /// called. Calling it jumps to address zero, which is exactly the fault that
    /// took the session down (an exception report with eip=0 and the DEP
    /// execute bit set). Checking the pointer first turns that into a skipped
    /// line of text.
    bool IsGraphicsSlotPresent(UTFWin::Graphics2D& graphics, size_t offset)
    {
        void** vtable = *reinterpret_cast<void***>(&graphics);
        if (vtable == nullptr)
        {
            return false;
        }

        return vtable[offset / sizeof(void*)] != nullptr;
    }

    /// The engine calls that make the notice, isolated so they can be guarded.
    ///
    /// Everything they touch is a virtual method of a game object, reached
    /// through the vtable the SDK documents. Each slot is checked before it is
    /// called (see IsGraphicsSlotPresent), and the whole sequence also sits
    /// behind __try for anything that cannot be checked up front. A wrong slot
    /// costs the notice, not the session, and the log names the stage it died
    /// in.
    ///
    /// Only plain data is used here, on purpose: a __try block in a function
    /// that needs C++ unwinding does not compile.
    bool PaintNoticeUnsafe(UTFWin::UIRenderer* pRenderer, const Math::Rectangle& area,
        float alpha)
    {
        gNoticeStage = 1;
        UTFWin::Graphics2D& graphics = pRenderer->GetGraphics2D();

        // Slots, in the order Spore\UTFWin\Graphics2D.h declares them:
        // 0x04 SetColor, 0x38 FillRectangle, 0x40 FillGradientHorizontal,
        // 0x44 FillGradientVertical.
        //
        // The notice only ever fills shapes, so these four are all it needs -
        // SetColor and FillRectangle are also confirmed by the game's own
        // Window::OnPaint, which calls both every time it fills a window
        // background. The gradient pair has no such caller, so it is the pair
        // that most needs the check.
        const bool canColor = IsGraphicsSlotPresent(graphics, 0x04);
        const bool canFill = canColor && IsGraphicsSlotPresent(graphics, 0x38);
        const bool canGradient = canColor
            && IsGraphicsSlotPresent(graphics, 0x40)
            && IsGraphicsSlotPresent(graphics, 0x44);

        if (!canFill)
        {
            MOD_LOG("Notice paint: no usable drawing slot in this renderer.");
            return false;
        }

        gNoticeStage = 2;

        // Backing plate. There is no way to sample the screen back and blur it,
        // so the transition to the game is made of alpha instead: a solid core
        // with four fading bands around it, which reads as a soft, out-of-focus
        // frame rather than a hard rectangle sitting on top of the world.
        const float a = alpha;
        const Math::Color plate = Math::Color(uint8_t(10), uint8_t(10), uint8_t(12),
            uint8_t(a * 175.0f));
        const Math::Color clear = Math::Color(uint8_t(10), uint8_t(10), uint8_t(12), uint8_t(0));

        float feather = kNoticeFeather;
        if (feather > area.GetHeight() * 0.5f)
        {
            feather = area.GetHeight() * 0.5f;
        }
        if (feather > area.GetWidth() * 0.25f)
        {
            feather = area.GetWidth() * 0.25f;
        }

        if (canGradient && feather > 0.0f)
        {
            const float innerTop = area.y1 + feather;
            const float innerBottom = area.y2 - feather;
            const float innerLeft = area.x1 + feather;
            const float innerRight = area.x2 - feather;

            // Vertical fade at the top and the bottom, full width.
            graphics.FillGradientVertical(Math::Rectangle(area.x1, area.y1, area.x2, innerTop),
                clear, plate);
            graphics.FillGradientVertical(Math::Rectangle(area.x1, innerBottom, area.x2, area.y2),
                plate, clear);

            // Horizontal fade on the left and the right of the core band.
            graphics.FillGradientHorizontal(Math::Rectangle(area.x1, innerTop, innerLeft, innerBottom),
                clear, plate);
            graphics.FillGradientHorizontal(Math::Rectangle(innerRight, innerTop, area.x2, innerBottom),
                plate, clear);

            graphics.SetColor(plate);
            graphics.FillRectangle(innerLeft, innerTop, innerRight, innerBottom);
        }
        else
        {
            graphics.SetColor(plate);
            graphics.FillRectangle(area.x1, area.y1, area.x2, area.y2);
        }

        gNoticeStage = 3;

        // The icon in front of the text, drawn from rectangles: the renderer can
        // only blit images that already exist as game resources, and there is no
        // "saved" icon among them that reads at this size.
        const uint8_t ink = uint8_t(a * 255.0f);
        const float iconX = area.x1 + kNoticePaddingX;
        const float iconY = area.y1 + (area.GetHeight() - kNoticeIconSize) * 0.5f;
        const float right = iconX + kNoticeIconSize;

        // Body of the disk.
        graphics.SetColor(Math::Color(uint8_t(238), uint8_t(238), uint8_t(238), ink));
        graphics.FillRectangle(iconX, iconY, right, iconY + kNoticeIconSize);

        // The metal shutter at the top and the label at the bottom, both cut
        // out in the plate colour.
        graphics.SetColor(Math::Color(uint8_t(16), uint8_t(16), uint8_t(16), ink));
        graphics.FillRectangle(iconX + 3.5f, iconY + 1.5f, iconX + 9.5f, iconY + 5.0f);
        graphics.FillRectangle(iconX + 2.0f, iconY + 7.0f, iconX + 9.0f, iconY + 11.5f);

        gNoticeStage = 4;

        // The text itself, to the right of the icon.
        // It is drawn as a picture, not through the text call. The text call
        // takes no size and the game's default face is a small aliased bitmap
        // one - measured at thirteen pixels, five grey levels, no smoothing -
        // which is exactly what made the old line look like a low resolution
        // screenshot. So the string is rendered once, at build time, into
        // NoticeGlyphData.hpp: a list of small rectangles, each carrying its
        // own coverage, which the renderer fills as ordinary shapes. That is
        // what gives the line a size of its own and real antialiasing.
        //
        // One SetColor per coverage level rather than per rectangle: the table
        // is sorted by level, so the colour only changes when the level does.
        const float textX = area.x1 + kNoticePaddingX + kNoticeIconSize + kNoticeIconGap;
        const float textY = area.y1 + (area.GetHeight()
            - AutoSaveOnEvent::kNoticeTextHeight) * 0.5f;
        const float textInk = a * 255.0f;

        const size_t count = sizeof(AutoSaveOnEvent::kNoticeGlyph)
            / sizeof(AutoSaveOnEvent::kNoticeGlyph[0]);

        int level = -1;
        for (size_t i = 0; i < count; ++i)
        {
            const AutoSaveOnEvent::NoticeGlyphRect& rect =
                AutoSaveOnEvent::kNoticeGlyph[i];

            if (int(rect.level) != level)
            {
                level = int(rect.level);

                // A coverage of n/(levels-1) over white on the plate, folded
                // into the alpha the renderer blends with.
                const float weight = float(level)
                    / float(AutoSaveOnEvent::kNoticeGlyphLevels - 1);
                graphics.SetColor(Math::Color(uint8_t(255), uint8_t(255), uint8_t(255),
                    uint8_t(ink * weight)));
            }

            const float x1 = textX + float(rect.x);
            const float y1 = textY + float(rect.y);
            graphics.FillRectangle(x1, y1, x1 + float(rect.width), y1 + float(rect.height));
        }

        return true;
    }

    bool PaintNotice(UTFWin::UIRenderer* pRenderer, const Math::Rectangle& area,
        float alpha)
    {
        __try
        {
            return PaintNoticeUnsafe(pRenderer, area, alpha);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // 1 = GetGraphics2D, 2 = the plate, 3 = the icon, 4 = the text.
            MOD_LOG("Notice paint raised 0x%08X at stage %d.",
                uint32_t(GetExceptionCode()), int(gNoticeStage));
            return false;
        }
    }

    /// The window the notice is drawn in. Created on first use and kept for the
    /// lifetime of the mod: an interface window has no cheap way to be told that
    /// its owner is unloading, so it is deliberately never destroyed. It is one
    /// window, and it is hidden whenever there is nothing to show.
    UTFWin::IWindow* gNoticeWindow = nullptr;

    /// The drawable installed in that window. Also never released, for the same
    /// reason: SetDrawable() gives no hint about ownership, and guessing wrong
    /// would either leak or free something still in use.
    NoticeDrawable* gNoticeDrawable = nullptr;

    /// The area the notice is drawn in, derived from the window it belongs to.
    /// Returned rather than drawn directly so Paint() can bail out before
    /// touching the renderer when there is no room for the box.
    Math::Rectangle ComputeNoticeBox(const Math::Rectangle& area)
    {
        // The line's width is known exactly: it is the width of the baked glyph
        // table, not an estimate of a font this code cannot measure.
        const float boxWidth = AutoSaveOnEvent::kNoticeTextWidth + 2.0f * kNoticePaddingX
            + kNoticeIconSize + kNoticeIconGap;

        const float x2 = area.GetWidth() - kNoticeMarginX;
        const float x1 = x2 - boxWidth;
        const float y1 = kNoticeMarginY;
        const float y2 = y1 + kNoticeHeight;

        // A window narrower than the box (or one that has not been laid out yet,
        // which starts at zero size) would put the box off screen.
        if (x1 < 0.0f || area.GetWidth() <= 0.0f)
        {
            return Math::Rectangle(0.0f, 0.0f, 0.0f, 0.0f);
        }

        return Math::Rectangle(x1, y1, x2, y2);
    }

    /// Creates the notice window if it does not exist yet, and makes sure it is
    /// a child of the current main window. Returns null when there is no main
    /// window to attach to (during loading, for instance).
    UTFWin::IWindow* AcquireNoticeWindow()
    {
        // Note: WindowManager is a macro (it expands to
        // (*UTFWin::IWindowManager::Get())), so it cannot be qualified - asking
        // the manager directly is the only spelling that parses.
        UTFWin::IWindowManager* manager = UTFWin::IWindowManager::Get();
        if (manager == nullptr)
        {
            return nullptr;
        }

        UTFWin::IWindow* mainWindow = manager->GetMainWindow();
        if (mainWindow == nullptr)
        {
            return nullptr;
        }

        if (gNoticeWindow == nullptr)
        {
            // Not CreatePanel(): that one sets kWinFlagIgnoreMouse to false, and
            // a window that swallows the mouse would make the corner of the
            // screen unclickable. Start hidden and configure it from scratch.
            gNoticeWindow = new UTFWin::Window();

            // Held for the lifetime of the mod. The children list is an
            // intrusive list, so being somebody's child is not a reference: if
            // the interface disposes its children (it does that when it rebuilds
            // itself, e.g. on a stage change), it releases the object and frees
            // it while gNoticeWindow still points at it.
            //
            // Two references, not one, and neither is ever released. A fresh
            // Window starts at zero, so a single AddRef() would leave the game
            // holding the only reference - the opposite of what is wanted. With
            // two, a release by the interface drops it to one and the object
            // survives, detached, which is exactly what AcquireNoticeWindow()
            // can repair. The cost is one leaked window.
            gNoticeWindow->AddRef();
            gNoticeWindow->AddRef();

            gNoticeWindow->SetControlID(0);
            gNoticeWindow->SetFlag(UTFWin::kWinFlagVisible, false);

            // Left enabled on purpose: the flag is about input, not about
            // painting, and a disabled window is the kind of thing an interface
            // may skip. Ignoring the mouse is what keeps the corner clickable.
            gNoticeWindow->SetFlag(UTFWin::kWinFlagIgnoreMouse, true);
            gNoticeWindow->SetShadeColor(Math::Color(0xFFFFFFFF));

            gNoticeDrawable = new NoticeDrawable();
            gNoticeWindow->SetDrawable(gNoticeDrawable);
        }

        // The main window survives game modes, but not necessarily a reload, so
        // re-parent rather than assume the hierarchy still holds.
        if (gNoticeWindow->GetParent() != mainWindow)
        {
            UTFWin::IWindow* oldParent = gNoticeWindow->GetParent();
            if (oldParent != nullptr)
            {
                oldParent->RemoveWindow(gNoticeWindow);
            }

            mainWindow->AddWindow(gNoticeWindow);
        }

        // The whole point of this window - and it is set only once the window
        // has a parent. SetFlag() routes some flags through the window manager,
        // and a window that is not in a hierarchy yet has no manager to route
        // through. An earlier revision set this before the parenting above,
        // which was one more call into engine state than the flag needs.
        // SetFlag() returns immediately when the flags are already as asked, so
        // repeating it on every call costs nothing.
        gNoticeWindow->SetFlag(UTFWin::kWinFlagAlwaysInFront, true);

        return gNoticeWindow;
    }

    /// AcquireNoticeWindow() behind a guard.
    ///
    /// Creating the window means calling into the interface manager, and that is
    /// engine state this mod does not own: a stage change rebuilds the whole
    /// hierarchy, and the notice can still be asking for a parent while that
    /// happens. A throw there must cost the notice, never the session, so the
    /// whole call sits behind __try and one line in the log says what happened.
    UTFWin::IWindow* AcquireNoticeWindowGuarded()
    {
        __try
        {
            return AcquireNoticeWindow();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            MOD_LOG("Notice window raised 0x%08X; the notice is off until the next save.",
                uint32_t(GetExceptionCode()));
            gNoticeActive = 0;
            return nullptr;
        }
    }

    /// Positions the notice and makes it visible (or hides it, for alpha 0).
    /// Called once per notice, and again every frame while it is on screen.
    void UpdateNoticeWindowUnsafe(bool visible)
    {
        if (!visible)
        {
            if (gNoticeWindow != nullptr)
            {
                gNoticeWindow->SetFlag(UTFWin::kWinFlagVisible, false);
                gNoticeWindow->Invalidate();
            }
            return;
        }

        // Always through AcquireNoticeWindow(), not only when the window is
        // missing or orphaned. It is also what re-parents the window when the
        // interface swaps its main window (a stage change does that), and a
        // window still attached to the *previous* main window would be drawn
        // into a hierarchy nobody is looking at.
        if (AcquireNoticeWindow() == nullptr)
        {
            return;
        }

        UTFWin::IWindowManager* manager = UTFWin::IWindowManager::Get();
        UTFWin::IWindow* mainWindow = (manager != nullptr) ? manager->GetMainWindow() : nullptr;
        if (mainWindow == nullptr)
        {
            return;
        }

        // Copied, not bound: GetArea() hands out a reference into the window.
        const Math::Rectangle mainArea = mainWindow->GetArea();
        const Math::Rectangle box = ComputeNoticeBox(mainArea);
        if (box.GetWidth() <= 0.0f)
        {
            // No room: leave it hidden rather than draw somewhere wrong.
            gNoticeWindow->SetFlag(UTFWin::kWinFlagVisible, false);
            return;
        }

        gNoticeWindow->SetArea(box);
        gNoticeWindow->SetFlag(UTFWin::kWinFlagVisible, true);
        gNoticeWindow->Invalidate();
    }

    /// UpdateNoticeWindowUnsafe() behind a guard.
    ///
    /// Every call in there goes through the interface manager - the same object
    /// a stage change rebuilds - and this runs from Tick(), i.e. from inside the
    /// game's own message loop. An exception there would otherwise take the
    /// session down over a line of text.
    void UpdateNoticeWindow(bool visible)
    {
        __try
        {
            UpdateNoticeWindowUnsafe(visible);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            MOD_LOG("Notice update raised 0x%08X; the notice is off until the next save.",
                uint32_t(GetExceptionCode()));
            gNoticeActive = 0;
        }
    }

    /// Starts the fading notice. Called from the dialog detour, i.e. at the
    /// moment the game itself decided the save was done.
    void StartNotice()
    {
        gNoticeStartTick = GetTickCount64();
        gNoticeActive = 1;
        gNoticeDrawn = 0;

        if (AcquireNoticeWindowGuarded() == nullptr)
        {
            // No main window yet: there is nowhere to put the notice. The save
            // itself already happened, so this only costs the confirmation.
            MOD_LOG("Save notice skipped: no main window to draw it in.");
            gNoticeActive = 0;
            return;
        }

        UpdateNoticeWindow(true);
    }


    // -- replaying the box's result ------------------------------------------
    //
    // Suppressing the box is not the whole job. The save handler runs its own
    // bookkeeping when the box is dismissed, and skipping that bookkeeping
    // breaks saving for the rest of the session: measured behaviour was that
    // the first save went through and then every later one - including the
    // player's own manual save - was skipped silently.
    //
    // The handler was disassembled to confirm that. It keeps a byte that it
    // clears when a save starts and restores only from the code path the
    // answered box leads into:
    //
    //     cmp  byte ptr [0x156C994], 0
    //     je   <skip>                      ; "another save is still settling"
    //     mov  byte ptr [0x156C994], 0     ; a save starts
    //     ...                              ; the box is shown here
    //     mov  byte ptr [0x156C994], 1     ; only reachable via the box
    //
    // The restoration lives in the handler re-entry that the box's callback
    // posts, and it also records the timestamp that the "too soon" check reads.
    // Nothing on this side of ShowDialog reaches either of them, so the byte
    // stays cleared and every save after the first is refused.
    //
    // The fix is to run the callback the box would have run. That callback is
    // the game's own object, so it writes the latch, the timestamps and the
    // follow-up message using its own addresses - which is also what keeps this
    // working on the other executable variant, where none of the offsets
    // disassembled here would be right.
    //
    // The follow-up message matters for more than the latch: a save started from
    // the "save and quit to the main menu / to the desktop" flow does not move on
    // until the box has been answered, so suppressing the box and writing the
    // latch by hand leaves that flow waiting for a click that can never come. The
    // callback is what releases it.

    /// Set once the callback has been replayed at least once, so LogDetourStatus
    /// can say whether the mechanism has been exercised.
    volatile LONG gDialogResultReplayed = 0;

    void ReplayDialogResult(UTFWin::MessageBoxCallback* pCallback)
    {
        const bool haveCallback = pCallback != nullptr;

        if (haveCallback)
        {
            // __try/__except: the callback's first act is a virtual call on the
            // message box singleton. There is no box in this build of the flow -
            // the whole point of the suppression - so that pointer is whatever
            // the previous box left behind. A stale pointer would raise here,
            // and unwinding out of a detour with an exception is not something
            // the game would survive if it reached the handler instead.
            __try
            {
                pCallback->OnButtonPress(nullptr, UTFWin::cSPUIMessageBox::kButtonOK);
                gDialogResultReplayed = 1;
                MOD_LOG("Save dialog result replayed.");
                return;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                MOD_LOG("Save dialog result raised 0x%08X; falling back to the raw latch.",
                    uint32_t(GetExceptionCode()));
            }
        }

        // The callback is either missing or could not run. Do the minimum the
        // handler needs on its own, so that a save that already reached the disk
        // does not also cost the rest of the session. The two addresses are the
        // ones the disassembly above named; both are checked before they are
        // written so that a different executable layout is reported instead of
        // silently corrupted.
        __try
        {
            uint8_t* const gate = reinterpret_cast<uint8_t*>(Address(0x0156C994));
            if (*gate > 1)
            {
                MOD_LOG("Save latch not restored: this executable does not match the layout (0x%02X).", uint32_t(*gate));
                return;
            }

            void* const lastSave = reinterpret_cast<void*>(Address(0x01682208));
            CALL(Address(0x0093A810), void, Args(void*), Args(lastSave));
            *gate = 1;
            gDialogResultReplayed = 1;
            MOD_LOG("Save latch restored directly (callback %s).", haveCallback ? "raised" : "missing");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            MOD_LOG("Restoring the save latch raised 0x%08X; later saves may be skipped.",
                uint32_t(GetExceptionCode()));
        }
    }


    // -- the detour ----------------------------------------------------------

    /// Suppresses the blocking "your game has been saved" box.
    ///
    /// ShowDialog is a __cdecl static method taking (callback, ResourceKey&) -
    /// confirmed by disassembly, the save handler pushes two arguments and then
    /// does `add esp, 8` itself.
    ///
    /// The success box is suppressed unconditionally, not only around a save
    /// this mod started. Two things forced that:
    ///
    ///   - A measured run proved the box does not arrive inside the window
    ///     between the save call and its return. The save files were rewritten
    ///     while the log showed no box at all, and the box appeared later. No
    ///     time window can bound that safely, and a window that is too short
    ///     leaves exactly the dialog this mod exists to remove.
    ///   - The instance id is used by nothing but the save handler (its two
    ///     references in the whole executable are both inside that handler), so
    ///     there is nothing else this could be intercepting.
    ///
    /// Suppressing it for the game's own "save" button too is the consistent
    /// result: every save reports itself the same quiet way.
    static_detour(SaveDialogSuppress__detour, bool(UTFWin::MessageBoxCallback*, const ResourceKey&))
    {
        bool detoured(UTFWin::MessageBoxCallback* pCallback, const ResourceKey& name)
        {
            const uint32_t instance = name.instanceID;

            // Every dialog that passes through here is logged with its full
            // key. A measured run showed the player still getting a "your game
            // has been saved" box while this detour was attached and never
            // fired, which means either the box arrives with a key other than
            // the two known ones, or it is not shown through this function at
            // all. One line per dialog tells those apart; dialogs are rare
            // enough that this costs nothing in a normal session.
            MOD_LOG("ShowDialog 0x%08X 0x%08X 0x%08X.",
                name.instanceID, name.typeID, name.groupID);

            if (instance == kSaveSucceededDialogInstance)
            {
                // Pretend the player dismissed the box. Returning true is what
                // ShowDialog itself returns when the layout loaded and the
                // callback was bound, so the save path sees no error.
                MOD_LOG("Save dialog suppressed.");

                if (gNoticeWanted != 0)
                {
                    StartNotice();
                }

                // The box is gone, but the handler's bookkeeping is not: run
                // what the answered box would have run, or every save after
                // this one is refused. See ReplayDialogResult().
                ReplayDialogResult(pCallback);

                return true;
            }

            if (instance == kSaveFailedDialogInstance)
            {
                // The save failed. Let the game say so: a silent failure would
                // be worse than the box this mod removes. No notice is drawn
                // here, so the player is never told two contradictory things.
                MOD_LOG("Save dialog kept: the save reported a failure.");
            }

            return original_function(pCallback, name);
        }
    };
}


namespace SaveNotification
{
    void AttachDetours()
    {
        gDialogDetourResult = SaveDialogSuppress__detour::attach(
            GetAddress(UTFWin::cSPUIMessageBox, ShowDialog));

        gDetoursAttached = (gDialogDetourResult == NO_ERROR);
    }

    void DetachDetours()
    {
        gNoticeActive = 0;

        // Hiding is not enough before unloading: the window and its drawable
        // carry vtables that live in this DLL. As long as the window is still a
        // child of the interface, the game keeps asking it to paint, and after
        // the module is gone that call lands in unmapped memory. Detach it from
        // the hierarchy so nothing in the game holds a pointer it will call.
        //
        // Done before the early return below on purpose: the window is torn down
        // whether or not the dialog detour managed to attach, because the two
        // are independent and this is the part that must never be skipped.
        if (gNoticeWindow != nullptr)
        {
            gNoticeWindow->SetFlag(UTFWin::kWinFlagVisible, false);

            UTFWin::IWindow* parent = gNoticeWindow->GetParent();
            if (parent != nullptr)
            {
                parent->RemoveWindow(gNoticeWindow);
            }

            // Take the drawable out of the window as well. The window holds a
            // pointer to it and would paint it if anything still asked, and that
            // Paint() is code in this DLL. Clearing it also drops whatever
            // reference SetDrawable() took, leaving only the one NoticeDrawable
            // holds for itself.
            gNoticeWindow->SetDrawable(nullptr);

            // Nothing may reach either object again: both carry vtables from
            // this DLL, and the module is about to disappear.
            gNoticeWindow = nullptr;
            gNoticeDrawable = nullptr;
        }

        if (!gDetoursAttached)
        {
            return;
        }
        gDetoursAttached = false;

        if (gDialogDetourResult == NO_ERROR)
        {
            gDialogDetourResult = SaveDialogSuppress__detour::detach();
        }
    }

    void LogDetourStatus()
    {
        // The notice itself needs no hook: it is an ordinary window that the
        // game's own interface renders. Only the dialog suppression can fail.
        MOD_LOG("Save notification detour: ShowDialog %s (0x%08X).",
            gDialogDetourResult == NO_ERROR ? "attached" : "FAILED",
            uint32_t(gDialogDetourResult));
    }

    void BeginSave(bool showNotice)
    {
        // Only a preference is recorded here. Whether a notice actually appears
        // is decided later, by the save's own result box - see EndSave().
        gNoticeWanted = showNotice ? 1 : 0;

        // gNoticeDrawn is deliberately not reset here. A notice from an earlier
        // save may still be on screen; clearing the flag now would make that one
        // log "expiring without ever being drawn" when it fades, which is the
        // opposite of what happened. It is reset when a new notice starts.
    }

    void EndSave()
    {
        // Nothing is decided here on purpose. The result of the save is reported
        // by the game with a box of its own, arriving whenever the game gets to
        // it - a measured run had the save files written during this call while
        // the box only appeared afterwards. So the detour handles both the
        // suppression and the notice, and this function only exists to close the
        // preference window opened by BeginSave().
    }

    void Tick()
    {
        if (gNoticeActive == 0)
        {
            return;
        }

        // Repositioned every frame, not just once when the notice starts. The
        // main interface lays its children out itself, so a window placed once
        // can be moved back to wherever the layout thinks it belongs; and a
        // paused editor screen can be static enough that nothing would have
        // redrawn the notice anyway.
        UpdateNoticeWindow(true);

        if (ElapsedNoticeSeconds() < kNoticeSeconds)
        {
            return;
        }

        gNoticeActive = 0;
        UpdateNoticeWindow(false);

        // "Never drawn" means the window never reached the screen - the dialog
        // was still suppressed, so this is a missing confirmation, not a broken
        // save. Worth one line in the log.
        if (gNoticeDrawn == 0)
        {
            MOD_LOG("Save notice expiring without ever being drawn.");
        }
    }
}

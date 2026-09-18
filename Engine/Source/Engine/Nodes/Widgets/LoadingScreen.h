#pragma once

#include "Nodes/Widgets/Canvas.h"
#include "Nodes/Widgets/Quad.h"
#include "Nodes/Widgets/Text.h"

// A loading screen: a backdrop, a progress bar, a line of text and an optional logo.
//
// Every project that loads anything on console ends up writing this, because there is nothing to
// look at while a disc read is in progress and a frozen frame is indistinguishable from a hang.
// It lives beside the console and stats overlays so it is available without a project building its
// own, and it is laid out with anchors so it fits whatever resolution it is shown at.
//
// The engine shows it by itself during the startup asset load. A project only has to do anything if
// it wants its own logo or its own wording:
//
//   logo      Config.ini, "LoadingScreenLogo=T_MyGameLogo". Config rather than a call, because the
//             screen is already up before any game code has run. Unset by default, and the engine
//             ships no logo of its own. Console textures must be power-of-two, so pad rather than
//             stretch anything that is not.
//
//   message   Renderer::SetLoadingMessage, or the argument to DrawLoadingFrame. The engine says
//             "Loading..."; a project can say what it is actually doing, per phase.
//
// Note that making this visible before a blocking load is not enough to see it -- loading here is
// synchronous, so nothing renders until the load has already finished. Use
// Renderer::DrawLoadingFrame between the steps of slow work instead.
//
// See Documentation/Info/LoadingScreen.md.
class LoadingScreen : public Canvas
{
public:

    DECLARE_NODE(LoadingScreen, Canvas);

    virtual void Create() override;

    // 0 to 1. Values outside that are clamped, so a caller dividing by a total that turns out to
    // be zero cannot produce a bar wider than the screen.
    void SetProgress(float progress);
    float GetProgress() const { return mProgress; }

    void SetMessage(const char* message);

    // Hides the bar for work whose length is not known up front, leaving just the message.
    void SetBarVisible(bool visible);

    // An image above the bar, usually the game's logo. Sized from the texture's own proportions
    // and the screen's, so a wide logo is not stretched into a square one. Null removes it.
    void SetLogo(class Texture* texture);

protected:

    Quad* mBackdrop = nullptr;
    Quad* mLogo = nullptr;
    Quad* mBarBack = nullptr;
    Quad* mBar = nullptr;
    Text* mText = nullptr;

    float mProgress = 0.0f;
};

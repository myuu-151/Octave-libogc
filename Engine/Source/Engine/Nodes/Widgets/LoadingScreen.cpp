#include "Nodes/Widgets/LoadingScreen.h"

#include "AssetManager.h"
#include "Assets/Font.h"
#include "Utilities.h"

FORCE_LINK_DEF(LoadingScreen);
DEFINE_NODE(LoadingScreen, Canvas);

// Laid out as fractions of the screen rather than in pixels, so the same widget is right on a
// 640x480 television and on a desktop window, and so nothing has to be rebuilt when the resolution
// changes underneath it.
static const float kBarX = 0.25f;
static const float kBarY = 0.62f;
static const float kBarW = 0.50f;
static const float kBarH = 0.030f;

// The border sits a little outside the bar on every side.
static const float kBorder = 0.006f;

void LoadingScreen::Create()
{
    Super::Create();

    SetAnchorMode(AnchorMode::FullStretch);
    SetRatios(0.0f, 0.0f, 1.0f, 1.0f);

    // Children are drawn in the order they are created, so the backdrop goes first and the bar
    // over it.
    mBackdrop = CreateChild<Quad>("LoadBackdrop");
    mBackdrop->SetAnchorMode(AnchorMode::FullStretch);
    mBackdrop->SetRatios(0.0f, 0.0f, 1.0f, 1.0f);
    mBackdrop->SetColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

    mText = CreateChild<Text>("LoadText");
    mText->SetAnchorMode(AnchorMode::FullStretch);
    mText->SetRatios(0.0f, kBarY - 0.10f, 1.0f, 0.07f);
    mText->SetTextSize(28.0f);
    mText->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    mText->SetHorizontalJustification(Justification::Center);
    mText->SetText("Loading...");

    mBarBack = CreateChild<Quad>("LoadBarBack");
    mBarBack->SetAnchorMode(AnchorMode::FullStretch);
    mBarBack->SetRatios(kBarX - kBorder,
                        kBarY - kBorder,
                        kBarW + kBorder * 2.0f,
                        kBarH + kBorder * 2.0f);
    mBarBack->SetColor(glm::vec4(0.22f, 0.22f, 0.24f, 1.0f));

    mBar = CreateChild<Quad>("LoadBar");
    mBar->SetAnchorMode(AnchorMode::FullStretch);
    mBar->SetRatios(kBarX, kBarY, 0.0f, kBarH);
    mBar->SetColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));

    SetProgress(0.0f);
}

void LoadingScreen::SetProgress(float progress)
{
    mProgress = glm::clamp(progress, 0.0f, 1.0f);

    if (mBar != nullptr)
    {
        mBar->SetRatios(kBarX, kBarY, kBarW * mProgress, kBarH);
    }
}

void LoadingScreen::SetMessage(const char* message)
{
    if (mText != nullptr && message != nullptr)
    {
        mText->SetText(message);
    }
}

void LoadingScreen::SetBarVisible(bool visible)
{
    if (mBar != nullptr)
    {
        mBar->SetVisible(visible);
    }

    if (mBarBack != nullptr)
    {
        mBarBack->SetVisible(visible);
    }
}

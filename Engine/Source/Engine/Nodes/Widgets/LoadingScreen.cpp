#include "Nodes/Widgets/LoadingScreen.h"

#include "AssetManager.h"
#include "Assets/Font.h"
#include "Assets/Texture.h"
#include "Engine.h"
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

    // Created before the text so it draws behind it if the two ever overlap.
    mLogo = CreateChild<Quad>("LoadLogo");
    mLogo->SetAnchorMode(AnchorMode::FullStretch);
    mLogo->SetRatios(0.25f, 0.18f, 0.5f, 0.2f);
    mLogo->SetVisible(false);

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

void LoadingScreen::SetLogo(Texture* texture)
{
    if (mLogo == nullptr)
    {
        return;
    }

    mLogo->SetTexture(texture);
    mLogo->SetVisible(texture != nullptr);

    if (texture == nullptr)
    {
        return;
    }

    // Keep the logo's proportions. The widget's ratios are fractions of the screen, so a height
    // expressed as a fraction is stretched by however far the screen is from square; correcting by
    // the screen's aspect is what stops a wide logo being squashed into a tall one.
    const float texWidth = (float)glm::max<uint32_t>(1u, texture->GetWidth());
    const float texHeight = (float)glm::max<uint32_t>(1u, texture->GetHeight());

    const float screenWidth = (float)glm::max<uint32_t>(1u, GetEngineState()->mWindowWidth);
    const float screenHeight = (float)glm::max<uint32_t>(1u, GetEngineState()->mWindowHeight);

    const float widthFrac = 0.55f;
    const float heightFrac = widthFrac * (texHeight / texWidth) * (screenWidth / screenHeight);

    // Sit it above the text, growing upwards, so a taller logo does not push down into the bar.
    const float bottom = kBarY - 0.16f;

    mLogo->SetRatios(0.5f - widthFrac * 0.5f,
                     glm::max(bottom - heightFrac, 0.02f),
                     widthFrac,
                     heightFrac);
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

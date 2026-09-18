# Loading screen

A backdrop, a progress bar, a line of text, and an optional logo. It lives beside the console and
stats overlays: created with the renderer, hidden until something asks for it, and drawn after both
so it covers everything.

The engine puts it up by itself during startup. A project only needs to do anything if it wants its
own logo, its own wording, or a screen over slow work of its own.

## Why a widget on its own is not enough

Loading here is synchronous. It blocks the frame loop, so a screen that is merely made *visible*
before a load is never drawn — nothing renders until the load has already finished and the screen
is about to come down again.

Anything that wants to be seen during a load has to give the renderer a frame to draw:

```cpp
Renderer::Get()->DrawLoadingFrame(progress, "Reticulating splines...");
```

That shows the screen, sets the progress and message, and draws one frame immediately. Call it
between the steps of whatever slow thing is happening. Every few steps rather than every one: a
frame costs far more than most single steps, and that cost lands on the load it is reporting.

This is also how the engine covers its own startup. `AssetManager` draws a frame every fourth asset
it reads from disc while the pump is on, which startup turns on around the scene load:

```cpp
AssetManager::Get()->EnableLoadProgressPump(true);
```

## A project logo

Set in the project's `Config.ini`:

```ini
LoadingScreenLogo=T_MyGameLogo
```

A texture asset name. It has to be config rather than a call from game code, because the startup
loading screen is already up before any game code has run — there is no moment at which a project
could set it in time.

The engine ships no logo of its own. With the key unset, which is the default, no logo is loaded and
the screen is just the bar and the text.

The logo is sized from the texture's own proportions corrected by the screen's, so a wide logo is
not squashed towards square, and it sits above the message and grows upwards so a tall one cannot
push down into the bar.

Textures must be power-of-two on console — GX addresses them that way — so a logo that is not will
fail to import. Pad it rather than stretch it: padding one axis only keeps the artwork spanning the
full width of the quad at its own proportions, with transparent margin on the other axis.

## A custom message

The engine says `"Loading..."` throughout its own startup load. A project overrides it per call:

```cpp
Renderer::Get()->SetLoadingMessage("Setting up the board...");
Renderer::Get()->DrawLoadingFrame(progress, "Setting up the board...");
```

Message and logo are deliberately set in different ways. A logo is one per game and is needed before
any game code runs, so it belongs in config. A message is per phase — different text for different
work — and by the time a project is doing its own loading its code is running and can simply say
what it is doing.

## The rest of it

```cpp
Renderer::Get()->EnableLoadingScreen(true);     // show or hide
Renderer::Get()->SetLoadingProgress(0.4f);      // 0..1, clamped
Renderer::Get()->GetLoadingScreenWidget()->SetBarVisible(false);   // message only
Renderer::Get()->GetLoadingScreenWidget()->SetLogo(texture);       // or set it directly
```

Progress is clamped, so a caller dividing by a total that turns out to be zero cannot draw a bar
wider than the screen. Hide the bar for work whose length is not known up front, leaving the message
on its own.

Everything is laid out in fractions of the screen using anchors rather than in pixels, so the same
widget is right on a 640x480 television and in a desktop window without being rebuilt when the
resolution changes underneath it.

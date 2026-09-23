#if PLATFORM_WINDOWS

#include "Input/Input.h"
#include "Input/InputUtils.h"

#include "Engine.h"
#include "Log.h"

// GAME CONTROLLERS THROUGH SDL2. XInput sees only Xbox-style pads; SDL's game controller layer
// also reads Nintendo Switch Pro controllers and Joy-Cons, PlayStation pads and most others, and
// gives them all one layout. SDL2.dll is LOADED AT RUN TIME, not linked: next to the exe (packaging
// copies it there, ActionManager.cpp) or from External/SDL2 for a build run from the engine's
// folder. Without it everything falls back to XInput, as before. The header is for SDL's types
// only; every function is called through a pointer taken from the DLL.
#define SDL_MAIN_HANDLED
#include "SDL2/include/SDL.h"

namespace
{
    struct SdlApi
    {
        HMODULE mDll = nullptr;
        bool mReady = false;
        decltype(&SDL_SetHint) SetHint = nullptr;
        decltype(&SDL_Init) Init = nullptr;
        decltype(&SDL_Quit) Quit = nullptr;
        decltype(&SDL_NumJoysticks) NumJoysticks = nullptr;
        decltype(&SDL_IsGameController) IsGameController = nullptr;
        decltype(&SDL_JoystickGetDeviceInstanceID) DeviceInstanceID = nullptr;
        decltype(&SDL_GameControllerOpen) Open = nullptr;
        decltype(&SDL_GameControllerClose) Close = nullptr;
        decltype(&SDL_GameControllerGetAttached) GetAttached = nullptr;
        decltype(&SDL_GameControllerGetJoystick) GetJoystick = nullptr;
        decltype(&SDL_JoystickInstanceID) InstanceID = nullptr;
        decltype(&SDL_GameControllerUpdate) Update = nullptr;
        decltype(&SDL_GameControllerGetButton) GetButton = nullptr;
        decltype(&SDL_GameControllerGetAxis) GetAxis = nullptr;
    };

    SdlApi sSdl;
    SDL_GameController* sPads[INPUT_MAX_GAMEPADS] = {};

    template <typename T>
    bool LoadFn(T& fn, const char* name)
    {
        fn = reinterpret_cast<T>(GetProcAddress(sSdl.mDll, name));
        return fn != nullptr;
    }

    void SdlStart()
    {
        sSdl.mDll = LoadLibraryA("SDL2.dll");
        if (sSdl.mDll == nullptr)
        {
            sSdl.mDll = LoadLibraryA("External/SDL2/lib/win64/SDL2.dll");
        }
        if (sSdl.mDll == nullptr)
        {
            LogDebug("SDL2.dll not found: game controllers through XInput only");
            return;
        }

        bool ok = LoadFn(sSdl.SetHint, "SDL_SetHint") && LoadFn(sSdl.Init, "SDL_Init") &&
            LoadFn(sSdl.Quit, "SDL_Quit") && LoadFn(sSdl.NumJoysticks, "SDL_NumJoysticks") &&
            LoadFn(sSdl.IsGameController, "SDL_IsGameController") &&
            LoadFn(sSdl.DeviceInstanceID, "SDL_JoystickGetDeviceInstanceID") &&
            LoadFn(sSdl.Open, "SDL_GameControllerOpen") && LoadFn(sSdl.Close, "SDL_GameControllerClose") &&
            LoadFn(sSdl.GetAttached, "SDL_GameControllerGetAttached") &&
            LoadFn(sSdl.GetJoystick, "SDL_GameControllerGetJoystick") &&
            LoadFn(sSdl.InstanceID, "SDL_JoystickInstanceID") &&
            LoadFn(sSdl.Update, "SDL_GameControllerUpdate") &&
            LoadFn(sSdl.GetButton, "SDL_GameControllerGetButton") &&
            LoadFn(sSdl.GetAxis, "SDL_GameControllerGetAxis");

        if (ok)
        {
            // The window is the engine's, not SDL's: read the pads whatever has the focus, as
            // XInput did. Nintendo pads by the LABEL on the button (A is the right-hand one), so
            // A is A on every pad, as on the GameCube's.
            sSdl.SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            sSdl.SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "1");
            ok = (sSdl.Init(SDL_INIT_GAMECONTROLLER) == 0);
        }

        if (!ok)
        {
            LogWarning("SDL2.dll could not be started: game controllers through XInput only");
            FreeLibrary(sSdl.mDll);
            sSdl = SdlApi();
            return;
        }
        sSdl.mReady = true;
        LogDebug("Game controllers through SDL2");
    }

    void SdlStop()
    {
        if (!sSdl.mReady)
        {
            return;
        }
        for (int32_t i = 0; i < INPUT_MAX_GAMEPADS; ++i)
        {
            if (sPads[i] != nullptr)
            {
                sSdl.Close(sPads[i]);
                sPads[i] = nullptr;
            }
        }
        sSdl.Quit();
        FreeLibrary(sSdl.mDll);
        sSdl = SdlApi();
    }

    float SdlAxis(SDL_GameController* pad, SDL_GameControllerAxis axis)
    {
        float v = (float)sSdl.GetAxis(pad, axis) / 32767.0f;
        return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    }

    // Controllers plugged in are opened into the first free slot, and those pulled out let go.
    void SdlUpdate(InputState& input)
    {
        sSdl.Update();

        for (int32_t i = 0; i < INPUT_MAX_GAMEPADS; ++i)
        {
            if (sPads[i] != nullptr && !sSdl.GetAttached(sPads[i]))
            {
                sSdl.Close(sPads[i]);
                sPads[i] = nullptr;
            }
        }

        int32_t count = sSdl.NumJoysticks();
        for (int32_t d = 0; d < count; ++d)
        {
            if (!sSdl.IsGameController(d))
            {
                continue;
            }
            SDL_JoystickID id = sSdl.DeviceInstanceID(d);
            int32_t freeSlot = -1;
            bool open = false;
            for (int32_t i = 0; i < INPUT_MAX_GAMEPADS; ++i)
            {
                if (sPads[i] == nullptr)
                {
                    if (freeSlot < 0)
                    {
                        freeSlot = i;
                    }
                }
                else if (sSdl.InstanceID(sSdl.GetJoystick(sPads[i])) == id)
                {
                    open = true;
                }
            }
            if (!open && freeSlot >= 0)
            {
                sPads[freeSlot] = sSdl.Open(d);
            }
        }

        for (int32_t i = 0; i < INPUT_MAX_GAMEPADS; ++i)
        {
            GamepadState& pad = input.mGamepads[i];
            SDL_GameController* c = sPads[i];
            pad.mConnected = (c != nullptr);
            if (c == nullptr)
            {
                continue;
            }

            pad.mButtons[GAMEPAD_A] = sSdl.GetButton(c, SDL_CONTROLLER_BUTTON_A);
            pad.mButtons[GAMEPAD_B] = sSdl.GetButton(c, SDL_CONTROLLER_BUTTON_B);
            pad.mButtons[GAMEPAD_X] = sSdl.GetButton(c, SDL_CONTROLLER_BUTTON_X);
            pad.mButtons[GAMEPAD_Y] = sSdl.GetButton(c, SDL_CONTROLLER_BUTTON_Y);
            pad.mButtons[GAMEPAD_L1] = sSdl.GetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
            pad.mButtons[GAMEPAD_R1] = sSdl.GetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
            pad.mButtons[GAMEPAD_THUMBL] = sSdl.GetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK);
            pad.mButtons[GAMEPAD_THUMBR] = sSdl.GetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
            pad.mButtons[GAMEPAD_START] = sSdl.GetButton(c, SDL_CONTROLLER_BUTTON_START);
            pad.mButtons[GAMEPAD_SELECT] = sSdl.GetButton(c, SDL_CONTROLLER_BUTTON_BACK);
            pad.mButtons[GAMEPAD_LEFT] = sSdl.GetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
            pad.mButtons[GAMEPAD_RIGHT] = sSdl.GetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
            pad.mButtons[GAMEPAD_UP] = sSdl.GetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP);
            pad.mButtons[GAMEPAD_DOWN] = sSdl.GetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN);

            // SDL's sticks are down-positive; the engine's (XInput's) are up-positive
            pad.mAxes[GAMEPAD_AXIS_LTRIGGER] = SdlAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            pad.mAxes[GAMEPAD_AXIS_RTRIGGER] = SdlAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
            pad.mAxes[GAMEPAD_AXIS_LTHUMB_X] = SdlAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
            pad.mAxes[GAMEPAD_AXIS_LTHUMB_Y] = -SdlAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
            pad.mAxes[GAMEPAD_AXIS_RTHUMB_X] = SdlAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
            pad.mAxes[GAMEPAD_AXIS_RTHUMB_Y] = -SdlAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);

            pad.mButtons[GAMEPAD_L2] = pad.mAxes[GAMEPAD_AXIS_LTRIGGER] > 0.2f;
            pad.mButtons[GAMEPAD_R2] = pad.mAxes[GAMEPAD_AXIS_RTRIGGER] > 0.2f;
        }
    }
}

void INP_Initialize()
{
    RAWINPUTDEVICE Rid;

    // Mouse
    Rid.usUsagePage = 1;
    Rid.usUsage = 2;
    Rid.dwFlags = 0;
    Rid.hwndTarget = NULL;

    if (!RegisterRawInputDevices(&Rid, 1, sizeof(RAWINPUTDEVICE)))
    {
        LogError("Failed to register RawInput device");
    }

    InputInit();
    SdlStart();
}

void INP_Shutdown()
{
    SdlStop();
    InputShutdown();
}

void INP_Update()
{
    InputAdvanceFrame();

    InputState& input = GetEngineState()->mInput;
    memset(input.mXinputStates, 0, sizeof(XINPUT_STATE) * INPUT_MAX_GAMEPADS);

    if (sSdl.mReady)
    {
        SdlUpdate(input);
        InputPostUpdate();
        return;
    }

    for (int32_t i = 0; i < XUSER_MAX_COUNT && i < INPUT_MAX_GAMEPADS; i++)
    {
        if (!XInputGetState(i, &input.mXinputStates[i]))
        {
            input.mGamepads[i].mConnected = true;

            // Buttons
            input.mGamepads[i].mButtons[GAMEPAD_A] = input.mXinputStates[i].Gamepad.wButtons & XINPUT_GAMEPAD_A;
            input.mGamepads[i].mButtons[GAMEPAD_B] = input.mXinputStates[i].Gamepad.wButtons & XINPUT_GAMEPAD_B;
            input.mGamepads[i].mButtons[GAMEPAD_X] = input.mXinputStates[i].Gamepad.wButtons & XINPUT_GAMEPAD_X;
            input.mGamepads[i].mButtons[GAMEPAD_Y] = input.mXinputStates[i].Gamepad.wButtons & XINPUT_GAMEPAD_Y;
            input.mGamepads[i].mButtons[GAMEPAD_L1] = input.mXinputStates[i].Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER;
            input.mGamepads[i].mButtons[GAMEPAD_R1] = input.mXinputStates[i].Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER;
            input.mGamepads[i].mButtons[GAMEPAD_THUMBL] = input.mXinputStates[i].Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB;
            input.mGamepads[i].mButtons[GAMEPAD_THUMBR] = input.mXinputStates[i].Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB;
            input.mGamepads[i].mButtons[GAMEPAD_START] = input.mXinputStates[i].Gamepad.wButtons & XINPUT_GAMEPAD_START;
            input.mGamepads[i].mButtons[GAMEPAD_SELECT] = input.mXinputStates[i].Gamepad.wButtons & XINPUT_GAMEPAD_BACK;
            input.mGamepads[i].mButtons[GAMEPAD_LEFT] = input.mXinputStates[i].Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT;
            input.mGamepads[i].mButtons[GAMEPAD_RIGHT] = input.mXinputStates[i].Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT;
            input.mGamepads[i].mButtons[GAMEPAD_UP] = input.mXinputStates[i].Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP;
            input.mGamepads[i].mButtons[GAMEPAD_DOWN] = input.mXinputStates[i].Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN;

            // Axes
            input.mGamepads[i].mAxes[GAMEPAD_AXIS_LTRIGGER] = (float)input.mXinputStates[i].Gamepad.bLeftTrigger / 255;
            input.mGamepads[i].mAxes[GAMEPAD_AXIS_RTRIGGER] = (float)input.mXinputStates[i].Gamepad.bRightTrigger / 255;
            input.mGamepads[i].mAxes[GAMEPAD_AXIS_LTHUMB_X] = (float)input.mXinputStates[i].Gamepad.sThumbLX / 32767;
            input.mGamepads[i].mAxes[GAMEPAD_AXIS_LTHUMB_Y] = (float)input.mXinputStates[i].Gamepad.sThumbLY / 32767;
            input.mGamepads[i].mAxes[GAMEPAD_AXIS_RTHUMB_X] = (float)input.mXinputStates[i].Gamepad.sThumbRX / 32767;
            input.mGamepads[i].mAxes[GAMEPAD_AXIS_RTHUMB_Y] = (float)input.mXinputStates[i].Gamepad.sThumbRY / 32767;

            // Set digital inputs for analog triggers
            input.mGamepads[i].mButtons[GAMEPAD_L2] = input.mGamepads[i].mAxes[GAMEPAD_AXIS_LTRIGGER] > 0.2f;
            input.mGamepads[i].mButtons[GAMEPAD_R2] = input.mGamepads[i].mAxes[GAMEPAD_AXIS_RTRIGGER] > 0.2f;
        }
        else
        {
            input.mGamepads[i].mConnected = false;
        }
    }

    InputPostUpdate();
}

void INP_SetCursorPos(int32_t x, int32_t y)
{
    INP_SetMousePosition(x, y);

    POINT screenPoint = { x, y };
    ClientToScreen(GetEngineState()->mSystem.mWindow, &screenPoint);

    SetCursorPos(screenPoint.x, screenPoint.y);
}

void INP_ShowCursor(bool show)
{
    SystemState& system = GetEngineState()->mSystem;

    // Calling ShowCursor(true) twice will cause a sort of ref counting problem,
    // and then future ShowCursor(false) calls won't work.
    if (system.mWindowHasFocus && show != GetEngineState()->mInput.mCursorShown)
    {
        ShowCursor(show);
    }

    GetEngineState()->mInput.mCursorShown = show;
}

void INP_LockCursor(bool lock)
{
    SystemState& system = GetEngineState()->mSystem;
    InputState& input = GetEngineState()->mInput;
    input.mCursorLocked = lock;
}

void INP_TrapCursor(bool trap)
{
    SystemState& system = GetEngineState()->mSystem;
    InputState& input = GetEngineState()->mInput;

    if (system.mWindowHasFocus)
    {
        if (trap)
        {
            RECT rect;
            GetClientRect(system.mWindow, &rect);

            POINT tl;
            tl.x = rect.left;
            tl.y = rect.top;

            POINT br;
            br.x = rect.right;
            br.y = rect.bottom;

            MapWindowPoints(system.mWindow, nullptr, &tl, 1);
            MapWindowPoints(system.mWindow, nullptr, &br, 1);

            rect.left = tl.x;
            rect.top = tl.y;
            rect.right = br.x;
            rect.bottom = br.y;

            ClipCursor(&rect);
        }
        else
        {
            ClipCursor(nullptr);
        }
    }

    input.mCursorTrapped = trap;
}

const char* INP_ShowSoftKeyboard(bool show)
{
    return nullptr;
}

bool INP_IsSoftKeyboardShown()
{
    return false;
}


#endif
package org.libsdl.app;

/**
 * KytyPS5's on-screen controller as an SDL joystick. Lives in SDL's package because the joystick JNI
 * entry points (nativeAddJoystick, onNativeJoy) are package-private there. The pad is added from
 * SDL's own device poll (pollInputDevices, called on the SDL thread every few seconds once the
 * joystick subsystem is up), so it never races SDL initialisation; SDL then generates a gamepad
 * mapping from the button/axis masks and the emulator opens it like any other gamepad.
 */
public final class KytyVirtualPad {

    /** Far from the small positive ids Android assigns to real input devices. */
    public static final int DEVICE_ID = 0x4B595450;

    private static final String NAME = "KytyPS5 Touch Controller";
    private static final String DESCRIPTOR = "kytyps5-touch-controller";

    /** SDL_GAMEPAD_BUTTON_* bits: face 0-3, back 4, start 6, sticks 7-8, shoulders 9-10, d-pad 11-14. */
    private static final int BUTTON_MASK = 0x7FDF;
    private static final int AXIS_COUNT = 6;
    /** Left stick, right stick, both triggers. */
    private static final int AXIS_MASK = 0x3F;

    private static volatile boolean sWanted;
    private static volatile boolean sAdded;
    /**
     * Set one poll (about 3 s) after the add. Until the emulator has opened the gamepad SDL turns pad
     * keys into keyboard events and drops axis motion, so input is held back until then.
     */
    private static volatile boolean sReady;

    private KytyVirtualPad() {
    }

    /** Hooks SDL's device poll. Call after SDLActivity.onCreate, once per game session. */
    public static void install() {
        sAdded = false;
        sReady = false;
        if (!(SDLControllerManager.mJoystickHandler instanceof PollHook)) {
            SDLControllerManager.mJoystickHandler = new PollHook();
        }
    }

    /** Whether the pad should exist; takes effect at SDL's next device poll. */
    public static void setWanted(boolean wanted) {
        sWanted = wanted;
    }

    public static boolean isConnected() {
        return sReady;
    }

    public static void button(int keyCode, boolean down) {
        if (!sReady) {
            return;
        }
        if (down) {
            SDLControllerManager.onNativePadDown(DEVICE_ID, keyCode, 0);
        } else {
            SDLControllerManager.onNativePadUp(DEVICE_ID, keyCode, 0);
        }
    }

    public static void axis(int index, float value) {
        if (sReady) {
            SDLControllerManager.onNativeJoy(DEVICE_ID, index, value);
        }
    }

    private static void sync() {
        if (sWanted && sAdded && !sReady) {
            sReady = true;
        } else if (sWanted && !sAdded) {
            SDLControllerManager.nativeAddJoystick(DEVICE_ID, NAME, DESCRIPTOR, 0, 0, BUTTON_MASK,
                    AXIS_COUNT, AXIS_MASK, 0, false, false, false, false);
            sAdded = true;
        } else if (!sWanted && sAdded) {
            sReady = false;
            SDLControllerManager.nativeRemoveJoystick(DEVICE_ID);
            sAdded = false;
        }
    }

    static final class PollHook extends SDLJoystickHandler {
        @Override
        synchronized void pollInputDevices() {
            super.pollInputDevices();
            sync();
        }
    }
}

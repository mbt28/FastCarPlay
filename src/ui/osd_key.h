#ifndef SRC_UI_OSD_KEY
#define SRC_UI_OSD_KEY

#include <cstdint>

// Navigation keys the on-screen UI understands, independent of where they come
// from (a Bluetooth keyboard today). Kept free of both <linux/input.h> and
// Slint so the key reader and the UI facade can share it.
enum class OsdKey : uint8_t
{
    None,  // no navigation meaning: only the typed character (if any) matters
    Up,
    Down,
    Left,
    Right,
    Ok,    // activate the focused row / confirm
    Back,  // leave the current screen, dialog or editor
    Erase, // delete a character while editing; Back otherwise
    Home,  // straight to the home screen
    Next,  // one rotary detent forward: the next focusable thing, wrapping
    Prev,  // one rotary detent back
};

#endif /* SRC_UI_OSD_KEY */

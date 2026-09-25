#pragma once

#include "hotkey.h"

#include <string>

struct InputHotkeys
{
    Hotkey input;
    Hotkey overlay;
};

// Reads shortcuts from RobloxShadeHost.ini beside the exe, creating the file on first run. Invalid values
// are reported and replaced by the defaults.
InputHotkeys LoadInputHotkeys();

// Folder of RobloxShadeHost.exe, with a trailing backslash.
std::wstring ExeDirectory();

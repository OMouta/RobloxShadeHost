#pragma once

#include <string>

struct Update
{
    std::wstring version; // such as 0.5.0
    std::wstring url;     // the release page
};

// Asks GitHub for the newest release on a background thread.
void CheckForUpdate();

// The newer release, once found. Empty version while checking, when up to date, or when the check failed.
Update AvailableUpdate();

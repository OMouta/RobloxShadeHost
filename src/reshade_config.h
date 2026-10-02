#pragma once

// Adjusts ReShade.ini beside the exe before the add-on registers and ReShade reads the file for the swapchain:
// turns the host's add-on back on if it was disabled, marks ReShade's tutorial as done, since its menu only
// opens from the host's menu, saves screenshots to Pictures\Unishade, uses Segoe UI and the host's
// colors, and turns off Require DLSS in the DLSS5 add-on, unless the user picked their own.
void PrepareReShadeConfig();

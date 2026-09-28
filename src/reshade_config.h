#pragma once

// Adjusts ReShade.ini beside the exe before the add-on registers and ReShade reads the file for the swapchain:
// turns the host's add-on back on if it was disabled, marks ReShade's tutorial as done, since its menu only
// opens from the host's menu, and uses Segoe UI and the host's colors unless the user picked their own.
void PrepareReShadeConfig();

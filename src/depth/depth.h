#pragma once

#include <d3d11.h>

// Loads the depth model from beside the exe and supplies ReShade's DEPTH texture through the host's
// add-on. Returns false when depth is not installed, and logs why when it is installed but unusable.
bool InitDepth();

// Estimates depth for the captured frame on a worker thread and publishes the newest result to ReShade.
// Frames that arrive while an estimate is in progress are skipped.
void UpdateDepth(ID3D11Texture2D* frame);

void ShutdownDepth();

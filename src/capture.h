#pragma once

#include <windows.h>

// D3D11 device shared by the capture pool and the overlay swapchain.
void CreateDevice();

// Starts Windows.Graphics.Capture on the target game window and records it as the target.
void StartCapture(HWND target);

// Ends capture, releases input, and clears the target.
void StopCapture();

// Copies the newest captured frame into the overlay swapchain, creating or resizing it as needed.
void PresentLatestFrame();

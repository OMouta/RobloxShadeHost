---
title: Troubleshooting
description: What the messages in the Unishade window mean, and where the log is.
order: 5
---

The Unishade window lists what it found at startup and any problem since. Most messages say what to do. Only the newest version is supported, so [update](/download/) first.

## ReShade was not found, is too old, or didn't load the add-on

Run **Unishade Setup** from the Start menu and choose **Update or change add-ons**. It installs the ReShade build with full add-on support, which the menu needs.

If you installed without Setup, install ReShade again with full add-on support, pick `Unishade.exe` and choose **DirectX 10/11/12**. ReShade set up for DirectX 9 or OpenGL shows no effects.

## No effects found

Run Setup again to download them.

## A shortcut is in use by another program

Pick another key for it in the menu's **Settings**, or close the other program. If it's the menu's key, the menu can't open, so change `ToggleKey` in `RobloxShadeHost.ini` in Unishade's folder and restart Unishade.

## The game isn't detected

- Run the game in windowed or borderless mode.
- Check that the game is in the Unishade window's list and switched on.
- If the game moved to another folder, remove it and [add it again](/docs/games/#add-a-game).
- Try **Pick a window** in the Unishade window.

Some games block screen capture or overlays, and those may not work.

## Unishade is inside Roblox's folder

Roblox replaces its folder when it updates, which deletes Unishade. Uninstall and install again into another folder.

## Lower frame rate

Effects cost frame rate. Turn off the heaviest ones or pick a lighter preset. Depth estimation costs some too.

## The log

**Open log** in the Unishade window opens `Unishade.log`. The previous run's log is `Unishade.old.log` in the same folder. Setup writes its own log to `%TEMP%\Unishade-Setup.log`.

## Get help

Ask on [Discord](https://discord.gg/wVbVUdENas) and attach the log.

# Installing RobloxShadeHost

You need 64-bit Windows 10 version 1903 or newer, or Windows 11.

## 1. Run Setup

Download **RobloxShadeHost-Setup** from the [latest release](https://github.com/OMouta/RobloxShadeHost/releases/latest) and run it. Keep the suggested folder or pick your own. Don't install it in Roblox's folder, which Roblox replaces when it updates.

![Setup's first page](assets/docs/setup-welcome.png)

## 2. Pick add-ons

ReShade and every effect package on ReShade's official list are always installed. Keep **Presets** on for ready-made looks.

The two add-ons are optional, and they don't work together:

- **Depth estimation** makes depth effects such as ambient occlusion, depth of field and fog work. It costs some frame rate.
- **DLSS5** needs an NVIDIA RTX card. After installing, set the host to use that card, see [DLSS5 setup](DLSS5-README.md).

![Choosing add-ons](assets/docs/setup-addons.png)

Accept ReShade's license on the next page, and Setup downloads and installs everything.

## 3. Start it

Leave **Start RobloxShadeHost now** checked and click **Finish**. Next time, start **RobloxShadeHost** from the Start menu. Roblox can already be open or not; the host waits for it.

![Setup finished](assets/docs/setup-done.png)

The RobloxShadeHost window shows whether it found Roblox and anything that needs fixing. Keep it open while you play. Minimizing is fine; closing it quits the host.

![The RobloxShadeHost window](assets/docs/launcher.png)

## 4. Open the menu in Roblox

Click into Roblox and press **Home**. Pick a preset, or turn effects on and adjust them in **Effects**. Press **Home** or **Escape** to go back to playing. Shortcuts can be changed in **Settings**.

![The menu over Roblox](assets/docs/menu-presets.jpg)

## Updating

We only support the newest version. When a new one is out, the RobloxShadeHost window and the menu's **Status** tab link to it. Run the new Setup; it keeps your presets, settings and shortcuts.

## Uninstalling

Open **RobloxShadeHost Setup** from the Start menu and choose **Uninstall**. Your presets stay unless you tick the box to delete them.

## Installing without Setup

1. Put **RobloxShadeHost.exe** from the [latest release](https://github.com/OMouta/RobloxShadeHost/releases/latest) in its own folder.
2. Run a current ReShade installer from [reshade.me](https://reshade.me/#download), the version with full add-on support. Pick **RobloxShadeHost.exe**, not Roblox, and **DirectX 10/11/12**.
3. Start **RobloxShadeHost.exe**.

The depth and DLSS5 add-ons come with Setup.

## Need help?

Ask on our [Discord](https://discord.gg/wVbVUdENas). Update to the newest version first.

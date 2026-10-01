<p align="center">
  <img src="assets/unishade-banner1.png" alt="Unishade"/>
</p>

<p align="center">
  <a href="https://unishade.me/download/">Download</a>
  &nbsp;·&nbsp;
  <a href="https://unishade.me/docs/">Docs</a>
  &nbsp;·&nbsp;
  <a href="https://discord.gg/wVbVUdENas">Discord</a>
</p>

Unishade puts ReShade effects on games where ReShade can't run. It used to be called RobloxShadeHost.

It never touches the game. Unishade copies the game's window, runs the effects on the copy and draws the result on top. ReShade runs inside Unishade, and nothing gets injected into the game or changed in its files.

## Getting started

1. [Download Setup](https://unishade.me/download/) and run it. You get ReShade, every official effect and a set of presets.
2. Open your game, click **Add game** in Unishade and pick its window.
3. Press **Home** in the game and pick a preset.

Games need to run windowed or borderless. Unishade also runs on [macOS and Linux](https://unishade.me/docs/macos-linux/), where ReShade itself doesn't.

If regular ReShade already works in your game, use that.

Setup isn't code-signed yet, so the first time Windows may say **Windows protected your PC**. Click **More info**, then **Run anyway**. [Releases](https://github.com/OMouta/Unishade/releases) from 0.6.0 on come with `SHA256SUMS.txt`, the SHA-256 of each file, so you can check what you downloaded.

## Help and presets

Ask on [Discord](https://discord.gg/wVbVUdENas). People share their presets there too. The [docs](https://unishade.me/docs/) cover the menu, add-ons and troubleshooting.

To build Unishade or send a preset, see [CONTRIBUTING.md](CONTRIBUTING.md). The macOS and Linux version lives in [src/posix](src/posix/README.md).

Unishade is free. If you want to support it, I'm on [Ko-fi](https://ko-fi.com/omouta).

## License

[MIT](LICENSE)

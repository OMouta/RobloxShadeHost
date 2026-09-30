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

<p align="center">Formerly RobloxShadeHost.</p>

Unishade applies ReShade post-processing externally to supported games, making it useful where traditional ReShade injection is unavailable or undesirable. It runs outside the game process, captures the game image through Windows Graphics Capture, and renders ReShade effects in an overlay, with ReShade itself running inside Unishade’s process. Unishade does not inject DLLs into the game or modify game files as part of its normal architecture.

Unishade also runs on macOS and Linux, where ReShade itself does not. See [macOS and Linux](https://unishade.me/docs/macos-linux/) and [src/posix](src/posix/README.md).

Use normal ReShade where it already works for you. Unishade support depends on the game’s compatibility with external capture and overlays.

## License

Unishade is licensed under the [MIT license](LICENSE).

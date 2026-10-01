---
layout: ../layouts/Legal.astro
title: Privacy
description: Unishade doesn't collect anything about you.
updated: October 1, 2026
---

Unishade doesn't collect anything about you. There are no accounts, analytics or telemetry.

Everything Unishade captures or saves, like screenshots, presets and logs, stays on your computer.

## What Unishade downloads

Unishade and Setup only go online to check for a new version and to download what they need. Like any website you visit, those sites see your IP address.

- Each time it starts, Unishade for Windows asks GitHub (api.github.com) for the newest version. The request sends your IP address and a user agent with Unishade's version, like `Unishade/0.6.0`. **Check for updates** in the menu's **Settings** turns this off.
- Setup downloads ReShade from reshade.me, and ReShade's license, the effects and the presets from GitHub. If you pick an add-on, it comes from GitHub, and depth estimation's model from Hugging Face.
- On macOS and Linux, Unishade downloads the effects and presets from GitHub when you ask it to, with **Download effects and presets** or `unishade --install-effects`.

What those sites do with a request is up to their own privacy policies.

## This website

unishade.me is hosted on GitHub Pages, so GitHub sees your IP address when you visit and handles it as [GitHub's privacy statement](https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement) says. The site itself has no cookies, analytics or trackers. The download buttons link to files on GitHub.

Questions go to [tiago@mouta.me](mailto:tiago@mouta.me).

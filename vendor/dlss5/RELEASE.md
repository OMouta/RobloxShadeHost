# DLSS5 optional downloads

These files are downloaded separately by the Unishade installer when you select the DLSS5 add-on. They are not bundled in the installer executable.

- `renodx-dlss.addon64`: RenoDX / [clshortfuse](https://github.com/clshortfuse). [RenoDX Discord](https://discord.com/invite/renodx).
- `nvngx_dlssnr.dll`: NVIDIA DLSSNR. Copyright 2026 NVIDIA Corporation.

These third-party files are not covered by Unishade's MIT license. Their inclusion does not imply endorsement by their authors.

For removal requests, contact [tiago@mouta.me](mailto:tiago@mouta.me).

The installer reads `downloads.ini` from this release and verifies both SHA-256 checksums before installing the pair. If either download is unavailable or fails verification, it skips DLSS5 and completes the remaining installation.

# iPSX2

iPSX2 is an unofficial PlayStation 2 emulator for iPhone and iPad, ported to Apple silicon from [PCSX2](https://github.com/PCSX2/pcsx2) through [ARMSX2](https://github.com/ARMSX2/ARMSX2).

<a href="https://apps.apple.com/us/app/ipsx2/id6805684708">
  <img src="https://developer.apple.com/assets/elements/badges/download-on-the-app-store.svg" alt="Download on the App Store" height="50">
</a>

## About

- Designed for iPhone and iPad; the App Store version uses interpreter-based emulation rather than runtime code generation.
- PlayStation 2 BIOS and game images are **not included**. Users must provide their own legally obtained files.
- Game-specific, offline-translated VU1 microprogram data (`iv2_t1_*.inc`) is **not included** in this public source tree. Without that optional data, the code falls back to interpreter execution.

## Open source

iPSX2 is a modified derivative of PCSX2 via ARMSX2, **not** an official PCSX2 release. Please report iPSX2-specific issues here, not to the upstream projects.

The source code is distributed under **GPL-3.0-or-later**, subject to applicable third-party component licenses. See [LICENSE](LICENSE), [NOTICE](NOTICE) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

PlayStation is a trademark of Sony Interactive Entertainment. This project is not affiliated with or endorsed by Sony, PCSX2, or ARMSX2.

Apple and the Apple logo are trademarks of Apple Inc., registered in the U.S. and other countries. App Store is a service mark of Apple Inc.

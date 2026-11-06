# iPSX2

iPSX2 is an unofficial PlayStation 2 emulator for iPhone and iPad, built on [PCSX2](https://github.com/PCSX2/pcsx2) and the ARM64 porting efforts of [PCSX2_ARM64](https://github.com/pontos2024/PCSX2_ARM64) and [ARMSX2](https://github.com/ARMSX2/ARMSX2).

<a href="https://apps.apple.com/us/app/ipsx2/id6805684708">
  <img src="https://developer.apple.com/assets/elements/badges/download-on-the-app-store.svg" alt="Download on the App Store" height="50">
</a>

## About

- Designed for iPhone and iPad; the App Store version uses interpreter-based emulation rather than runtime code generation.
- PlayStation 2 BIOS and game images are **not included**. Users must provide their own legally obtained files.
- Game-specific, offline-translated VU1 microprogram data (`iv2_t1_*.inc`) is **not included** in this public source tree. Without that optional data, the code falls back to interpreter execution.

## Credits and project lineage

**[PCSX2](https://github.com/PCSX2/pcsx2) → [PCSX2_ARM64 (Pontos)](https://github.com/pontos2024/PCSX2_ARM64) → [ARMSX2](https://github.com/ARMSX2/ARMSX2) → iPSX2**

- **PCSX2 team:** Created and maintain the PlayStation 2 emulator at the core of this project, developed over many years.
- **Pontos / PCSX2_ARM64 (Android PCSX2):** Pioneered substantial PCSX2-to-ARM64 porting work for Android, including early recompiler translation. This work provided a crucial foundation for subsequent ARM-based projects, including the original ARMSX2 effort.
- **ARMSX2 team:** Built on PCSX2_ARM64 to advance PCSX2 emulation on ARM platforms. iPSX2 was derived from the ARMSX2 codebase.
- **iPSX2 project:** Adapted the codebase for iOS/iPadOS and Apple silicon, with platform integration, an iOS frontend, and interpreter-focused development for App Store distribution.

Many thanks to all upstream developers and contributors. iPSX2 would not exist without their work.

## Open source

iPSX2 is a modified derivative of PCSX2 via ARMSX2, **not** an official PCSX2 release. Please report iPSX2-specific issues here, not to the upstream projects.

The source code is distributed under **GPL-3.0-or-later**, subject to applicable third-party component licenses. See [LICENSE](LICENSE), [NOTICE](NOTICE) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

PlayStation is a trademark of Sony Interactive Entertainment. This project is not affiliated with or endorsed by Sony, PCSX2, or ARMSX2.

Apple and the Apple logo are trademarks of Apple Inc., registered in the U.S. and other countries. App Store is a service mark of Apple Inc.

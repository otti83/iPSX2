// HelpView.swift — practical user guide
// SPDX-License-Identifier: GPL-3.0+
//
// Reached from Settings › About rather than from a tab of its own. What used to be here about
// where files go is now also said at the point it comes up — the library's empty state and the
// game-folder list — so this is the reference copy, not the only copy.
//
// Everything below is checked against what the code actually does. The previous version claimed
// .cue files were supported (nothing in the tree reads one) and that files in Documents/ were
// detected (they were not, until the scan list gained that folder).

import SwiftUI

struct HelpSection: Identifiable {
    let id = UUID()
    let title: String
    let icon: String
    let items: [HelpItem]
}

struct HelpItem: Identifiable {
    let id = UUID()
    let question: String
    let answer: String
}

private let helpData: [HelpSection] = [
    HelpSection(title: "File Placement", icon: "folder", items: [
        HelpItem(
            question: "Where do game files go?",
            answer: "Documents/iso/ — including any subfolders you make inside it — or Documents/ itself. Both are scanned every time the library refreshes. You can add any other folder under Settings › Game Folders."
        ),
        HelpItem(
            question: "Which game formats work?",
            answer: "ISO, BIN, IMG, MDF, CSO, ZSO, GZ and CHD disc images, plus ELF executables. CUE sheets are not read — if you have a BIN/CUE pair, point the app at the BIN."
        ),
        HelpItem(
            question: "Where does the BIOS go?",
            answer: "Documents/bios/, as a .bin or .rom between 1 MB and 50 MB, dumped from a PS2 you own. A BIOS left loose in Documents/ is moved into bios/ for you — tap refresh in the BIOS tab after copying it over. Choose the one to use there."
        ),
        HelpItem(
            question: "Where do cheat files go?",
            answer: "Documents/cheats/, as .pnach files named after the game's serial. The Tweaks tab can import one for you and shows which codes a game has."
        ),
        HelpItem(
            question: "How do I get files onto the device?",
            answer: "Use the Files app — this device's own storage, then iPSX2. Or connect it to a computer: Finder › your device › Files on a Mac, iTunes › File Sharing on Windows."
        ),
        HelpItem(
            question: "I added a game and it is not listed.",
            answer: "Pull down on the Games list, or tap the refresh button, to rescan. If it still does not appear, check that its extension is one of the formats above and that it is in a folder listed under Settings › Game Folders."
        ),
    ]),
    HelpSection(title: "Settings Guide", icon: "gearshape", items: [
        HelpItem(
            question: "IV2 or Interpreter?",
            answer: "IV2 is the default for every component and what this app runs on: it emulates without generating code, which is what App Store rules allow, and it is much faster than the plain Interpreter. Interpreter is slower but the most faithful — switch one component to it if a game misbehaves. Takes effect on the next boot."
        ),
        HelpItem(
            question: "Fast Boot",
            answer: "Skips the PS2 BIOS intro and starts the game directly. A few games need the intro to initialise properly — missing 3D graphics is the usual sign — so turn it off for those."
        ),
        HelpItem(
            question: "Internal Resolution",
            answer: "1x is the PS2's own resolution and the safe choice. 2x and 3x are sharper and cost a lot more GPU time."
        ),
        HelpItem(
            question: "Settings for one game only",
            answer: "The Tweaks tab holds per-game overrides. Anything left alone there follows the global setting, so you only have to set what actually differs."
        ),
    ]),
    HelpSection(title: "On-Screen Display", icon: "chart.bar.xaxis", items: [
        HelpItem(
            question: "Presets and items",
            answer: "A preset in Settings › On-Screen Display turns on a set of items at once; each item can then be switched individually. The position can be top left or top right."
        ),
        HelpItem(
            question: "Turning it off during a game",
            answer: "Tap the menu button in the corner of the screen and switch OSD off. Turning it back on restores the preset you had chosen."
        ),
        HelpItem(
            question: "FPS and VPS",
            answer: "FPS is how fast the game itself is running. VPS is how many frames actually reached the screen. They differ on purpose — a game running internally at 30 can still present at 60."
        ),
    ]),
]

struct HelpView: View {
    var body: some View {
        List {
            ForEach(helpData) { section in
                Section {
                    ForEach(section.items) { item in
                        DisclosureGroup {
                            Text(item.answer)
                                .font(.body)
                                .foregroundStyle(.secondary)
                                .padding(.vertical, 4)
                        } label: {
                            Text(item.question)
                                .font(.body)
                        }
                    }
                } header: {
                    Label(section.title, systemImage: section.icon)
                }
            }
        }
        .navigationTitle("Help")
        .navigationBarTitleDisplayMode(.inline)
    }
}

// GameTweaksView.swift — one game's overrides
// SPDX-License-Identifier: GPL-3.0+
//
// Every row is three-state: it either follows the global setting or carries an override. A plain
// Toggle cannot say "follows the global setting", so each row is a menu whose first item is
// exactly that, and the current global value is shown next to it so the user can see what they
// would be inheriting.
//
// The edit session is opened on appear and closed on disappear. Closing is what writes the file,
// deletes it when nothing is left, and reloads the game if it happens to be the one running.

import SwiftUI

struct GameTweaksView: View {
    let game: GameEntry

    /// Bumped after every write so the body re-reads the bridge. The values live in an INI, not
    /// in Swift state, so there is nothing for @Observable to watch.
    @State private var revision = 0
    @State private var showResetAlert = false

    var body: some View {
        Form {
            Section {
                LabeledContent("Serial", value: game.serial.isEmpty ? "—" : game.serial)
                LabeledContent("CRC", value: String(format: "%08X", game.crc))
            } header: {
                Text("Game")
            } footer: {
                Text("Overrides are filed under this serial and CRC. A different release of the same game keeps its own.")
            }

            ForEach(GameTweaks.groups) { group in
                Section {
                    ForEach(group.settings) { setting in
                        settingRow(setting)
                    }
                } header: {
                    Text(group.title)
                } footer: {
                    if let footer = group.footer { Text(footer) }
                }
            }

            Section {
                NavigationLink {
                    GameCheatsView(game: game, revision: $revision)
                } label: {
                    Label("Cheat Codes", systemImage: "key")
                }
            } footer: {
                Text("Codes come from .pnach files in the cheats folder. Turning a code on here also needs Enable Cheats above, or the global switch.")
            }

            Section {
                Button(role: .destructive) {
                    showResetAlert = true
                } label: {
                    Text("Reset All to Global")
                }
            } footer: {
                Text("Removes every override for this game, including which cheat codes are on. The global settings are untouched.")
            }
        }
        .navigationTitle(game.title)
        .navigationBarTitleDisplayMode(.inline)
        .alert("Reset This Game?", isPresented: $showResetAlert) {
            Button("Cancel", role: .cancel) {}
            Button("Reset", role: .destructive) {
                iPSX2Bridge.clearAllGameSettings()
                revision += 1
            }
        } message: {
            Text("\(game.title) will go back to following every global setting.")
        }
        .onAppear { _ = iPSX2Bridge.openGameSettings(forEntryPath: game.path) }
        .onDisappear { iPSX2Bridge.closeGameSettings() }
    }

    // MARK: - Rows

    @ViewBuilder
    private func settingRow(_ setting: TweakSetting) -> some View {
        let overridden = overrides(setting)
        VStack(alignment: .leading, spacing: 4) {
            Menu {
                Button {
                    iPSX2Bridge.clearGameSettings(setting.section, key: setting.key)
                    revision += 1
                } label: {
                    // A leading "✓ " rather than an empty systemImage — Image(systemName: "")
                    // is not a documented no-op and renders as a gap on some releases.
                    Text(overridden ? "Use Global Setting" : "✓ Use Global Setting")
                }
                Divider()
                ForEach(Array(options(setting).enumerated()), id: \.offset) { _, option in
                    Button {
                        option.apply()
                        revision += 1
                    } label: {
                        Text((overridden && option.isCurrent) ? "✓ \(option.label)" : option.label)
                    }
                }
            } label: {
                HStack {
                    Text(setting.title).foregroundStyle(.primary)
                    Spacer()
                    Text(overridden ? currentLabel(setting) : "Global (\(globalLabel(setting)))")
                        .foregroundStyle(overridden ? .blue : .secondary)
                        .font(overridden ? .body : .callout)
                }
            }
            if let help = setting.help {
                Text(help).font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    // MARK: - Values
    //
    // `revision` is read in each of these so SwiftUI re-evaluates them after a write.

    private func overrides(_ s: TweakSetting) -> Bool {
        _ = revision
        return iPSX2Bridge.gameSettingsHasOverride(s.section, key: s.key)
    }

    private struct Option {
        let label: String
        let isCurrent: Bool
        let apply: () -> Void
    }

    private func options(_ s: TweakSetting) -> [Option] {
        _ = revision
        switch s.kind {
        case .boolean:
            let current = iPSX2Bridge.gameSettingsBool(s.section, key: s.key, defaultValue: false)
            return [true, false].map { value in
                Option(label: value ? "On" : "Off", isCurrent: current == value) {
                    iPSX2Bridge.setGameSettingsBool(s.section, key: s.key, value: value)
                }
            }

        case .integer(let range):
            let current = Int(iPSX2Bridge.gameSettingsInt(s.section, key: s.key, defaultValue: 0))
            return range.map { value in
                Option(label: "\(value)", isCurrent: current == value) {
                    iPSX2Bridge.setGameSettingsInt(s.section, key: s.key, value: Int32(value))
                }
            }

        case .choice(let values, let labels):
            let current = Int(iPSX2Bridge.gameSettingsInt(s.section, key: s.key, defaultValue: 0))
            return zip(values, labels).map { value, label in
                Option(label: label, isCurrent: current == value) {
                    iPSX2Bridge.setGameSettingsInt(s.section, key: s.key, value: Int32(value))
                }
            }

        case .decimal(let range, let step):
            let current = iPSX2Bridge.gameSettingsFloat(s.section, key: s.key, defaultValue: 1.0)
            return stride(from: range.lowerBound, through: range.upperBound, by: step).map { value in
                Option(label: Self.format(value), isCurrent: abs(current - value) < step / 2) {
                    iPSX2Bridge.setGameSettingsFloat(s.section, key: s.key, value: value)
                }
            }
        }
    }

    /// Falls back to the raw stored value rather than an em dash: an override written by hand
    /// (or by an older build with a wider range) is still an override, and showing "—" for it
    /// would read as "not set" when it very much is.
    private func currentLabel(_ s: TweakSetting) -> String {
        if let match = options(s).first(where: \.isCurrent) { return match.label }
        switch s.kind {
        case .boolean:
            return iPSX2Bridge.gameSettingsBool(s.section, key: s.key, defaultValue: false) ? "On" : "Off"
        case .integer, .choice:
            return "\(iPSX2Bridge.gameSettingsInt(s.section, key: s.key, defaultValue: 0))"
        case .decimal:
            return Self.format(iPSX2Bridge.gameSettingsFloat(s.section, key: s.key, defaultValue: 0))
        }
    }

    /// What this key resolves to without an override — read from the base layer, which is
    /// exactly the value being inherited.
    private func globalLabel(_ s: TweakSetting) -> String {
        _ = revision
        switch s.kind {
        case .boolean:
            return iPSX2Bridge.getINIBool(s.section, key: s.key, defaultValue: false) ? "On" : "Off"
        case .integer:
            return "\(iPSX2Bridge.getINIInt(s.section, key: s.key, defaultValue: 0))"
        case .choice(let values, let labels):
            let value = Int(iPSX2Bridge.getINIInt(s.section, key: s.key, defaultValue: 0))
            return zip(values, labels).first { $0.0 == value }?.1 ?? "\(value)"
        case .decimal:
            return Self.format(iPSX2Bridge.getINIFloat(s.section, key: s.key, defaultValue: 1.0))
        }
    }

    private static func format(_ value: Float) -> String {
        value == value.rounded() ? String(format: "%.0f", value) : String(format: "%.1f", value)
    }
}

// SaveStateView.swift — save/load state sheet
// SPDX-License-Identifier: GPL-3.0+
//
// [S2] Shaped for iPhone rather than transplanted from a desktop dialog:
//
//  - It is a sheet with .medium/.large detents over a translucent material, so the game stays
//    visible behind it and a half-height glance is enough to pick a slot. A modal that covers
//    the game would make "which moment am I saving?" unanswerable.
//  - Slots are a horizontal shelf of thumbnail cards. A vertical grid runs out of room in
//    landscape, which is the orientation games actually run in, and thumbnails are the whole
//    point of the screen.
//  - Load and Save are a segmented mode rather than two buttons on every card, so the common
//    action is a single tap on a big target instead of a tap plus a menu choice.
//  - Destructive and rare actions (overwrite, delete, restore backup) are a confirmation
//    dialog and a context menu, which is where iOS users expect them.
//  - Timestamps are relative and localised via RelativeDateTimeFormatter.

import SwiftUI

struct SaveStateSlot: Identifiable {
    let index: Int
    let exists: Bool
    let date: Date?
    let screenshot: UIImage?
    let hasBackup: Bool

    var id: Int { index }

    init(_ d: [String: Any]) {
        index      = (d["index"] as? NSNumber)?.intValue ?? 0
        exists     = (d["exists"] as? NSNumber)?.boolValue ?? false
        date       = d["date"] as? Date
        hasBackup  = (d["hasBackup"] as? NSNumber)?.boolValue ?? false
        if let data = d["screenshot"] as? Data {
            screenshot = UIImage(data: data)
        } else {
            screenshot = nil
        }
    }
}

enum SaveStateMode: String, CaseIterable {
    case load = "Load"
    case save = "Save"
}

struct SaveStateView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var slots: [SaveStateSlot] = []
    @State private var mode: SaveStateMode = .load
    @State private var confirmSlot: SaveStateSlot? = nil
    @State private var deleteSlot: SaveStateSlot? = nil
    @State private var banner: String? = nil

    private static let relative: RelativeDateTimeFormatter = {
        let f = RelativeDateTimeFormatter()
        f.unitsStyle = .abbreviated
        return f
    }()

    var body: some View {
        NavigationStack {
            Group {
                if !iPSX2Bridge.canUseSaveStates() {
                    unavailable
                } else {
                    content
                }
            }
            .navigationTitle("Save States")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .presentationDetents([.medium, .large])
        .presentationDragIndicator(.visible)
        .presentationBackground(.regularMaterial)
        .onAppear(perform: reload)
        .onReceive(NotificationCenter.default.publisher(for: NSNotification.Name("iPSX2SaveStateSaved"))) { _ in
            reload()
            flash("Saved")
        }
        .onReceive(NotificationCenter.default.publisher(for: NSNotification.Name("iPSX2SaveStateLoaded"))) { note in
            let ok = (note.userInfo?["success"] as? NSNumber)?.boolValue ?? false
            if ok {
                dismiss()
            } else {
                flash("Load failed")
            }
        }
        .confirmationDialog(
            confirmSlot.map { "Overwrite Slot \($0.index)?" } ?? "",
            isPresented: Binding(get: { confirmSlot != nil }, set: { if !$0 { confirmSlot = nil } }),
            titleVisibility: .visible
        ) {
            Button("Overwrite", role: .destructive) {
                if let s = confirmSlot { save(s.index) }
                confirmSlot = nil
            }
            Button("Cancel", role: .cancel) { confirmSlot = nil }
        } message: {
            Text("The existing state in this slot will be replaced.")
        }
        .confirmationDialog(
            deleteSlot.map { "Delete Slot \($0.index)?" } ?? "",
            isPresented: Binding(get: { deleteSlot != nil }, set: { if !$0 { deleteSlot = nil } }),
            titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) {
                if let s = deleteSlot {
                    _ = iPSX2Bridge.deleteState(inSlot: Int32(s.index))
                    reload()
                    haptic(.rigid)
                }
                deleteSlot = nil
            }
            Button("Cancel", role: .cancel) { deleteSlot = nil }
        } message: {
            Text("This also removes the slot's backup, and cannot be undone.")
        }
    }

    private var content: some View {
        VStack(spacing: 14) {
            Picker("Mode", selection: $mode) {
                ForEach(SaveStateMode.allCases, id: \.self) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.segmented)
            .padding(.horizontal)

            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 12) {
                    ForEach(slots) { slot in
                        slotCard(slot)
                    }
                }
                .padding(.horizontal)
                .padding(.bottom, 4)
            }

            Text(mode == .load
                 ? "Tap a slot to load it. Touch and hold for more."
                 : "Tap a slot to save into it.")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .padding(.horizontal)

            Spacer(minLength: 0)
        }
        .padding(.top, 8)
        .overlay(alignment: .top) {
            if let banner {
                Text(banner)
                    .font(.subheadline.weight(.semibold))
                    .padding(.horizontal, 14).padding(.vertical, 8)
                    .background(.thinMaterial, in: Capsule())
                    .transition(.move(edge: .top).combined(with: .opacity))
                    .padding(.top, 4)
            }
        }
        .animation(.snappy, value: banner)
    }

    private func slotCard(_ slot: SaveStateSlot) -> some View {
        // Loading an empty slot is impossible, so it reads as disabled rather than failing on tap.
        let enabled = (mode == .save) || slot.exists

        return Button {
            tap(slot)
        } label: {
            VStack(alignment: .leading, spacing: 6) {
                ZStack {
                    if let img = slot.screenshot {
                        Image(uiImage: img).resizable().aspectRatio(contentMode: .fill)
                    } else if slot.exists {
                        // A state written before thumbnails existed. It is perfectly loadable, so
                        // it must not look like an empty slot — solid fill and a document icon,
                        // not the dashed "nothing here" box.
                        RoundedRectangle(cornerRadius: 8)
                            .fill(.quaternary)
                            .overlay {
                                Image(systemName: "doc.fill")
                                    .font(.title2)
                                    .foregroundStyle(.secondary)
                            }
                    } else {
                        RoundedRectangle(cornerRadius: 8)
                            .strokeBorder(style: StrokeStyle(lineWidth: 1.5, dash: [5]))
                            .foregroundStyle(.tertiary)
                            .overlay {
                                Image(systemName: mode == .save ? "plus" : "square.slash")
                                    .font(.title2)
                                    .foregroundStyle(.tertiary)
                            }
                    }
                }
                .frame(width: 148, height: 111)
                .clipShape(RoundedRectangle(cornerRadius: 8))
                .overlay(alignment: .topLeading) {
                    Text("\(slot.index)")
                        .font(.caption2.weight(.bold))
                        .padding(.horizontal, 6).padding(.vertical, 2)
                        .background(.black.opacity(0.55), in: Capsule())
                        .foregroundStyle(.white)
                        .padding(6)
                }

                Text(slot.date.map { Self.relative.localizedString(for: $0, relativeTo: Date()) } ?? "Empty")
                    .font(.caption)
                    .foregroundStyle(slot.exists ? .primary : .secondary)
                    .lineLimit(1)
            }
            .frame(width: 148)
            .opacity(enabled ? 1.0 : 0.4)
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .contextMenu {
            if slot.exists {
                Button {
                    iPSX2Bridge.loadState(fromSlot: Int32(slot.index), backup: false)
                    haptic(.medium)
                } label: { Label("Load", systemImage: "tray.and.arrow.down") }

                Button {
                    confirmSlot = slot
                } label: { Label("Overwrite", systemImage: "tray.and.arrow.up") }

                if slot.hasBackup {
                    Button {
                        iPSX2Bridge.loadState(fromSlot: Int32(slot.index), backup: true)
                        haptic(.medium)
                    } label: { Label("Load Backup", systemImage: "clock.arrow.circlepath") }
                }

                Divider()
                Button(role: .destructive) { deleteSlot = slot } label: {
                    Label("Delete", systemImage: "trash")
                }
            } else {
                Button {
                    save(slot.index)
                } label: { Label("Save Here", systemImage: "tray.and.arrow.up") }
            }
        }
        .accessibilityLabel(slot.exists
            ? "Slot \(slot.index), saved \(slot.date.map { Self.relative.localizedString(for: $0, relativeTo: Date()) } ?? "")"
            : "Slot \(slot.index), empty")
    }

    private var unavailable: some View {
        VStack(spacing: 12) {
            Image(systemName: "tray.slash").font(.system(size: 40)).foregroundStyle(.secondary)
            Text("Save States Unavailable").font(.headline)
            Text("Start a game with a readable disc serial to use save state slots.")
                .font(.footnote).foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .padding()
    }

    private func tap(_ slot: SaveStateSlot) {
        switch mode {
        case .load:
            guard slot.exists else { return }
            iPSX2Bridge.loadState(fromSlot: Int32(slot.index), backup: false)
            haptic(.medium)
        case .save:
            if slot.exists {
                confirmSlot = slot
            } else {
                save(slot.index)
            }
        }
    }

    private func save(_ index: Int) {
        iPSX2Bridge.saveState(toSlot: Int32(index))
        haptic(.medium)
    }

    private func reload() {
        slots = iPSX2Bridge.saveStateSlots().map { SaveStateSlot($0) }
    }

    private func flash(_ text: String) {
        banner = text
        Task {
            try? await Task.sleep(for: .seconds(1.4))
            if banner == text { banner = nil }
        }
    }

    private func haptic(_ style: UIImpactFeedbackGenerator.FeedbackStyle) {
        UIImpactFeedbackGenerator(style: style).impactOccurred()
    }
}

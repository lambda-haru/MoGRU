import AppKit
import Observation
import SwiftUI

/// One usage entry supplied by Mozc's existing renderer protocol.
@objc(MoGRUUsageItem)
public final class MoGRUUsageItem: NSObject, Identifiable {
    @objc public let informationID: Int32
    @objc public let title: String
    @objc public let descriptionText: String

    public var id: Int32 { informationID }

    @objc public init(
        informationID: Int32,
        title: String,
        descriptionText: String
    ) {
        self.informationID = informationID
        self.title = title
        self.descriptionText = descriptionText
    }
}

@MainActor
@Observable
private final class InfolistState {
    var items: [MoGRUUsageItem] = []
    var focusedIndex = -1
}

/// A SwiftUI usage list embedded in MoGRU's renderer window.
@objc(MoGRUInfolistView)
public final class MoGRUInfolistView: NSView {
    private let state: InfolistState
    private lazy var hostingView = NSHostingView(
        rootView: InfolistContentView(state: state)
    )

    public override init(frame frameRect: NSRect) {
        let state = InfolistState()
        self.state = state
        super.init(frame: frameRect)
        hostingView.autoresizingMask = [.width, .height]
        hostingView.frame = bounds
        addSubview(hostingView)
    }

    public required init?(coder: NSCoder) {
        nil
    }

    /// Refreshes the displayed usages and focused entry.
    @objc public func update(
        with items: [MoGRUUsageItem],
        focusedIndex: Int
    ) {
        state.items = items
        state.focusedIndex = focusedIndex
        hostingView.invalidateIntrinsicContentSize()
        hostingView.needsLayout = true
    }

    /// Returns the SwiftUI content size for the surrounding NSPanel.
    @objc public var preferredSize: NSSize {
        hostingView.layoutSubtreeIfNeeded()
        return hostingView.fittingSize
    }
}

private struct InfolistContentView: View {
    let state: InfolistState

    private static let width: CGFloat = 300
    private static let cornerRadius: CGFloat = 12
    private static let focusCornerRadius: CGFloat = 8

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("用例")
                .font(.system(size: 12, weight: .semibold))
                .padding(.horizontal, 8)
                .padding(.vertical, 5)

            Divider()

            ForEach(Array(state.items.enumerated()), id: \.element.id) { index, item in
                usageRow(item, isFocused: index == state.focusedIndex)
            }
        }
        .padding(4)
        .frame(width: Self.width, alignment: .leading)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: Self.cornerRadius))
        .fixedSize(horizontal: false, vertical: true)
    }

    private func usageRow(_ item: MoGRUUsageItem, isFocused: Bool) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(item.title)
                .font(.system(size: 15, weight: .semibold))

            Text(item.descriptionText)
                .font(.system(size: 12))
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background {
            if isFocused {
                RoundedRectangle(cornerRadius: Self.focusCornerRadius)
                    .fill(Color.accentColor.opacity(0.18))
            }
        }
    }
}

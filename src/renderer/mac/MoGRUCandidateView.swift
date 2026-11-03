import AppKit
import Observation
import SwiftUI

/// A candidate supplied by MoGRU's existing renderer protocol.
@objc(MoGRUCandidateItem)
public final class MoGRUCandidateItem: NSObject, Identifiable {
    @objc public let index: Int
    @objc public let candidateID: Int32
    @objc public let text: String
    @objc public let shortcut: String
    @objc public let descriptionText: String
    @objc public let hasSubCandidates: Bool
    
    public var id: Int { index }
    
    /// Creates the display data for one candidate on the current page.
    @objc public init(
        index: Int,
        candidateID: Int32,
        text: String,
        shortcut: String,
        descriptionText: String,
        hasSubCandidates: Bool
    ) {
        self.index = index
        self.candidateID = candidateID
        self.text = text
        self.shortcut = shortcut
        self.descriptionText = descriptionText
        self.hasSubCandidates = hasSubCandidates
    }
    
}

/// Receives mouse selections and forwards them to the existing Mozc session.
@objc(MoGRUCandidateSelectionDelegate)
public protocol MoGRUCandidateSelectionDelegate: AnyObject {
    /// Selects the candidate identified by Mozc's stable candidate ID.
    func candidateView(_ view: MoGRUCandidateView, didSelectCandidateWithID candidateID: Int32)
}

/// Observable state shared by the AppKit host and the SwiftUI candidate list.
@MainActor
@Observable
private final class CandidateListState {
    var candidates: [MoGRUCandidateItem] = []
    var focusedIndex: Int = -1
    var footer = ""
    
    init(
        candidates: [MoGRUCandidateItem] = [],
        focusedIndex: Int = -1,
        footer: String = ""
    ) {
        self.candidates = candidates
        self.focusedIndex = focusedIndex
        self.footer = footer
    }
}

/// A nonactivating candidate view embedded in MoGRU's renderer window.
@objc(MoGRUCandidateView)
public final class MoGRUCandidateView: NSView {
    private let state: CandidateListState
    @objc public var selectionDelegate: MoGRUCandidateSelectionDelegate?
    
    private lazy var hostingView = NSHostingView(
        rootView: CandidateListView(
            state: state,
            selectCandidate: { [weak self] candidateID in
                guard let self else { return }
                self.selectionDelegate?.candidateView(self, didSelectCandidateWithID: candidateID)
            }
        )
    )
    
    
    /// Creates a SwiftUI backed view for the existing AppKit renderer window.
    public override init(frame frameRect: NSRect) {
        let state = CandidateListState()
        self.state = state
        super.init(frame: frameRect)
        hostingView.autoresizingMask = [.width, .height]
        hostingView.frame = bounds
        addSubview(hostingView)
    }
    
    /// Interface Builder is not used by the renderer.
    public required init?(coder: NSCoder) {
        return nil
    }
    
    /// Refreshes the displayed page, focus, and footer from one renderer update.
    @objc public func update(
        with candidates: [MoGRUCandidateItem],
        focusedIndex: Int,
        footer: String
    ) {
        state.candidates = candidates
        state.focusedIndex = focusedIndex
        state.footer = footer
        hostingView.invalidateIntrinsicContentSize()
        hostingView.needsLayout = true
    }
    
    /// Returns the SwiftUI content size for the surrounding NSPanel.
    @objc public var preferredSize: NSSize {
        // Resolve pending SwiftUI layout before positioning the panel.
        hostingView.layoutSubtreeIfNeeded()
        let fitting = hostingView.fittingSize
        return NSSize(
            width: max(300, fitting.width),
            height: min(300, fitting.height)
        )
    }
    
    /// Returns a candidate's current row in AppKit coordinates for a submenu.
    @objc public func frameForCandidate(at index: Int) -> NSRect {
        CandidateListView(state: state)
            .frame(forCandidateAt: index, in: preferredSize) ?? .zero
    }
}

/// Nagi's candidate layout adapted to MoGRU's renderer data.
private struct CandidateListView: View {
    let state: CandidateListState
    let selectCandidate: (Int32) -> Void
    
    private static let animationEnabled = true
    private static let animationDuration = 0.1
    private static let footerEnabled = false
    private static let fontSize: CGFloat = 15
    private static let rowHeight: CGFloat = 27
    private static let maxRows = 10
    private static let dividerHeight: CGFloat = 1
    private static let focusCornerRadius: CGFloat = 8
    
    init(
        state: CandidateListState,
        selectCandidate: @escaping (Int32) -> Void = { _ in }
    ) {
        self.state = state
        self.selectCandidate = selectCandidate
    }
    
    private var visibleCandidates: [MoGRUCandidateItem] {
        let candidates = state.candidates
        guard candidates.count > Self.maxRows else { return candidates }
        let focusedPosition = candidates.firstIndex { $0.index == state.focusedIndex } ?? 0
        let start = min(
            max(0, focusedPosition - Self.maxRows / 2),
            candidates.count - Self.maxRows
        )
        return Array(candidates[start..<(start + Self.maxRows)])
    }

    private var focusedVisiblePosition: Int? {
        visibleCandidates.firstIndex { $0.index == state.focusedIndex }
    }
    
    /// Finds the visible row for a candidate in AppKit's bottom-up coordinates.
    func frame(forCandidateAt index: Int, in size: NSSize) -> NSRect? {
        var topOffset: CGFloat = 4
        for (position, candidate) in visibleCandidates.enumerated() {
            if position > 0 {
                topOffset += Self.dividerHeight
            }
            if candidate.index == index {
                return NSRect(
                    x: 0,
                    y: size.height - topOffset - Self.rowHeight,
                    width: size.width,
                    height: Self.rowHeight
                )
            }
            topOffset += Self.rowHeight
        }
        return nil
    }
    
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Grid(alignment: .leading, verticalSpacing: 0) {
                ForEach(Array(visibleCandidates.enumerated()), id: \.element.id) { position, candidate in
                    if position > 0 {
                        GridRow {
                            Color.clear
                                .frame(height: Self.dividerHeight)
                                .gridCellUnsizedAxes([.horizontal, .vertical])
                            Divider()
                                .frame(height: Self.dividerHeight)
                                .gridCellColumns(2)
                                .gridCellUnsizedAxes(.horizontal)
                                .opacity(
                                    candidate.index == state.focusedIndex ||
                                    visibleCandidates[position - 1].index == state.focusedIndex ? 0 : 0.5
                                )
                        }
                    }

                    GridRow(alignment: .center) {
                        Text(candidate.shortcut.isEmpty ? String(candidate.index + 1) : candidate.shortcut)
                            .font(.caption)
                            .foregroundStyle(.secondary)

                        Text(candidate.text)
                            .font(.system(size: Self.fontSize))
                            .lineLimit(1)
                            .fixedSize(horizontal: true, vertical: false)
                        
                        if candidate.hasSubCandidates {
                                Image(systemName: "chevron.right")
                                    .foregroundStyle(.secondary)
                                    .rotationEffect(.degrees(candidate.index == state.focusedIndex ? 90 : 0))
                                    .padding(.trailing, 5)
                                    .gridCellAnchor(.trailing)
                            
                        } else {
                            Text(candidate.descriptionText)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                                .fixedSize(horizontal: true, vertical: false)
                        }
                    }
                    .frame(height: Self.rowHeight)
                    .contentShape(Rectangle())
                    .onTapGesture { selectCandidate(candidate.candidateID) }
                }
            }
            .padding(.horizontal, 8)
            .background(alignment: .topLeading) {
                if let focusedVisiblePosition {
                    RoundedRectangle(cornerRadius: Self.focusCornerRadius)
                        .fill(Color.accentColor)
                        .frame(height: Self.rowHeight)
                        .offset(
                            y: CGFloat(focusedVisiblePosition) *
                                (Self.rowHeight + Self.dividerHeight)
                        )
                }
            }
            if Self.footerEnabled && !state.footer.isEmpty {
                Text(state.footer)
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
            }
        }
        .padding(4)
        .background(.regularMaterial)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .fixedSize(horizontal: false, vertical: true)
        .animation(
            Self.animationEnabled ? .easeInOut(duration: Self.animationDuration) : nil,
            value: state.focusedIndex
        )
    }
}

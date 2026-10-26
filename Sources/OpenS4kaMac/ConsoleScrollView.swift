import AppKit
import SwiftUI

// Keep the content in SwiftUI's render tree: AppKit-backed ScrollView layers
// are omitted by the launcher's CRT distortion shader.
struct ConsoleScrollView<Content: View>: View {
    let axis: Axis
    var focusedItem: Int? = nil
    var edgeAttempt: ConsoleScrollEdgeAttempt? = nil
    @ViewBuilder let content: () -> Content

    @State private var offset: CGFloat = 0
    @State private var contentSize: CGSize = .zero
    @State private var viewportFrame: CGRect = .zero
    @StateObject private var targets = ConsoleScrollTargets()
    @State private var dragOrigin: CGFloat?
    @State private var wheelOrigin: CGFloat?
    @State private var wheelTravel: CGFloat = 0
    @State private var wheelMonitor: Any?
    @State private var settleTask: Task<Void, Never>?

    private var limit: CGFloat {
        max(0, length(contentSize) - length(viewportFrame.size))
    }

    var body: some View {
        GeometryReader { viewport in
            content()
                .frame(width: axis == .vertical ? viewport.size.width : nil,
                       height: axis == .horizontal ? viewport.size.height : nil,
                       alignment: .topLeading)
                .fixedSize(horizontal: axis == .horizontal, vertical: axis == .vertical)
                .coordinateSpace(name: "console-scroll-content")
                .environmentObject(targets)
                .background {
                    GeometryReader { content in
                        Color.clear
                            .onAppear { contentSize = content.size }
                            .onChange(of: content.size) { size in
                                contentSize = size
                                offset = clamped(offset)
                            }
                    }
                }
                .offset(x: axis == .horizontal ? offset : 0, y: axis == .vertical ? offset : 0)
                .frame(width: viewport.size.width, height: viewport.size.height, alignment: .topLeading)
                .contentShape(Rectangle())
                .clipped()
                .simultaneousGesture(
                    DragGesture(minimumDistance: 8)
                        .onChanged { gesture in
                            settleTask?.cancel()
                            if dragOrigin == nil { dragOrigin = clamped(offset) }
                            let distance = axis == .horizontal ? gesture.translation.width : gesture.translation.height
                            offset = resisted((dragOrigin ?? 0) + distance)
                        }
                        .onEnded { _ in dragOrigin = nil; settle() }
                )
                .onChange(of: focusedItem) { item in
                    guard let item else { settle(to: 0); return }
                    reveal(item)
                }
                .onChange(of: edgeAttempt) { attempt in
                    guard let attempt else { return }
                    bounceAtEdge(direction: attempt.direction)
                }
                .onChange(of: viewport.frame(in: .global)) { frame in
                    viewportFrame = frame
                    offset = clamped(offset)
                }
                .onAppear {
                    viewportFrame = viewport.frame(in: .global)
                    startWheelMonitor()
                }
                .onDisappear {
                    settleTask?.cancel()
                    if let wheelMonitor { NSEvent.removeMonitor(wheelMonitor) }
                    wheelMonitor = nil
                }
                .accessibilityAction(named: Text(axis == .vertical ? "Scroll down" : "Scroll right")) {
                    settle(to: offset - length(viewportFrame.size) * 0.7)
                }
                .accessibilityAction(named: Text(axis == .vertical ? "Scroll up" : "Scroll left")) {
                    settle(to: offset + length(viewportFrame.size) * 0.7)
                }
        }
    }

    private func length(_ size: CGSize) -> CGFloat { axis == .horizontal ? size.width : size.height }
    private func clamped(_ value: CGFloat) -> CGFloat { min(0, max(-limit, value)) }

    private func resisted(_ value: CGFloat) -> CGFloat {
        let bounded = clamped(value)
        let overflow = value - bounded
        // Keep the rubber-band visible even for a short trackpad gesture. This
        // is especially important when content is smaller than the viewport
        // (for example, the library currently contains one game).
        return bounded + overflow / (1 + abs(overflow) / 90) * 0.68
    }

    private func settle(to value: CGFloat? = nil) {
        wheelOrigin = nil
        wheelTravel = 0
        withAnimation(.spring(response: 0.32, dampingFraction: 0.82)) {
            offset = clamped(value ?? offset)
        }
    }

    private func bounceAtEdge(direction: Int) {
        settleTask?.cancel()
        let anchor = clamped(offset)
        let distance = min(52, max(34, length(viewportFrame.size) * 0.055))
        let target = resisted(anchor + CGFloat(direction >= 0 ? 1 : -1) * distance)
        withAnimation(.easeOut(duration: 0.12)) {
            offset = target
        }
        settleTask = Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(120))
            guard !Task.isCancelled else { return }
            withAnimation(.spring(response: 0.28, dampingFraction: 0.62)) {
                offset = anchor
            }
        }
    }

    private func reveal(_ item: Int) {
        guard let target = targets.frames[item] else { return }
        let start = axis == .horizontal ? target.minX : target.minY
        let end = axis == .horizontal ? target.maxX : target.maxY
        let visibleLength = length(viewportFrame.size)
        if start + offset < 8 { settle(to: 8 - start) }
        else if end + offset > visibleLength - 8 { settle(to: visibleLength - 8 - end) }
    }

    private func startWheelMonitor() {
        guard wheelMonitor == nil else { return }
        wheelMonitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { event in
            let handled = MainActor.assumeIsolated {
                guard NSApp.isActive, NSApp.modalWindow == nil,
                      let window = event.window, window.attachedSheet == nil,
                      let view = window.contentView else { return false }
                var point = view.convert(event.locationInWindow, from: nil)
                if !view.isFlipped { point.y = view.bounds.height - point.y }
                guard viewportFrame.contains(point) else { return false }
                let delta = axis == .horizontal
                    ? (abs(event.scrollingDeltaX) > abs(event.scrollingDeltaY) ? event.scrollingDeltaX : event.scrollingDeltaY)
                    : event.scrollingDeltaY
                let scale: CGFloat = event.hasPreciseScrollingDeltas ? 1 : 16
                if event.phase.contains(.began) || event.momentumPhase.contains(.began) || wheelOrigin == nil {
                    wheelOrigin = clamped(offset)
                    wheelTravel = 0
                }
                wheelTravel += delta * scale
                offset = resisted((wheelOrigin ?? clamped(offset)) + wheelTravel)
                settleTask?.cancel()
                settleTask = Task { @MainActor in
                    try? await Task.sleep(for: .milliseconds(120))
                    guard !Task.isCancelled else { return }
                    settle()
                }
                return true
            }
            return handled ? nil : event
        }
    }
}

struct ConsoleScrollEdgeAttempt: Equatable {
    let sequence: Int
    /// Direction of the content's rubber-band movement: +1 shifts right/down,
    /// -1 shifts left/up.
    let direction: Int
}

@MainActor private final class ConsoleScrollTargets: ObservableObject {
    var frames: [Int: CGRect] = [:]
}

private struct ConsoleScrollTarget: ViewModifier {
    let id: Int
    @EnvironmentObject private var targets: ConsoleScrollTargets

    func body(content: Content) -> some View {
        content.background {
            GeometryReader { geometry in
                let frame = geometry.frame(in: .named("console-scroll-content"))
                Color.clear
                    .onAppear { targets.frames[id] = frame }
                    .onChange(of: frame) { targets.frames[id] = $0 }
                    .onDisappear { targets.frames.removeValue(forKey: id) }
            }
        }
    }
}

extension View {
    func consoleScrollTarget(_ id: Int) -> some View {
        modifier(ConsoleScrollTarget(id: id))
    }
}

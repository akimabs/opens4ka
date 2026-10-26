import SwiftUI

struct CRTScreen: ViewModifier {
    let size: CGSize

    func body(content: Content) -> some View {
        curved(content)
            .background(.black)
            .clipShape(RoundedRectangle(cornerRadius: 28))
            .overlay {
                ZStack {
                    LinearGradient(stops: [
                        .init(color: .white.opacity(0.07), location: 0),
                        .init(color: .white.opacity(0.015), location: 0.22),
                        .init(color: .clear, location: 0.45),
                        .init(color: .black.opacity(0.1), location: 1)
                    ], startPoint: .topLeading, endPoint: .bottomTrailing)
                    RoundedRectangle(cornerRadius: 28).strokeBorder(.black.opacity(0.9), lineWidth: 9)
                    RoundedRectangle(cornerRadius: 25).inset(by: 5)
                        .stroke(.white.opacity(0.09), lineWidth: 1)
                }
                .allowsHitTesting(false)
            }
    }

    @ViewBuilder private func curved(_ content: Content) -> some View {
        if #available(macOS 14.0, *), let library = CRTShader.library {
            content.compositingGroup()
                .distortionEffect(library.crtCurve(.float2(Float(size.width), Float(size.height)), .float(0.045)),
                                  maxSampleOffset: CGSize(width: size.width * 0.06, height: size.height * 0.06))
        } else {
            content
        }
    }
}

@available(macOS 14.0, *)
private enum CRTShader {
    static let library: ShaderLibrary? = {
        let packaged = Bundle.main.resourceURL?.appendingPathComponent("OpenS4kaMac_OpenS4kaMac.bundle/CRT.metallib")
        if let packaged, FileManager.default.fileExists(atPath: packaged.path) {
            return ShaderLibrary(url: packaged)
        }
        let development = Bundle.module.bundleURL.appendingPathComponent("CRT.metallib")
        return FileManager.default.fileExists(atPath: development.path) ? ShaderLibrary(url: development) : nil
    }()
}

// Quét mã QR cài đặt máy chủ chỉ đường (VisionKit). Mã hợp lệ: navesp://server?url=...&key=...
import SwiftUI
import VisionKit

struct QRScannerView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var message: String?

    var body: some View {
        NavigationStack {
            Group {
                if DataScannerViewController.isSupported && DataScannerViewController.isAvailable {
                    Scanner { text in
                        if let u = URL(string: text), RouteSettings.shared.apply(u) {
                            dismiss()
                        } else {
                            message = "Mã QR không phải mã cài đặt NavESP"
                        }
                    }
                    .ignoresSafeArea()
                } else {
                    ContentUnavailableView("Không quét được", systemImage: "qrcode.viewfinder",
                                           description: Text("Máy không hỗ trợ quét, hoặc chưa cho phép dùng camera."))
                }
            }
            .safeAreaInset(edge: .bottom) {
                if let message {
                    Text(message).font(.callout).padding(10)
                        .background(.regularMaterial, in: Capsule()).padding(.bottom, 12)
                }
            }
            .navigationTitle("Quét mã QR")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Huỷ") { dismiss() } } }
        }
    }

    private struct Scanner: UIViewControllerRepresentable {
        let onCode: (String) -> Void

        func makeUIViewController(context: Context) -> DataScannerViewController {
            let vc = DataScannerViewController(recognizedDataTypes: [.barcode(symbologies: [.qr])],
                                               isHighlightingEnabled: true)
            vc.delegate = context.coordinator
            try? vc.startScanning()
            return vc
        }

        func updateUIViewController(_ vc: DataScannerViewController, context: Context) {}

        func makeCoordinator() -> Coordinator { Coordinator(onCode: onCode) }

        final class Coordinator: NSObject, DataScannerViewControllerDelegate {
            let onCode: (String) -> Void
            init(onCode: @escaping (String) -> Void) { self.onCode = onCode }

            func dataScanner(_ s: DataScannerViewController, didAdd items: [RecognizedItem], allItems: [RecognizedItem]) {
                for case .barcode(let b) in items {
                    if let v = b.payloadStringValue { onCode(v); return }
                }
            }
        }
    }
}

import SwiftUI

@main
struct NavESPApp: App {
    init() {
        // Tạo bộ điều phối + BLE ngay lúc khởi động, kể cả khi iOS mở app ở nền để khôi phục kết nối BLE
        AppLog.write("App khoi dong (\(BLEManager.appState))")
        _ = NavController.shared
        let nc = NotificationCenter.default
        nc.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) { _ in AppLog.write("App vao nen") }
        nc.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil, queue: .main) { _ in AppLog.write("App mo lai") }
        nc.addObserver(forName: UIApplication.willTerminateNotification, object: nil, queue: .main) { _ in AppLog.write("App sap bi tat") }
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .onOpenURL { RouteSettings.shared.apply($0) }      // liên kết navesp://server?... (mở từ mã QR)
        }
    }
}

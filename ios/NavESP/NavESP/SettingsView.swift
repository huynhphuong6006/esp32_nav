// Trang cài đặt: kết nối với thiết bị, chế độ hiển thị, máy chủ chỉ đường.
import SwiftUI

struct SettingsView: View {
    @ObservedObject private var ble = NavController.shared.ble
    @ObservedObject private var nav = NavController.shared
    @Environment(\.dismiss) private var dismiss
    @State private var confirmForget = false
    @ObservedObject private var route = RouteSettings.shared
    @State private var scanning = false
    @State private var testResult: String?
    @State private var testing = false


    var body: some View {
        NavigationStack {
            Form {
                connectionSection
                displaySection
                routeSection
            }
            .navigationTitle("Cài đặt")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Xong") { dismiss() }
                }
            }
        }
        .task {                                   // RSSI trong lúc mở trang này
            while !Task.isCancelled {
                ble.readRSSI()
                try? await Task.sleep(for: .seconds(2))
            }
        }
        .onDisappear { ble.stopListing() }
    }

    // MARK: Máy chủ chỉ đường

    private var routeSection: some View {
        Section {
            Toggle("Dùng máy chủ riêng", isOn: $route.useCustom)
            if route.useCustom {
                TextField("Địa chỉ, ví dụ https://route.example.com", text: $route.url)
                    .keyboardType(.URL)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                SecureField("Khoá truy cập", text: $route.key)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button {
                    testing = true
                    testResult = nil
                    Task {
                        switch await RouteService.testCustom() {
                        case .success(let s): testResult = String(format: "Kết nối được (%.1f giây)", s)
                        case .failure(let e): testResult = "Lỗi: \(e.localizedDescription)"
                        }
                        testing = false
                    }
                } label: {
                    HStack {
                        Text("Thử máy chủ")
                        Spacer()
                        if testing { ProgressView() } else if let testResult { Text(testResult).foregroundStyle(.secondary).font(.callout) }
                    }
                }
                .disabled(testing || route.custom == nil)
            }
            Button("Quét mã QR") { scanning = true }
        } header: {
            Text("Máy chủ chỉ đường")
        } footer: {
            Text(route.useCustom
                 ? "Máy chủ riêng lỗi hoặc chậm quá 4 giây thì app tạm dùng máy chủ demo (tính đường cho ô tô, có thể đi cao tốc)."
                 : "Đang dùng máy chủ demo công cộng: tính đường cho ô tô, có thể đi cao tốc. Có máy chủ riêng (tự host hoặc được mời) thì bật ở trên hoặc quét mã QR.")
        }
        .sheet(isPresented: $scanning) { QRScannerView() }
    }

    // MARK: Chế độ hiển thị

    private var displaySection: some View {
        Section {
            Picker("Chế độ", selection: $nav.displayMode) {
                ForEach(NavController.DisplayMode.allCases) { Text($0.title).tag($0) }
            }
            .pickerStyle(.segmented)
        } header: {
            Text("Chế độ hiển thị")
        } footer: {
            Text(displayFooter)
        }
    }

    private var displayFooter: String {
        guard nav.displayMode == .auto, let t = nav.sunTimes else {
            return "Thiết bị giữ chế độ đã đặt, khởi động lại vẫn giữ."
        }
        let f = DateFormatter()
        f.dateFormat = "HH:mm"
        return "Hôm nay: mặt trời mọc \(f.string(from: t.sunrise)), lặn \(f.string(from: t.sunset)). Thiết bị chỉ đổi chế độ khi kết nối với app."
    }

    // MARK: Kết nối

    private var connectionSection: some View {
        Section {
            LabeledContent("Trạng thái") {
                Text(linkText).foregroundStyle(ble.link == .connected ? .green : .secondary)
            }
            if ble.hasDevice {
                LabeledContent("Thiết bị", value: ble.deviceName ?? "NavESP")
            }
            if ble.link == .connected, let r = ble.rssi {
                LabeledContent("Tín hiệu (RSSI)", value: "\(r) dBm")
            }
            Toggle("Tự kết nối lại", isOn: $ble.autoConnect)
            if ble.link == .connected || ble.link == .connecting {
                Button("Ngắt kết nối") { ble.disconnectNow() }
            } else if ble.link == .idle, ble.hasDevice {
                Button("Kết nối") { ble.connectNow() }
            }
            if ble.link != .off {
                Button(ble.listing ? "Dừng quét" : "Quét thiết bị") {
                    if ble.listing { ble.stopListing() } else { ble.startListing() }
                }
            }
            if ble.listing || !ble.found.isEmpty {
                ForEach(ble.found) { item in
                    Button { ble.connect(to: item) } label: {
                        LabeledContent(item.name, value: "\(item.rssi) dBm")
                    }
                    .foregroundStyle(.primary)
                }
                if ble.listing && ble.found.isEmpty {
                    HStack(spacing: 8) {
                        ProgressView()
                        Text("Đang quét…").foregroundStyle(.secondary)
                    }
                }
            }
            if ble.hasDevice {
                Button("Quên thiết bị", role: .destructive) { confirmForget = true }
                    .confirmationDialog("Quên thiết bị này?", isPresented: $confirmForget, titleVisibility: .visible) {
                        Button("Quên thiết bị", role: .destructive) { ble.forgetDevice() }
                    } message: {
                        Text("App sẽ thôi tự kết nối lại với thiết bị này. Muốn kết nối lại thì bấm Quét thiết bị rồi chọn.")
                    }
            }
        } header: {
            Text("Kết nối")
        } footer: {
            Text("Ngắt kết nối không dừng dẫn đường: thiết bị vẫn chạy theo tuyến đã có, nhưng app không tính lại đường được khi lệch tuyến.")
        }
    }

    private var linkText: String {
        switch ble.link {
        case .off: return "Bluetooth tắt"
        case .idle: return "Chưa kết nối"
        case .scanning: return "Đang tìm NavESP…"
        case .connecting: return "Đang kết nối…"
        case .connected: return "Đã kết nối"
        }
    }
}

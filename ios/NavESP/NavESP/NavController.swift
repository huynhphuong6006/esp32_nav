// Điều phối: điểm đến, tính đường, gửi xuống thiết bị, tự tính lại đường khi thiết bị báo lệch tuyến.
// Chạy cả khi app ở nền/khoá máy (iOS đánh thức app khi có trạng thái BLE): tính lại đường từ vị trí GPS của thiết bị,
// gửi lại tuyến nếu thiết bị khởi động lại mất tuyến. Điểm đến và trạng thái dẫn đường được lưu để dùng khi app bị mở lại.
import Combine
import CoreLocation
import Foundation
import MapKit
import UIKit

@MainActor
final class NavController: NSObject, ObservableObject, CLLocationManagerDelegate {
    /// Tạo ngay khi app khởi động (kể cả khi iOS mở app ở nền để khôi phục BLE)
    static let shared = NavController()

    @Published var destination: CLLocationCoordinate2D?
    @Published var destinationName = ""
    @Published private(set) var route: PlannedRoute?
    @Published private(set) var busy = false
    @Published var message: String?
    @Published private(set) var userLocation: CLLocation?
    @Published private(set) var rerouteCount = 0

    enum DisplayMode: String, CaseIterable, Identifiable {
        case auto, day, night
        var id: String { rawValue }
        var title: String {
            switch self {
            case .auto: return "Tự động"
            case .day: return "Ngày"
            case .night: return "Đêm"
            }
        }
    }
    @Published var displayMode: DisplayMode = DisplayMode(rawValue: UserDefaults.standard.string(forKey: "NavESP.displayMode") ?? "") ?? .auto {
        didSet {
            UserDefaults.standard.set(displayMode.rawValue, forKey: "NavESP.displayMode")
            lastThemeSend = .distantPast
            syncTheme()
        }
    }
    private var lastThemeSend = Date.distantPast

    /// Vị trí để tính mặt trời: iPhone, không thì thiết bị, không thì Buôn Ma Thuột
    private var sunCoordinate: CLLocationCoordinate2D {
        if let c = userLocation?.coordinate { return c }
        if let st = ble.status, st.lat != 0 { return CLLocationCoordinate2D(latitude: st.lat, longitude: st.lon) }
        return CLLocationCoordinate2D(latitude: 12.6874, longitude: 108.0449)
    }
    var sunTimes: SunTimes? { SunTimes.compute(date: Date(), coordinate: sunCoordinate) }

    var wantNight: Bool {
        switch displayMode {
        case .day: return false
        case .night: return true
        case .auto: return SunTimes.isNight(date: Date(), coordinate: sunCoordinate)
        }
    }

    /// Gửi giao diện nếu thiết bị đang khác. Gọi theo status BLE (mỗi 2 giây, cả khi app ở nền) và vị trí,
    /// không dùng timer vì timer không chạy được ở nền.
    private func syncTheme() {
        guard ble.link == .connected, let st = ble.status else { return }
        let want = wantNight
        guard st.night != want, Date().timeIntervalSince(lastThemeSend) >= 3 else { return }
        lastThemeSend = Date()
        log("Dat giao dien \(want ? "dem" : "ngay")")
        ble.setTheme(night: want)
    }

    let ble = BLEManager()
    private let location = CLLocationManager()
    private var subs = Set<AnyCancellable>()
    private var offRouteSince: Date?
    private var lastReroute = Date.distantPast
    private var lastResend = Date.distantPast
    /// CRC tuyến đã gửi thành công gần nhất (= tuyến đang dẫn trên thiết bị)
    @Published private(set) var sentCRC: UInt32?
    /// App đang dẫn đường (đã bấm Bắt đầu, chưa Dừng/Xoá); giữ nguyên khi tính lại đường
    @Published private(set) var navigating = false {
        didSet { UserDefaults.standard.set(navigating, forKey: "NavESP.navigating") }
    }
    @Published private(set) var rerouting = false

    /// Tuyến xem trước khác tuyến đang dẫn (vừa chọn điểm đến mới) -> nút "Đổi tuyến"
    var routeChanged: Bool {
        guard let r = route, let c = sentCRC else { return route != nil }
        return CRC32.checksum(r.rte1) != c
    }

    /// Ghi log ra console và file Documents/NavESP-log.txt (xem bằng app Tệp hoặc Finder khi cắm máy)
    private func log(_ m: String) {
        print("[NavESP] \(m)")
        AppLog.write(m)
    }

    static let rerouteAfter: TimeInterval = 5       // lệch tuyến liên tục bấy lâu thì tính lại
    static let rerouteEvery: TimeInterval = 20      // không tính lại dày hơn mức này

    override init() {
        super.init()
        location.delegate = self
        location.desiredAccuracy = kCLLocationAccuracyBest
        location.requestWhenInUseAuthorization()
        location.startUpdatingLocation()
        ble.objectWillChange.sink { [weak self] in self?.objectWillChange.send() }.store(in: &subs)
        ble.$status.sink { [weak self] st in self?.onStatus(st) }.store(in: &subs)
        ble.$link.removeDuplicates().sink { [weak self] l in self?.log("BLE: \(l)") }.store(in: &subs)
        restore()
        #if DEBUG
        // Thử giao diện trên simulator: biến môi trường NAVESP_PREVIEW = "dest" (chọn sẵn điểm đến)
        // hoặc "nav" (thêm giả trạng thái đang dẫn đường). Chạy: SIMCTL_CHILD_NAVESP_PREVIEW=nav xcrun simctl launch ...
        let preview = ProcessInfo.processInfo.environment["NAVESP_PREVIEW"] ?? ""
        if preview == "dest" || preview == "nav" {
            userLocation = CLLocation(latitude: 12.6900, longitude: 108.0380)
            setDestination(CLLocationCoordinate2D(latitude: 12.6790, longitude: 108.0510), name: "Nguyễn Công Trứ")
        }
        if preview == "nav" { ble.debugFake(status: .debugNavigating()) }
        #endif
    }

    // MARK: Lưu / khôi phục điểm đến (khi iOS đóng app rồi mở lại ở nền)

    private func save() {
        let d = UserDefaults.standard
        if let c = destination {
            d.set([c.latitude, c.longitude], forKey: "NavESP.dest")
            d.set(destinationName, forKey: "NavESP.destName")
        } else {
            d.removeObject(forKey: "NavESP.dest")
        }
    }

    private func restore() {
        let d = UserDefaults.standard
        if let a = d.array(forKey: "NavESP.dest") as? [Double], a.count == 2 {
            destination = CLLocationCoordinate2D(latitude: a[0], longitude: a[1])
            destinationName = d.string(forKey: "NavESP.destName") ?? ""
            navigating = d.bool(forKey: "NavESP.navigating")
        }
    }

    // MARK: Vị trí

    nonisolated func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let loc = locations.last else { return }
        Task { @MainActor in
            self.userLocation = loc
            self.syncTheme()
        }
    }

    nonisolated func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        manager.startUpdatingLocation()
    }

    /// Điểm xuất phát: vị trí thiết bị (nếu có GPS hoặc đang SIM), không thì vị trí iPhone
    private var origin: CLLocationCoordinate2D? {
        if let st = ble.status, st.gpsFix || st.sim, st.lat != 0 {
            return CLLocationCoordinate2D(latitude: st.lat, longitude: st.lon)
        }
        return userLocation?.coordinate
    }

    // MARK: Thao tác

    func setDestination(_ c: CLLocationCoordinate2D, name: String = "") {
        destination = c
        destinationName = name.isEmpty ? String(format: "%.5f, %.5f", c.latitude, c.longitude) : name
        save()
        Task { await plan() }
    }

    /// Tính đường xem trước (chưa gửi xuống thiết bị)
    func plan() async {
        guard let dest = destination else { return }
        guard let from = origin else { message = "Chưa có vị trí hiện tại"; return }
        busy = true
        defer { busy = false }
        do {
            let r = try await RouteService.route(from: from, to: dest)
            route = r
            message = nil
            log(String(format: "Tinh duong tu %.6f,%.6f: %.0f m, %d buoc", from.latitude, from.longitude, r.distanceM, r.steps.count))
        } catch {
            message = error.localizedDescription
            log("Loi tinh duong: \(error.localizedDescription)")
        }
    }

    /// Gửi tuyến xuống thiết bị và bắt đầu dẫn đường
    func start() {
        guard let r = route else { return }
        log("Bat dau dan duong, \(r.rte1.count) byte")
        navigating = true
        upload(r)
    }

    func stop() {
        log("Dung dan duong")
        navigating = false
        ble.send(.stop)
    }

    func clear() {
        navigating = false
        route = nil
        sentCRC = nil
        destination = nil
        destinationName = ""
        save()
        ble.send(.clear)
    }

    func toggleSim() {
        ble.send(ble.status?.sim == true ? .simOff : .simOn)
    }

    /// Thử tính lại đường: SIM trên thiết bị rời tuyến đi thẳng (lệnh 06)
    func simOffRoute() {
        ble.send(.simOffRoute)
    }

    private func upload(_ r: PlannedRoute, done: (() -> Void)? = nil) {
        busy = true
        ble.sendRoute(r.rte1) { [weak self] ok in
            Task { @MainActor in
                guard let self else { return }
                self.busy = false
                self.ignoreArrivedUntil = Date().addingTimeInterval(3)
                if ok {
                    self.sentCRC = CRC32.checksum(r.rte1)
                    self.log(String(format: "Gui tuyen xong, CRC %08x", self.sentCRC!))
                    self.ble.send(.start)
                } else {
                    self.message = "Gửi tuyến thất bại"
                }
                done?()
            }
        }
    }

    /// Chạy việc cần mạng/BLE khi app có thể đang ở nền: xin iOS thêm thời gian cho tới khi xong
    private final class TaskID { var id = UIBackgroundTaskIdentifier.invalid }

    private func inBackground(_ name: String, _ work: @escaping (@escaping () -> Void) -> Void) {
        let t = TaskID()
        let end = {
            guard t.id != .invalid else { return }
            UIApplication.shared.endBackgroundTask(t.id)
            t.id = .invalid
        }
        t.id = UIApplication.shared.beginBackgroundTask(withName: name) { end() }
        work { end() }
    }

    /// Tính lại đường (từ vị trí thiết bị) rồi gửi xuống
    private func replanAndSend(reason: String) {
        log("Tinh lai duong (\(reason))")
        rerouting = true
        inBackground(reason) { finish in
            Task {
                await self.plan()
                guard let r = self.route, self.ble.link == .connected, self.navigating else {
                    self.rerouting = false
                    finish()
                    return
                }
                if reason == "reroute" { self.rerouteCount += 1 }
                self.upload(r) {
                    self.rerouting = false
                    finish()
                }
            }
        }
    }

    // MARK: Tự tính lại đường khi lệch tuyến (khi app đang mở)

    private var lastLogged: (DeviceStatus.State, UInt32)?
    private var ignoreArrivedUntil = Date.distantPast

    /// Xoá chuyến đã xong ở phía app (thiết bị tự gỡ tuyến); dòng trạng thái vẫn hiện "Đã đến nơi"
    private func finishArrived() {
        log("Da den noi, ket thuc dan duong")
        navigating = false
        offRouteSince = nil
        route = nil
        sentCRC = nil
        destination = nil
        destinationName = ""
        save()
    }

    private func onStatus(_ st: DeviceStatus?) {
        syncTheme()
        if let st, lastLogged?.0 != st.state || lastLogged?.1 != st.routeCRC {
            lastLogged = (st.state, st.routeCRC)
            log(String(format: "Trang thai thiet bi: %d, upload %d, CRC %08x, sim %d", st.state.rawValue, st.upload, st.routeCRC, st.sim ? 1 : 0))
        }
        guard let st, navigating, destination != nil else { offRouteSince = nil; return }
        if st.upload == 1 { return }                 // thiết bị đang nhận tuyến: trạng thái tạm thời, bỏ qua
        if st.upload == 3 || st.upload == 4 || st.upload == 5 || st.upload == 6 {
            message = "Thiết bị báo lỗi nhận tuyến (\(st.upload))"
        }
        let now = Date()
        // Thiết bị đã đến nơi: kết thúc dẫn đường, dù app đang ở nền hay đang tính lại đường dở.
        // Bỏ qua vài giây ngay sau khi gửi tuyến: có thể vẫn là trạng thái cũ chưa kịp cập nhật.
        if st.state == .arrived, !ble.uploading, now >= ignoreArrivedUntil {
            finishArrived()
            return
        }
        guard !busy, !ble.uploading else { return }

        // Thiết bị mất tuyến (vừa khởi động lại, đã bị xoá): gửi lại, tối đa 1 lần/20 s.
        // CRC khác nhưng khác 0 thì coi thiết bị đang dùng tuyến hợp lệ (vừa nhận xong), không gửi lại.
        if st.routeCRC == 0 || !st.routeLoaded {
            guard now.timeIntervalSince(lastResend) >= Self.rerouteEvery else { return }
            lastResend = now
            if let r = route, sentCRC == nil || CRC32.checksum(r.rte1) == sentCRC {
                log("Thiet bi mat tuyen, gui lai")
                inBackground("resend") { finish in self.upload(r) { finish() } }
            } else {
                replanAndSend(reason: "resend")
            }
            return
        }

        guard st.state == .offRoute else { offRouteSince = nil; return }
        if offRouteSince == nil { offRouteSince = now }
        guard now.timeIntervalSince(offRouteSince!) >= Self.rerouteAfter,
              now.timeIntervalSince(lastReroute) >= Self.rerouteEvery else { return }
        lastReroute = now
        replanAndSend(reason: "reroute")
    }
}

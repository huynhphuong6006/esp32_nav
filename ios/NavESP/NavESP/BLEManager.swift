// Kết nối BLE với thiết bị "NavESP" theo docs/ble_protocol.md:
// tự quét/kết nối lại, nhận trạng thái (notify), gửi tuyến chia gói BEGIN/DATA/END + CRC32, gửi lệnh.
// Chạy được khi khoá máy: chế độ nền bluetooth-central + khôi phục trạng thái (iOS đánh thức/mở lại app khi có sự kiện BLE).
// Nhớ thiết bị đã kết nối và luôn giữ một lệnh kết nối chờ, nên thiết bị bật lại là tự nối, không cần quét.
import CoreBluetooth
import Foundation
import UIKit

struct DeviceStatus: Equatable {
    enum State: UInt8 { case idle = 0, navigating = 1, offRoute = 2, arrived = 3 }
    var state: State
    var gpsFix: Bool
    var sim: Bool
    var routeLoaded: Bool
    var routeActive: Bool
    var night: Bool
    var upload: UInt8              // 0 chưa gửi, 1 đang nhận, 2 OK, 3 sai CRC, 4 sai định dạng, 5 quá lớn, 6 sai thứ tự
    var lat: Double
    var lon: Double
    var speedKmh: Double
    var nextStep: Int?
    var distNextM: Int
    var remainM: Int
    var routeCRC: UInt32

    #if DEBUG
    static func debugNavigating() -> DeviceStatus {
        var b = [UInt8](repeating: 0, count: 28)
        b[0] = 1; b[1] = 1; b[2] = 0b1111; b[3] = 2
        func put32(_ o: Int, _ v: UInt32) { for i in 0..<4 { b[o + i] = UInt8((v >> (8 * i)) & 0xFF) } }
        put32(4, UInt32(bitPattern: Int32(126_900_000))); put32(8, UInt32(bitPattern: Int32(1_080_380_000)))
        b[12] = 0xD0; b[13] = 0x07                              // 20 m/s
        b[14] = 3; put32(16, 180); put32(20, 2_380)
        return DeviceStatus(Data(b))!
    }
    #endif

    init?(_ d: Data) {
        guard d.count >= 28 else { return nil }
        let b = [UInt8](d)
        func u16(_ o: Int) -> UInt16 { UInt16(b[o]) | UInt16(b[o + 1]) << 8 }
        func u32(_ o: Int) -> UInt32 { UInt32(b[o]) | UInt32(b[o + 1]) << 8 | UInt32(b[o + 2]) << 16 | UInt32(b[o + 3]) << 24 }
        state = State(rawValue: b[1]) ?? .idle
        gpsFix = b[2] & 1 != 0
        sim = b[2] & 2 != 0
        routeLoaded = b[2] & 4 != 0
        routeActive = b[2] & 8 != 0
        night = b[2] & 16 != 0
        upload = b[3]
        lat = Double(Int32(bitPattern: u32(4))) / 1e7
        lon = Double(Int32(bitPattern: u32(8))) / 1e7
        speedKmh = Double(u16(12)) * 0.036
        let n = u16(14)
        nextStep = n == 0xFFFF ? nil : Int(n)
        distNextM = Int(u32(16))
        remainM = Int(u32(20))
        routeCRC = u32(24)
    }
}

enum Command: UInt8 { case start = 1, stop = 2, clear = 3, simOn = 4, simOff = 5, simOffRoute = 6 }

final class BLEManager: NSObject, ObservableObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    enum Link: Equatable { case off, idle, scanning, connecting, connected }

    static let service = CBUUID(string: "4E415600-BA5E-4C0D-8E2F-6A1B3C5D7E90")
    static let routeChr = CBUUID(string: "4E415600-BA5E-4C0D-8E2F-6A1B3C5D7E91")
    static let statusChr = CBUUID(string: "4E415600-BA5E-4C0D-8E2F-6A1B3C5D7E92")
    static let controlChr = CBUUID(string: "4E415600-BA5E-4C0D-8E2F-6A1B3C5D7E93")

    @Published private(set) var link: Link = .off
    @Published private(set) var status: DeviceStatus?
    @Published private(set) var uploading = false
    /// Tự kết nối khi mở app và khi thiết bị quay lại (mặc định bật)
    @Published var autoConnect: Bool = UserDefaults.standard.object(forKey: "NavESP.autoConnect") as? Bool ?? true {
        didSet {
            UserDefaults.standard.set(autoConnect, forKey: "NavESP.autoConnect")
            if autoConnect {
                manualOff = false
                if link == .idle {
                    if peripheral != nil { connectNow() } else if shouldAutoScan { scan() }
                }
            } else if link == .scanning || link == .connecting {
                disconnectNow()
            }
        }
    }
    /// Người dùng vừa bấm Ngắt kết nối: không tự nối lại cho tới khi bấm Kết nối hoặc mở lại app
    private var manualOff = false
    private var shouldAuto: Bool { autoConnect && !manualOff }
    /// Đã quên thiết bị: không tự quét rồi nối vào thiết bị đầu tiên nữa, chờ người dùng chọn trong danh sách
    private var forgotten: Bool {
        get { UserDefaults.standard.bool(forKey: "NavESP.forgotten") }
        set { UserDefaults.standard.set(newValue, forKey: "NavESP.forgotten") }
    }
    private var shouldAutoScan: Bool { shouldAuto && !forgotten }

    struct Found: Identifiable {
        let peripheral: CBPeripheral
        var name: String
        var rssi: Int
        var id: UUID { peripheral.identifier }
    }
    @Published private(set) var listing = false          // đang quét để liệt kê thiết bị (trang cài đặt)
    @Published private(set) var found: [Found] = []
    @Published private(set) var rssi: Int?
    var deviceName: String? { peripheral?.name }
    var hasDevice: Bool { peripheral != nil }

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var chrs: [CBUUID: CBCharacteristic] = [:]
    private var queue: [Data] = []                       // các gói tuyến chờ ghi (ghi có phản hồi, lần lượt)
    private var uploadDone: ((Bool) -> Void)?
    private static let restoreID = "NavESPCentral"
    private static let knownKey = "NavESP.peripheralID"

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main,
                                   options: [CBCentralManagerOptionRestoreIdentifierKey: Self.restoreID])
    }

    // MARK: Gửi tuyến / lệnh

    /// Gửi tuyến RTE1. completion(true) khi đã ghi hết gói; kết quả kiểm tra của thiết bị xem ở status.upload
    func sendRoute(_ data: Data, completion: @escaping (Bool) -> Void) {
        guard let p = peripheral, chrs[Self.routeChr] != nil, !uploading else { completion(false); return }
        let chunk = max(20, min(p.maximumWriteValueLength(for: .withResponse) - 5, 240))
        var packets: [Data] = []
        var begin = Data([1])
        begin.append(le32(UInt32(data.count)))
        begin.append(le32(CRC32.checksum(data)))
        packets.append(begin)
        var off = 0
        while off < data.count {
            let n = min(chunk, data.count - off)
            var pkt = Data([2])
            pkt.append(le32(UInt32(off)))
            pkt.append(data.subdata(in: off..<off + n))
            packets.append(pkt)
            off += n
        }
        packets.append(Data([3]))
        queue = packets
        uploadDone = completion
        uploading = true
        writeNext()
    }

    #if DEBUG
    /// Chỉ để chụp màn hình thử giao diện trên simulator (không có BLE)
    private var faking = false
    func debugFake(status: DeviceStatus) {
        faking = true
        link = .connected
        self.status = status
    }
    #endif

    // MARK: Cài đặt kết nối

    func connectNow() {
        manualOff = false
        guard central.state == .poweredOn else { return }
        guard let p = peripheral else { startListing(); return }
        link = .connecting
        if p.state == .connected { p.discoverServices([Self.service]) } else { central.connect(p) }
    }

    /// Quét để liệt kê thiết bị, không tự kết nối; người dùng chọn trong danh sách
    func startListing() {
        guard central.state == .poweredOn else { return }
        central.stopScan()
        found = []
        listing = true
        if link == .idle { link = .scanning }
        central.scanForPeripherals(withServices: [Self.service])
    }

    func stopListing() {
        guard listing else { return }
        listing = false
        central.stopScan()
        if link == .scanning {
            if shouldAutoScan { scan() } else { link = .idle }
        }
    }

    func connect(to item: Found) {
        listing = false
        central.stopScan()
        let old = peripheral
        peripheral = item.peripheral
        item.peripheral.delegate = self
        chrs = [:]
        status = nil
        rssi = nil
        manualOff = false
        forgotten = false
        if let old, old != item.peripheral { central.cancelPeripheralConnection(old) }
        link = .connecting
        central.connect(item.peripheral)
    }

    func readRSSI() {
        if link == .connected { peripheral?.readRSSI() }
    }

    func disconnectNow() {
        manualOff = true
        listing = false
        central.stopScan()
        if let p = peripheral, central.state == .poweredOn { central.cancelPeripheralConnection(p) }
        if central.state == .poweredOn { link = .idle }
    }

    /// Xoá thiết bị đã nhớ và thôi tự nối lại với nó; muốn nối lại thì quét rồi chọn trong danh sách
    func forgetDevice() {
        UserDefaults.standard.removeObject(forKey: Self.knownKey)
        forgotten = true
        listing = false
        rssi = nil
        central.stopScan()
        let old = peripheral
        peripheral = nil
        chrs = [:]
        status = nil
        if uploading { finishUpload(false) }
        manualOff = false
        guard central.state == .poweredOn else { return }
        if let old { central.cancelPeripheralConnection(old) }
        link = .idle
    }

    /// Giao diện thiết bị (lệnh 08): false ngày, true đêm
    func setTheme(night: Bool) {
        guard let p = peripheral, let c = chrs[Self.controlChr] else { return }
        p.writeValue(Data([8, night ? 1 : 0]), for: c, type: .withResponse)
    }

    func send(_ cmd: Command) {
        guard let p = peripheral, let c = chrs[Self.controlChr] else { return }
        p.writeValue(Data([cmd.rawValue]), for: c, type: .withResponse)
    }

    private func le32(_ v: UInt32) -> Data { withUnsafeBytes(of: v.littleEndian) { Data($0) } }

    private func writeNext() {
        guard let p = peripheral, let c = chrs[Self.routeChr] else { finishUpload(false); return }
        guard !queue.isEmpty else { finishUpload(true); return }
        p.writeValue(queue.removeFirst(), for: c, type: .withResponse)
    }

    private func finishUpload(_ ok: Bool) {
        queue = []
        uploading = false
        uploadDone?(ok)
        uploadDone = nil
    }

    // MARK: CBCentralManagerDelegate

    // iOS mở lại app ở nền (sau khi app bị đóng) và trả lại thiết bị đang kết nối/đang chờ kết nối
    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        AppLog.write("iOS mo lai app de khoi phuc BLE (\(Self.appState))")
        if let p = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first {
            peripheral = p
            p.delegate = self
        }
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        #if DEBUG
        if faking { return }
        #endif
        guard central.state == .poweredOn else { link = .off; return }
        if peripheral == nil, let s = UserDefaults.standard.string(forKey: Self.knownKey), let id = UUID(uuidString: s),
           let p = central.retrievePeripherals(withIdentifiers: [id]).first {
            peripheral = p                                // thiết bị đã biết: kết nối thẳng, không cần quét
            p.delegate = self
        }
        if let p = peripheral {
            if p.state == .connected {
                link = .connecting
                p.discoverServices([Self.service])
            } else {
                guard shouldAuto else { link = .idle; return }
                link = .connecting
                central.connect(p)                        // lệnh kết nối chờ: không hết hạn, chạy cả khi khoá máy
            }
        } else if shouldAutoScan {
            scan()
        } else {
            link = .idle
        }
    }

    private func scan() {
        link = .scanning
        central.scanForPeripherals(withServices: [Self.service])
    }

    func centralManager(_ central: CBCentralManager, didDiscover p: CBPeripheral, advertisementData: [String: Any], rssi: NSNumber) {
        if listing {
            let name = p.name ?? (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? "NavESP"
            if let i = found.firstIndex(where: { $0.id == p.identifier }) {
                found[i].rssi = rssi.intValue
            } else {
                found.append(Found(peripheral: p, name: name, rssi: rssi.intValue))
            }
            return
        }
        central.stopScan()
        peripheral = p
        p.delegate = self
        link = .connecting
        central.connect(p)
    }

    func centralManager(_ central: CBCentralManager, didConnect p: CBPeripheral) {
        UserDefaults.standard.set(p.identifier.uuidString, forKey: Self.knownKey)
        p.discoverServices([Self.service])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        guard p == peripheral else { return }
        AppLog.write("BLE khong ket noi duoc: \(Self.describe(error)); app \(Self.appState)")
        if shouldAuto { central.connect(p) } else { link = .idle }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        guard p == peripheral else { return }             // thiết bị vừa bị quên
        let since = connectedAt.map { String(format: "%.0f s", Date().timeIntervalSince($0)) } ?? "?"
        AppLog.write("BLE mat ket noi: \(Self.describe(error)); app \(Self.appState); RSSI cuoi \(lastRSSI.map { "\($0) dBm" } ?? "chua co"); sau \(since)")
        connectedAt = nil
        chrs = [:]
        status = nil
        rssi = nil
        if uploading { finishUpload(false) }
        guard shouldAuto else { link = .idle; return }
        link = .connecting
        central.connect(p)                                // tự kết nối lại khi thiết bị quay lại
    }

    // MARK: CBPeripheralDelegate

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = p.services?.first(where: { $0.uuid == Self.service }) else { return }
        p.discoverCharacteristics([Self.routeChr, Self.statusChr, Self.controlChr], for: svc)
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor svc: CBService, error: Error?) {
        for c in svc.characteristics ?? [] { chrs[c.uuid] = c }
        if let st = chrs[Self.statusChr] {
            p.setNotifyValue(true, for: st)
            p.readValue(for: st)
        }
        link = .connected
        connectedAt = Date()
        lastRSSI = nil
    }

    func peripheral(_ p: CBPeripheral, didReadRSSI value: NSNumber, error: Error?) {
        if error == nil {
            rssi = value.intValue
            lastRSSI = value.intValue
        }
    }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor c: CBCharacteristic, error: Error?) {
        if c.uuid == Self.statusChr, let v = c.value, let st = DeviceStatus(v) {
            status = st
            if Date().timeIntervalSince(lastRSSIRead) >= 10 {      // RSSI để ghi log khi mất kết nối
                lastRSSIRead = Date()
                p.readRSSI()
            }
        }
    }

    // MARK: Ghi log mất kết nối

    private var connectedAt: Date?
    private var lastRSSI: Int?
    private var lastRSSIRead = Date.distantPast

    static var appState: String {
        switch UIApplication.shared.applicationState {
        case .active: return "dang mo"
        case .inactive: return "chuyen trang thai"
        case .background: return "chay nen"
        @unknown default: return "?"
        }
    }

    /// Lỗi CoreBluetooth: mã + mô tả. nil = ngắt chủ động từ phía app (cancelPeripheralConnection)
    static func describe(_ error: Error?) -> String {
        guard let e = error as NSError? else { return "khong co loi (app chu dong ngat)" }
        let name: String
        switch (e.domain, e.code) {
        case (CBErrorDomain, CBError.connectionTimeout.rawValue): name = "het thoi gian cho (mat song)"
        case (CBErrorDomain, CBError.peripheralDisconnected.rawValue): name = "thiet bi ngat"
        case (CBErrorDomain, CBError.connectionFailed.rawValue): name = "ket noi that bai"
        case (CBErrorDomain, CBError.connectionLimitReached.rawValue): name = "qua so ket noi"
        default: name = e.localizedDescription
        }
        return "\(e.domain) \(e.code) \(name)"
    }

    func peripheral(_ p: CBPeripheral, didWriteValueFor c: CBCharacteristic, error: Error?) {
        guard c.uuid == Self.routeChr, uploading else { return }
        if error != nil { finishUpload(false) } else { writeNext() }
    }
}

// Tính đường bằng OSRM (dữ liệu OpenStreetMap) và chuyển sang RTE1 cho thiết bị.
// Máy chủ chính: máy chủ riêng trong Cài đặt (RouteSettings; ví dụ OSRM xe máy tự host, header X-Route-Key, xem tools/osrm).
// Không có, lỗi hoặc quá 4 giây thì dùng máy chủ demo công khai router.project-osrm.org (profile ô tô, có thể vào cao tốc).
import CoreLocation
import Foundation

struct PlannedRoute {
    let coordinates: [CLLocationCoordinate2D]
    let distanceM: Double
    let durationS: Double
    let steps: [RouteEncoder.Step]
    let rte1: Data
}

enum RouteError: LocalizedError {
    case server(String)
    case noRoute

    var errorDescription: String? {
        switch self {
        case .server(let m): return "Lỗi máy chủ chỉ đường: \(m)"
        case .noRoute: return "Không tìm được đường"
        }
    }
}

enum RouteService {
    static let fallback = "https://router.project-osrm.org/route/v1/driving/"
    static let userAgent = "NavESP-iOS/0.1 (personal test)"
    static let primaryTimeout: TimeInterval = 4

    /// Máy chủ đã dùng cho lần tính gần nhất ("xe máy" hoặc "demo ô tô"), để ghi log
    @MainActor static var lastServer = ""

    static func route(from a: CLLocationCoordinate2D, to b: CLLocationCoordinate2D) async throws -> PlannedRoute {
        let coords = String(format: "%.6f,%.6f;%.6f,%.6f", a.longitude, a.latitude, b.longitude, b.latitude)
        let query = coords + "?overview=full&geometries=polyline6&steps=true"
        if let c = await RouteSettings.shared.custom {
            do {
                let r = try await fetch(c.base + "/route/v1/driving/" + query, timeout: primaryTimeout, key: c.key)
                await MainActor.run { lastServer = "rieng" }
                AppLog.write("Tinh duong: may chu rieng \(c.base)")
                return r
            } catch {
                AppLog.write("May chu rieng loi (\(error.localizedDescription)), dung demo o to")
            }
        }
        let r = try await fetch(fallback + query, timeout: 20, key: nil)
        await MainActor.run { lastServer = "demo o to" }
        AppLog.write("Tinh duong: may chu demo o to")
        return r
    }

    /// Thử máy chủ riêng bằng một tuyến ngắn ở Buôn Ma Thuột: trả về thời gian (giây) hoặc lỗi
    static func testCustom() async -> Result<Double, Error> {
        guard let c = await RouteSettings.shared.custom else { return .failure(RouteError.server("Chưa nhập máy chủ")) }
        let t0 = Date()
        do {
            _ = try await fetch(c.base + "/route/v1/driving/108.044900,12.687400;108.051000,12.679000?overview=false&steps=true",
                                timeout: 8, key: c.key, parse: false)
            return .success(Date().timeIntervalSince(t0))
        } catch { return .failure(error) }
    }

    private static func fetch(_ url: String, timeout: TimeInterval, key: String?) async throws -> PlannedRoute {
        try await fetch(url, timeout: timeout, key: key, parse: true)!
    }

    @discardableResult
    private static func fetch(_ url: String, timeout: TimeInterval, key: String?, parse: Bool) async throws -> PlannedRoute? {
        var req = URLRequest(url: URL(string: url)!)
        req.setValue(userAgent, forHTTPHeaderField: "User-Agent")
        if let key, !key.isEmpty { req.setValue(key, forHTTPHeaderField: "X-Route-Key") }
        req.timeoutInterval = timeout
        let (data, resp) = try await URLSession.shared.data(for: req)
        if let h = resp as? HTTPURLResponse, h.statusCode != 200, h.statusCode != 400 {
            throw RouteError.server("HTTP \(h.statusCode)")
        }
        guard let json = try JSONSerialization.jsonObject(with: data) as? [String: Any] else { throw RouteError.noRoute }
        guard json["code"] as? String == "Ok" else { throw RouteError.server(json["message"] as? String ?? json["code"] as? String ?? "?") }
        if !parse { return nil }
        guard let route = (json["routes"] as? [[String: Any]])?.first,
              let geom = route["geometry"] as? String,
              let legs = route["legs"] as? [[String: Any]] else { throw RouteError.noRoute }
        let pts = RouteEncoder.decodePolyline(geom)
        guard pts.count >= 2 else { throw RouteError.noRoute }
        let steps = RouteEncoder.steps(points: pts, legs: legs)
        return PlannedRoute(coordinates: pts.map { CLLocationCoordinate2D(latitude: $0.0, longitude: $0.1) },
                            distanceM: route["distance"] as? Double ?? 0,
                            durationS: route["duration"] as? Double ?? 0,
                            steps: steps,
                            rte1: RouteEncoder.encode(points: pts, steps: steps))
    }
}

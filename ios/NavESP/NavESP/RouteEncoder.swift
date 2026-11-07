// Chuyển kết quả OSRM sang định dạng RTE1 mà thiết bị đọc.
// Phải cho ra đúng từng byte như tools/map_render/route_bin.py (kiểm tra bằng ios/check_encoder.sh).
import Foundation

enum RouteEncoder {
    // Loại chỉ dẫn, trùng với enum trong firmware/nav và route_bin.py
    static let kinds: [String: UInt8] = [
        "straight": 0, "slight right": 1, "right": 2, "sharp right": 3, "uturn": 4,
        "sharp left": 5, "left": 6, "slight left": 7,
    ]
    static let roundabout: UInt8 = 8, arrive: UInt8 = 9, depart: UInt8 = 10

    struct Step {
        let index: Int
        let kind: UInt8
        let exit: UInt8
        let name: String
    }

    static func dist(_ a: (Double, Double), _ b: (Double, Double)) -> Double {
        let dy = (b.0 - a.0) * 111320
        let dx = (b.1 - a.1) * 111320 * cos(a.0 * .pi / 180)
        return (dx * dx + dy * dy).squareRoot()
    }

    /// Các bước rẽ từ "legs" của OSRM (đã đọc bằng JSONSerialization), điểm tuyến dạng (lat, lon)
    static func steps(points: [(Double, Double)], legs: [[String: Any]]) -> [Step] {
        var out: [Step] = []
        for (li, leg) in legs.enumerated() {
            for s in leg["steps"] as? [[String: Any]] ?? [] {
                guard let m = s["maneuver"] as? [String: Any] else { continue }
                let t = m["type"] as? String ?? ""
                let mod = m["modifier"] as? String ?? "straight"
                let kind: UInt8
                if t == "depart" {
                    if li > 0 { continue }            // điểm trung gian: bỏ "xuất phát"/"đến" giữa các chặng
                    kind = depart
                } else if t == "arrive" {
                    if li < legs.count - 1 { continue }
                    kind = arrive
                } else if ["roundabout", "rotary", "roundabout turn"].contains(t) {
                    kind = roundabout
                } else if ["new name", "continue"].contains(t) && ["straight", "slight left", "slight right"].contains(mod) {
                    continue                          // đi thẳng/đổi tên đường: không cần báo
                } else {
                    kind = kinds[mod] ?? 0
                }
                guard let loc = m["location"] as? [Double], loc.count == 2 else { continue }
                let target = (loc[1], loc[0])
                var best = 0
                var bestD = Double.infinity
                for (i, p) in points.enumerated() {
                    let d = dist(p, target)
                    if d < bestD { bestD = d; best = i }   // giống min() của Python: lấy chỉ số đầu tiên
                }
                let exit = kind == roundabout ? UInt8(clamping: (m["exit"] as? Int) ?? 0) : 0
                out.append(Step(index: best, kind: kind, exit: exit, name: s["name"] as? String ?? ""))
            }
        }
        return out
    }

    static func encode(points: [(Double, Double)], steps: [Step]) -> Data {
        var d = Data("RTE1".utf8)
        func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        func i32(_ v: Int32) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        func u16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        u32(UInt32(points.count))
        for (la, lo) in points {
            // round() của Python làm tròn về số chẵn khi đúng giữa
            i32(Int32((la * 1e7).rounded(.toNearestOrEven)))
            i32(Int32((lo * 1e7).rounded(.toNearestOrEven)))
        }
        u16(UInt16(steps.count))
        for s in steps {
            let name = Data(s.name.utf8.prefix(255))
            u32(UInt32(s.index))
            d.append(s.kind)
            d.append(s.exit)
            d.append(UInt8(name.count))
            d.append(name)
        }
        return d
    }

    /// Giải mã polyline OSRM (geometries=polyline6) thành (lat, lon)
    static func decodePolyline(_ str: String, precision: Double = 1e6) -> [(Double, Double)] {
        var pts: [(Double, Double)] = []
        let bytes = Array(str.utf8)
        var i = 0, lat = 0, lon = 0
        func next() -> Int {
            var result = 0, shift = 0
            while i < bytes.count {
                let b = Int(bytes[i]) - 63
                i += 1
                result |= (b & 0x1f) << shift
                shift += 5
                if b < 0x20 { break }
            }
            return (result & 1) != 0 ? ~(result >> 1) : (result >> 1)
        }
        while i < bytes.count {
            lat += next()
            lon += next()
            pts.append((Double(lat) / precision, Double(lon) / precision))
        }
        return pts
    }
}

/// CRC32 chuẩn zlib/IEEE (đa thức 0xEDB88320), giống thiết bị kiểm tra
enum CRC32 {
    private static let table: [UInt32] = (0..<256).map { n -> UInt32 in
        var c = UInt32(n)
        for _ in 0..<8 { c = (c & 1) != 0 ? 0xEDB8_8320 ^ (c >> 1) : c >> 1 }
        return c
    }

    static func checksum(_ data: Data) -> UInt32 {
        var c: UInt32 = 0xFFFF_FFFF
        for b in data { c = table[Int((c ^ UInt32(b)) & 0xFF)] ^ (c >> 8) }
        return c ^ 0xFFFF_FFFF
    }
}

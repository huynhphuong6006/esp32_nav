// Giờ mặt trời mọc/lặn tính tại máy, không cần mạng (phương trình mặt trời, sai số vài phút).
import CoreLocation
import Foundation

struct SunTimes {
    let sunrise: Date
    let sunset: Date

    private static let j1970 = 2440587.5
    private static let j2000 = 2451545.0
    private static let horizon = -0.833      // độ, tính cả khúc xạ và bán kính mặt trời

    /// Mọc/lặn của ngày mặt trời chứa `date`; nil nếu ở vùng cực (không mọc hoặc không lặn).
    static func compute(date: Date, coordinate c: CLLocationCoordinate2D) -> SunTimes? {
        // Lấy theo giờ trưa mặt trời gần `date` nhất, để cả đêm đều so với đúng mốc
        let rad = Double.pi / 180
        let j = date.timeIntervalSince1970 / 86400 + j1970
        let n = (j - j2000 - 0.0009 + c.longitude / 360).rounded()
        let jStar = n + 0.0009 - c.longitude / 360
        let m = (357.5291 + 0.98560028 * jStar).truncatingRemainder(dividingBy: 360)
        let eq = 1.9148 * sin(m * rad) + 0.02 * sin(2 * m * rad) + 0.0003 * sin(3 * m * rad)
        let lambda = (m + eq + 180 + 102.9372).truncatingRemainder(dividingBy: 360)
        let transit = j2000 + jStar + 0.0053 * sin(m * rad) - 0.0069 * sin(2 * lambda * rad)
        let sinD = sin(lambda * rad) * sin(23.44 * rad)
        let cosD = cos(asin(sinD))
        let cosW = (sin(horizon * rad) - sin(c.latitude * rad) * sinD) / (cos(c.latitude * rad) * cosD)
        guard abs(cosW) <= 1 else { return nil }
        let w = acos(cosW) / rad / 360
        func toDate(_ jd: Double) -> Date { Date(timeIntervalSince1970: (jd - j1970) * 86400) }
        return SunTimes(sunrise: toDate(transit - w), sunset: toDate(transit + w))
    }

    static func isNight(date: Date, coordinate: CLLocationCoordinate2D) -> Bool {
        guard let t = compute(date: date, coordinate: coordinate) else { return false }
        return date < t.sunrise || date >= t.sunset
    }
}

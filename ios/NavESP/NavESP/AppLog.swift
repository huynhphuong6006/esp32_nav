// Log của app ra file Documents/NavESP-log.txt để đối chiếu với log của thiết bị (thẻ SD).
// Xem trên iPhone: app Tệp -> Trên iPhone -> NavESP; hoặc Finder trên Mac khi cắm cáp (mục Tệp của iPhone).
import Foundation

enum AppLog {
    private static let queue = DispatchQueue(label: "navesp.log")
    private static let maxBytes = 2_000_000          // quá cỡ thì đổi tên sang .old và ghi file mới
    private static let url: URL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("NavESP-log.txt")
    private static let formatter: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return f
    }()

    static func write(_ message: String) {
        let line = "\(formatter.string(from: Date())) \(message)\n"
        queue.async {
            let fm = FileManager.default
            if let size = (try? fm.attributesOfItem(atPath: url.path)[.size]) as? Int, size > maxBytes {
                let old = url.deletingPathExtension().appendingPathExtension("old.txt")
                try? fm.removeItem(at: old)
                try? fm.moveItem(at: url, to: old)
            }
            if let h = try? FileHandle(forWritingTo: url) {
                h.seekToEndOfFile()
                h.write(Data(line.utf8))
                try? h.close()
            } else {
                try? Data(line.utf8).write(to: url)
            }
        }
    }
}

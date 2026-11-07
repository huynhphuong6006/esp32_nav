// Cài đặt máy chủ chỉ đường: địa chỉ + khoá (X-Route-Key). Khoá lưu trong Keychain.
// Nhập tay trong Cài đặt, hoặc quét mã QR / mở liên kết dạng
//   navesp://server?url=https%3A%2F%2Froute.example.com&key=<khoá>
// Chưa cài gì thì dùng giá trị lúc build (NavESPDefaultRoute* trong Info.plist, lấy từ Config/*.xcconfig);
// trống thì chỉ dùng máy chủ demo công cộng.
import Foundation
import Security

@MainActor
final class RouteSettings: ObservableObject {
    static let shared = RouteSettings()

    @Published var useCustom: Bool { didSet { save() } }
    @Published var url: String { didSet { save() } }
    @Published var key: String { didSet { saveKey() } }

    private static let urlKey = "NavESP.route.url"
    private static let useKey = "NavESP.route.useCustom"
    private static let keychainAccount = "route-key"

    private init() {
        let d = UserDefaults.standard
        let info = Bundle.main.infoDictionary ?? [:]
        let defURL = (info["NavESPDefaultRouteURL"] as? String ?? "").trimmingCharacters(in: .whitespaces)
        let defKey = (info["NavESPDefaultRouteKey"] as? String ?? "").trimmingCharacters(in: .whitespaces)
        if d.object(forKey: Self.useKey) == nil {          // lần đầu: lấy giá trị lúc build (nếu có)
            useCustom = !defURL.isEmpty
            url = defURL
            key = defKey
            save()
            saveKey()
        } else {
            useCustom = d.bool(forKey: Self.useKey)
            url = d.string(forKey: Self.urlKey) ?? ""
            key = Self.readKey() ?? ""
        }
    }

    /// Máy chủ riêng đang dùng (nil = chỉ dùng demo)
    var custom: (base: String, key: String)? {
        let u = url.trimmingCharacters(in: .whitespacesAndNewlines).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        guard useCustom, u.hasPrefix("https://") || u.hasPrefix("http://") else { return nil }
        return (u, key.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    /// Áp dụng liên kết/mã QR navesp://server?url=...&key=... ; trả về false nếu không đúng dạng
    @discardableResult
    func apply(_ link: URL) -> Bool {
        guard link.scheme == "navesp", link.host == "server",
              let items = URLComponents(url: link, resolvingAgainstBaseURL: false)?.queryItems,
              let u = items.first(where: { $0.name == "url" })?.value, u.hasPrefix("http") else { return false }
        url = u
        key = items.first(where: { $0.name == "key" })?.value ?? ""
        useCustom = true
        AppLog.write("Cai may chu chi duong tu ma QR/lien ket: \(u)")
        return true
    }

    private func save() {
        UserDefaults.standard.set(useCustom, forKey: Self.useKey)
        UserDefaults.standard.set(url, forKey: Self.urlKey)
    }

    private func saveKey() {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                kSecAttrAccount as String: Self.keychainAccount]
        SecItemDelete(q as CFDictionary)
        guard !key.isEmpty else { return }
        var add = q
        add[kSecValueData as String] = Data(key.utf8)
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock   // dùng được khi app chạy nền
        SecItemAdd(add as CFDictionary, nil)
    }

    private static func readKey() -> String? {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                kSecAttrAccount as String: keychainAccount,
                                kSecReturnData as String: true]
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data else { return nil }
        return String(data: d, encoding: .utf8)
    }
}

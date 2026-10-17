import Foundation
import Security
import UIKit

/// This phone's identity and the PCs it is paired with. Pairing keys live
/// in the Keychain (this device only, after first unlock).
final class Store {
    /// Generated once (Swift writes UUIDs upper-case) and used verbatim on
    /// the wire forever after.
    let deviceId: String = {
        if let id = UserDefaults.standard.string(forKey: "deviceId") { return id }
        let id = UUID().uuidString
        UserDefaults.standard.set(id, forKey: "deviceId")
        return id
    }()

    var deviceName: String { UIDevice.current.name }

    var model: String {
        var info = utsname()
        uname(&info)
        return withUnsafeBytes(of: &info.machine) { raw in
            String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
    }

    func pairKey(clientId: String) -> Data? {
        var query = baseQuery(clientId)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess else { return nil }
        return item as? Data
    }

    func savePairKey(clientId: String, key: Data) {
        SecItemDelete(baseQuery(clientId) as CFDictionary)
        var add = baseQuery(clientId)
        add[kSecValueData as String] = key
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(add as CFDictionary, nil)
    }

    private func baseQuery(_ clientId: String) -> [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: "vn.camstudio.pairing",
            kSecAttrAccount as String: clientId,
        ]
    }
}

import Foundation
import Security
import UIKit

/// This phone's identity and the PCs it is paired with, in the Keychain (this device only, after
/// first unlock). The app and the screen-broadcast extension share both through a Keychain
/// access group: Keychain Sharing works with the team's wildcard provisioning profile, while an
/// App Group would need an App ID registered for the bundle id.
final class Store {
    /// "<team>.vn.camstudio.shared", from Info.plist (CamStudioKeychainGroup, expanded by Xcode).
    /// nil in the xtool build, which has no Keychain Sharing entitlement: then everything stays
    /// in the app's own access group, as before.
    static let sharedGroup: String? = {
        guard let g = Bundle.main.object(forInfoDictionaryKey: "CamStudioKeychainGroup") as? String,
              !g.contains("$("), !g.hasPrefix(".") else { return nil }
        return g
    }()

    private static let pairingService = "vn.camstudio.pairing"
    private static let deviceService = "vn.camstudio.device"

    /// Generated once (Swift writes UUIDs upper-case) and used verbatim on
    /// the wire forever after.
    let deviceId: String

    init() {
        if let legacy = UserDefaults.standard.string(forKey: "deviceId") {
            // The app's own defaults (every build so far): that id wins, so a PC paired before
            // keeps its key; the Keychain copy is what the extension reads.
            if Store.read(Store.deviceService, "deviceId").flatMap({ String(data: $0, encoding: .utf8) }) != legacy {
                Store.save(Store.deviceService, "deviceId", Data(legacy.utf8))
            }
            deviceId = legacy
        } else if let data = Store.read(Store.deviceService, "deviceId"), let id = String(data: data, encoding: .utf8) {
            deviceId = id
        } else {
            deviceId = UUID().uuidString
            Store.save(Store.deviceService, "deviceId", Data(deviceId.utf8))
            if Bundle.main.bundlePath.hasSuffix(".app") { UserDefaults.standard.set(deviceId, forKey: "deviceId") }
        }
    }

    var deviceName: String { UIDevice.current.name }

    var model: String {
        var info = utsname()
        uname(&info)
        return withUnsafeBytes(of: &info.machine) { raw in
            String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
    }

    func pairKey(clientId: String) -> Data? { Store.read(Store.pairingService, clientId) }

    func savePairKey(clientId: String, key: Data) { Store.save(Store.pairingService, clientId, key) }

    /// Copies pairing keys saved by older builds (the app's own access group) into the shared
    /// one, so the screen-broadcast extension knows every PC paired before. Called at app launch.
    func shareLegacyKeys() {
        guard let group = Store.sharedGroup else { return }
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: Store.pairingService,
            kSecReturnAttributes as String: true,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitAll,
        ]
        var items: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &items) == errSecSuccess, let list = items as? [[String: Any]] else { return }
        for item in list where item[kSecAttrAccessGroup as String] as? String != group {
            guard let clientId = item[kSecAttrAccount as String] as? String, let key = item[kSecValueData as String] as? Data else { continue }
            if Store.read(Store.pairingService, clientId, group: group) == nil { Store.save(Store.pairingService, clientId, key) }
            // The old copy goes: a read that misses the shared group for a moment (the Keychain
            // does that now and then) must not fall back to a key from before a later pairing.
            // The PC then saw "paired" and a wrong MAC, and could only pair again.
            if let old = item[kSecAttrAccessGroup as String] as? String {
                SecItemDelete(Store.baseQuery(Store.pairingService, clientId, group: old) as CFDictionary)
            }
        }
    }

    // MARK: - Keychain

    /// The shared group first, then any group this process can read (keys from older builds).
    private static func read(_ service: String, _ account: String) -> Data? {
        if let group = sharedGroup, let data = read(service, account, group: group) { return data }
        return read(service, account, group: nil)
    }

    private static func read(_ service: String, _ account: String, group: String?) -> Data? {
        var query = baseQuery(service, account, group: group)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess else { return nil }
        return item as? Data
    }

    private static func save(_ service: String, _ account: String, _ data: Data) {
        let group = sharedGroup
        // Every copy this app can reach (no group: all its access groups), so a stale one in
        // another group can never shadow the new key.
        SecItemDelete(baseQuery(service, account, group: nil) as CFDictionary)
        if group != nil { SecItemDelete(baseQuery(service, account, group: group) as CFDictionary) }
        var add = baseQuery(service, account, group: group)
        add[kSecValueData as String] = data
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        if SecItemAdd(add as CFDictionary, nil) != errSecSuccess, group != nil {
            // Entitlement missing after all: the app's own access group.
            add[kSecAttrAccessGroup as String] = nil
            SecItemDelete(baseQuery(service, account, group: nil) as CFDictionary)
            SecItemAdd(add as CFDictionary, nil)
        }
    }

    private static func baseQuery(_ service: String, _ account: String, group: String?) -> [String: Any] {
        var q: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        if let group { q[kSecAttrAccessGroup as String] = group }
        return q
    }
}

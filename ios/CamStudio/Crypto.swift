import CryptoKit
import Foundation

/// The exact primitives of docs/protocol.md section 3. Byte layouts matter:
/// the PC (BCrypt) and Android (javax.crypto) must derive identical values,
/// which CryptoTests checks against docs/protocol-vectors.json.
enum Crypto {
    static let dirUp: UInt32 = 1   // PC -> phone
    static let dirDown: UInt32 = 2 // phone -> PC

    /// 65 bytes: 04 || X || Y.
    static func encodePublic(_ key: P256.KeyAgreement.PublicKey) -> Data { key.x963Representation }

    static func decodePublic(_ data: Data) throws -> P256.KeyAgreement.PublicKey {
        try P256.KeyAgreement.PublicKey(x963Representation: data)
    }

    /// Z = X coordinate of the shared point, 32 bytes big-endian.
    static func ecdh(_ own: P256.KeyAgreement.PrivateKey, _ peer: P256.KeyAgreement.PublicKey) throws -> Data {
        let secret = try own.sharedSecretFromKeyAgreement(with: peer)
        return secret.withUnsafeBytes { Data($0) }
    }

    static func sha256(_ parts: Data...) -> Data {
        var h = SHA256()
        parts.forEach { h.update(data: $0) }
        return Data(h.finalize())
    }

    static func hmac(_ key: Data, _ parts: Data...) -> Data {
        var h = HMAC<SHA256>(key: SymmetricKey(data: key))
        parts.forEach { h.update(data: $0) }
        return Data(h.finalize())
    }

    /// RFC 5869 HKDF-SHA256, written out with HMAC like the other two
    /// platforms (the protocol does not rely on any platform HKDF).
    static func hkdf(_ ikm: Data, salt: Data, info: Data, length: Int = 32) -> Data {
        let prk = hmac(salt, ikm)
        var out = Data()
        var t = Data()
        var counter: UInt8 = 1
        while out.count < length {
            t = hmac(prk, t, info, Data([counter]))
            out.append(t)
            counter += 1
        }
        return out.prefix(length)
    }

    /// LP(x) = u32 big-endian length || x
    static func lp(_ data: Data) -> Data {
        var n = UInt32(data.count).bigEndian
        return Data(bytes: &n, count: 4) + data
    }

    static func transcript(clientId: String, deviceId: String, pcPub: Data, phonePub: Data) -> Data {
        sha256(lp(Data("camstudio-v1".utf8)), lp(Data(clientId.utf8)), lp(Data(deviceId.utf8)), lp(pcPub), lp(phonePub))
    }

    static func sas(_ t: Data) -> String {
        let h = sha256(Data("camstudio-sas".utf8), t)
        let v = h.prefix(4).reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
        return String(format: "%06u", v % 1_000_000)
    }

    static func pairKey(z: Data, t: Data) -> Data { hkdf(z, salt: t, info: Data("camstudio pair".utf8)) }
    static func upKey(z: Data, kPair: Data, t: Data) -> Data { hkdf(z + kPair, salt: t, info: Data("camstudio pc->phone".utf8)) }
    static func downKey(z: Data, kPair: Data, t: Data) -> Data { hkdf(z + kPair, salt: t, info: Data("camstudio phone->pc".utf8)) }
    static func pcMac(kPair: Data, t: Data) -> Data { hmac(kPair, Data("pc".utf8), t) }
    static func phoneMac(kPair: Data, t: Data) -> Data { hmac(kPair, Data("phone".utf8), t) }

    private static func nonceAndAad(direction: UInt32, counter: UInt64) -> (AES.GCM.Nonce, Data) {
        var d = direction.bigEndian
        var c = counter.bigEndian
        let counterBytes = Data(bytes: &c, count: 8)
        let nonce = try! AES.GCM.Nonce(data: Data(bytes: &d, count: 4) + counterBytes)
        return (nonce, Data([0x02]) + counterBytes)
    }

    /// Returns ciphertext || 16-byte tag.
    static func seal(key: Data, direction: UInt32, counter: UInt64, plaintext: Data) throws -> Data {
        let (nonce, aad) = nonceAndAad(direction: direction, counter: counter)
        let box = try AES.GCM.seal(plaintext, using: SymmetricKey(data: key), nonce: nonce, authenticating: aad)
        return box.ciphertext + box.tag
    }

    static func open(key: Data, direction: UInt32, counter: UInt64, sealed: Data) throws -> Data {
        let (nonce, aad) = nonceAndAad(direction: direction, counter: counter)
        let box = try AES.GCM.SealedBox(nonce: nonce, ciphertext: sealed.dropLast(16), tag: sealed.suffix(16))
        return try AES.GCM.open(box, using: SymmetricKey(data: key), authenticating: aad)
    }

    /// Constant-time comparison for MACs.
    static func equal(_ a: Data, _ b: Data) -> Bool {
        guard a.count == b.count else { return false }
        return zip(a, b).reduce(UInt8(0)) { $0 | ($1.0 ^ $1.1) } == 0
    }
}

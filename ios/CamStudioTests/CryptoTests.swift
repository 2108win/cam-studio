import CryptoKit
import XCTest

/// Reproduces docs/protocol-vectors.json byte for byte (protocol appendix A).
final class CryptoTests: XCTestCase {
    private func vectors() throws -> [String: String] {
        let url = try XCTUnwrap(Bundle(for: CryptoTests.self).url(forResource: "protocol-vectors", withExtension: "json"))
        let json = try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: String]
        return try XCTUnwrap(json)
    }

    private func hex(_ s: String) -> Data {
        var data = Data()
        var i = s.startIndex
        while i < s.endIndex {
            let j = s.index(i, offsetBy: 2)
            data.append(UInt8(s[i..<j], radix: 16)!)
            i = j
        }
        return data
    }

    private func hex(_ d: Data) -> String { d.map { String(format: "%02x", $0) }.joined() }

    func testHandshakeMatchesVectors() throws {
        let v = try vectors()
        let phonePriv = try P256.KeyAgreement.PrivateKey(rawRepresentation: hex(v["phonePrivate"]!))
        let pcPub = hex(v["pcPublic"]!)
        let phonePub = hex(v["phonePublic"]!)

        XCTAssertEqual(hex(Crypto.encodePublic(phonePriv.publicKey)), v["phonePublic"])
        XCTAssertEqual(hex(Crypto.sha256(pcPub)), v["commit"])

        let z = try Crypto.ecdh(phonePriv, Crypto.decodePublic(pcPub))
        XCTAssertEqual(hex(z), v["Z"])
        // deviceId is upper-case in the vectors (as Swift writes UUIDs): used verbatim.
        let t = Crypto.transcript(clientId: v["clientId"]!, deviceId: v["deviceId"]!, pcPub: pcPub, phonePub: phonePub)
        XCTAssertEqual(hex(t), v["T"])
        XCTAssertEqual(Crypto.sas(t), v["SAS"])
        let kPair = Crypto.pairKey(z: z, t: t)
        XCTAssertEqual(hex(kPair), v["K_pair"])
        XCTAssertEqual(hex(Crypto.pcMac(kPair: kPair, t: t)), v["macPc"])
        XCTAssertEqual(hex(Crypto.phoneMac(kPair: kPair, t: t)), v["macPhone"])
        let up = Crypto.upKey(z: z, kPair: kPair, t: t)
        XCTAssertEqual(hex(up), v["K_up"])
        XCTAssertEqual(hex(Crypto.downKey(z: z, kPair: kPair, t: t)), v["K_down"])

        // Open the reference record the PC sealed, and seal it again identically.
        let record = hex(v["sealedRecord"]!)
        let sealed = record.dropFirst(13)
        let plain = try Crypto.open(key: up, direction: Crypto.dirUp, counter: 0, sealed: Data(sealed))
        XCTAssertEqual(hex(plain), v["sealedPlaintext"])
        XCTAssertEqual(hex(try Crypto.seal(key: up, direction: Crypto.dirUp, counter: 0, plaintext: plain)), hex(Data(sealed)))
    }
}

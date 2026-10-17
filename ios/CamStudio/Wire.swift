import Foundation
import Network

/// Record framing of docs/protocol.md section 2 over an NWConnection.
enum Wire {
    static let handshake: UInt8 = 0x01
    static let sealed: UInt8 = 0x02

    static let kindControl: UInt8 = 1
    static let kindVideoConfig: UInt8 = 2
    static let kindVideoFrame: UInt8 = 3

    static let flagKeyframe: UInt32 = 1

    static let maxHandshake = 64 * 1024
    static let maxSealed = 16 * 1024 * 1024
}

enum WireError: Error {
    case closed
    case protocolError(String)
}

/// Blocking-style reads on top of NWConnection, for the session's own
/// receive thread. Sends are queued by Network.framework.
final class Connection {
    let nw: NWConnection
    private let queue = DispatchQueue(label: "camstudio.connection")

    init(_ connection: NWConnection) {
        nw = connection
    }

    func start() {
        nw.start(queue: queue)
    }

    func close() {
        nw.cancel()
    }

    func readExact(_ count: Int) throws -> Data {
        let done = DispatchSemaphore(value: 0)
        var result: Data?
        var failed = false
        nw.receive(minimumIncompleteLength: count, maximumLength: count) { data, _, isComplete, error in
            if let data, data.count == count { result = data } else if error != nil || isComplete { failed = true }
            done.signal()
        }
        done.wait()
        guard let result, !failed else { throw WireError.closed }
        return result
    }

    func send(_ data: Data) {
        nw.send(content: data, completion: .contentProcessed { _ in })
    }

    /// Sends and waits until handed to the network stack (for backpressure).
    func sendAndWait(_ data: Data) throws {
        let done = DispatchSemaphore(value: 0)
        var failed = false
        nw.send(content: data, completion: .contentProcessed { error in
            failed = error != nil
            done.signal()
        })
        done.wait()
        if failed { throw WireError.closed }
    }

    // ---- handshake records ----

    func readHandshake() throws -> [String: Any] {
        let head = try readExact(5)
        let length = Int(head.prefix(4).reduce(UInt32(0)) { ($0 << 8) | UInt32($1) })
        guard length >= 1, length <= Wire.maxHandshake, head[head.startIndex + 4] == Wire.handshake else {
            throw WireError.protocolError("bad handshake record")
        }
        let body = try readExact(length - 1)
        guard let json = try JSONSerialization.jsonObject(with: body) as? [String: Any] else {
            throw WireError.protocolError("handshake is not a JSON object")
        }
        return json
    }

    func writeHandshake(_ json: [String: Any]) throws {
        let body = try JSONSerialization.data(withJSONObject: json)
        try sendAndWait(Self.u32(UInt32(body.count + 1)) + Data([Wire.handshake]) + body)
    }

    static func u32(_ v: UInt32) -> Data { withUnsafeBytes(of: v.bigEndian) { Data($0) } }
    static func u64(_ v: UInt64) -> Data { withUnsafeBytes(of: v.bigEndian) { Data($0) } }
    static func i64(_ v: Int64) -> Data { withUnsafeBytes(of: v.bigEndian) { Data($0) } }
}

/// AES-GCM sealed records with strict per-direction counters.
final class SecureChannel {
    struct Message {
        let kind: UInt8
        let ptsUs: Int64
        let flags: UInt32
        let payload: Data

        func json() -> [String: Any]? { try? JSONSerialization.jsonObject(with: payload) as? [String: Any] }
    }

    private let connection: Connection
    private let sendKey: Data
    private let receiveKey: Data
    private var sendCounter: UInt64 = 0
    private var receiveCounter: UInt64 = 0
    private let sendLock = NSLock()

    init(connection: Connection, sendKey: Data, receiveKey: Data) {
        self.connection = connection
        self.sendKey = sendKey
        self.receiveKey = receiveKey
    }

    /// Seals and sends; waits for the network stack so a slow link pushes back.
    func send(kind: UInt8, payload: Data, ptsUs: Int64 = 0, flags: UInt32 = 0) throws {
        sendLock.lock()
        defer { sendLock.unlock() }
        let plain = Data([kind]) + Connection.i64(ptsUs) + Connection.u32(flags) + payload
        let counter = sendCounter
        sendCounter += 1
        let sealed = try Crypto.seal(key: sendKey, direction: Crypto.dirDown, counter: counter, plaintext: plain)
        let body = Data([Wire.sealed]) + Connection.u64(counter) + sealed
        try connection.sendAndWait(Connection.u32(UInt32(body.count)) + body)
    }

    func sendJson(_ json: [String: Any]) throws {
        try send(kind: Wire.kindControl, payload: JSONSerialization.data(withJSONObject: json))
    }

    func receive() throws -> Message {
        let head = try connection.readExact(13)
        let length = Int(head.prefix(4).reduce(UInt32(0)) { ($0 << 8) | UInt32($1) })
        guard head[head.startIndex + 4] == Wire.sealed, length >= 1 + 8 + 16, length <= Wire.maxSealed else {
            throw WireError.protocolError("bad sealed record")
        }
        let counter = head.dropFirst(5).prefix(8).reduce(UInt64(0)) { ($0 << 8) | UInt64($1) }
        guard counter == receiveCounter else { throw WireError.protocolError("record counter out of order") }
        receiveCounter += 1
        let sealed = try connection.readExact(length - 9)
        let plain = try Crypto.open(key: receiveKey, direction: Crypto.dirUp, counter: counter, sealed: sealed)
        guard plain.count >= 13 else { throw WireError.protocolError("short record") }
        let p = plain.startIndex
        let pts = plain[(p + 1)..<(p + 9)].reduce(Int64(0)) { ($0 << 8) | Int64($1) }
        let flags = plain[(p + 9)..<(p + 13)].reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
        return Message(kind: plain[p], ptsUs: pts, flags: flags, payload: Data(plain[(p + 13)...]))
    }
}

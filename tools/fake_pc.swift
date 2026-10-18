// A stand-in for the Windows PC app, so the phone apps can be tested end to end from a Mac
// (docs/protocol.md): discovery, pairing, caps, streaming from every lens, controls. Video is
// decoded with VideoToolbox like the PC decodes it with Media Foundation, and the first frame of
// each lens is saved upright as a PNG (and the last one as <lens>-last.png).
//
// Build and run on a Mac: scripts/fake-pc.sh [options], which compiles
//   swiftc -O -parse-as-library -o build/fake-pc tools/fake_pc.swift ios/CamStudio/Crypto.swift
// Options:
//   build/fake-pc                                   find the phone over mDNS, stream its first lens 5 s
//   build/fake-pc --host 192.168.1.20               connect by address (port 47321)
//   build/fake-pc --lens all --seconds 4            every lens in caps, one after another
//   build/fake-pc --lens back-wide-1 --width 3840 --height 2160 --fps 30 --codec hevc
//   build/fake-pc --set '{"zoom":2,"torch":0.5}'    send a "set" part way through each stream
//   build/fake-pc --effects                         open the iPhone's video-effects panel while streaming
//   build/fake-pc --screen                          the iPhone's shared screen (TXT src=screen) instead
//   build/fake-pc --usb [--udid <udid>]             over the USB cable (usbmuxd, docs/protocol.md 1.2), no Wi‑Fi
//   build/fake-pc --forget                          drop saved pairing keys, pair again
//
// The PC side of pairing is accepted automatically; the code is printed so it can be compared
// with the one on the phone, where the person still has to press "Đồng ý".

import CoreImage
import CryptoKit
import Foundation
import Network
import VideoToolbox

// MARK: - options

struct Options {
    var host: String?
    var port: UInt16 = 47321
    var lens = "first"
    var width: Int?
    var height: Int?
    var fps: Int?
    var codec: String?
    var bitrateKbps = 8000
    var seconds = 5.0
    var set: [String: Any]?
    var effects = false
    var screen = false
    var forget = false
    var usb = false
    var udid: String?
    var out = "build/fake-pc-out"

    static func parse() -> Options {
        var o = Options()
        var args = CommandLine.arguments.dropFirst().makeIterator()
        func value(_ name: String) -> String {
            guard let v = args.next() else { fail("\(name) needs a value") }
            return v
        }
        while let a = args.next() {
            switch a {
            case "--host":
                let v = value(a)
                if let colon = v.lastIndex(of: ":"), let p = UInt16(v[v.index(after: colon)...]) {
                    o.host = String(v[..<colon]); o.port = p
                } else { o.host = v }
            case "--lens": o.lens = value(a)
            case "--width": o.width = Int(value(a))
            case "--height": o.height = Int(value(a))
            case "--fps": o.fps = Int(value(a))
            case "--codec": o.codec = value(a)
            case "--bitrate": o.bitrateKbps = Int(value(a)) ?? 8000
            case "--seconds": o.seconds = Double(value(a)) ?? 5
            case "--set":
                guard let json = try? JSONSerialization.jsonObject(with: Data(value(a).utf8)) as? [String: Any] else {
                    fail("--set needs a JSON object")
                }
                o.set = json
            case "--effects": o.effects = true
            case "--screen": o.screen = true
            case "--forget": o.forget = true
            case "--usb": o.usb = true
            case "--udid": o.usb = true; o.udid = value(a)
            case "--out": o.out = value(a)
            default: fail("unknown option \(a) (see the header of tools/fake_pc.swift)")
            }
        }
        return o
    }
}

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data("fake-pc: \(message)\n".utf8))
    exit(1)
}

func log(_ message: String) {
    let t = String(format: "%.3f", Date().timeIntervalSince(started))
    print("[\(t)] \(message)")
    fflush(stdout)
}

let started = Date()

// MARK: - this "PC"'s identity and pairing keys

struct PcState: Codable {
    var clientId = UUID().uuidString.lowercased() // like the C# PC app
    var keys: [String: Data] = [:]                // deviceId -> K_pair

    static let url = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/CamStudioFakePC/state.json")

    static func load() -> PcState {
        (try? JSONDecoder().decode(PcState.self, from: Data(contentsOf: url))) ?? PcState()
    }

    func save() {
        try? FileManager.default.createDirectory(at: Self.url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? JSONEncoder().encode(self).write(to: Self.url)
    }
}

// MARK: - discovery

/// The first phone found: its camera service, or with `screen` its shared-screen service.
func discover(timeout: TimeInterval, screen: Bool) -> NWEndpoint {
    let browser = NWBrowser(for: .bonjourWithTXTRecord(type: "_camstudio._tcp", domain: nil), using: .tcp)
    let found = DispatchSemaphore(value: 0)
    var endpoint: NWEndpoint?
    browser.stateUpdateHandler = { state in
        if case .failed(let error) = state { log("mDNS browse failed: \(error)") }
        if case .waiting(let error) = state { log("mDNS browse waiting: \(error) (Local Network permission?)") }
    }
    browser.browseResultsChangedHandler = { results, _ in
        for r in results {
            var txt = ""
            if case .bonjour(let record) = r.metadata {
                txt = record.dictionary.sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }.joined(separator: " ")
            }
            log("found \(r.endpoint)  \(txt)")
        }
        let wanted = results.first { r in
            guard case .bonjour(let record) = r.metadata else { return !screen }
            return (record["src"] == "screen") == screen
        }
        if endpoint == nil, let wanted {
            endpoint = wanted.endpoint
            found.signal()
        }
    }
    browser.start(queue: DispatchQueue(label: "browse"))
    log("browsing _camstudio._tcp for \(screen ? "a shared screen" : "a phone camera") …")
    if found.wait(timeout: .now() + timeout) == .timedOut {
        fail("no phone found over mDNS in \(Int(timeout)) s; try --host <ip shown in the phone app>")
    }
    browser.cancel()
    return endpoint!
}

// MARK: - USB cable (usbmuxd, docs/protocol.md 1.2)

/// A socket tunnelled by usbmuxd to `port` on the first iPhone on a USB cable (or the one with
/// `udid`): the same protocol as over Wi‑Fi, just another TCP pipe.
func usbmuxTunnel(udid: String?, port: UInt16) -> Int32 {
    func open() -> Int32 {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        withUnsafeMutableBytes(of: &addr.sun_path) { p in
            _ = "/var/run/usbmuxd".withCString { strcpy(p.baseAddress!.assumingMemoryBound(to: CChar.self), $0) }
        }
        let ok = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
        }
        guard fd >= 0, ok == 0 else { fail("usbmuxd is not running") }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        return fd
    }
    // 16-byte little-endian header (length, version 1, type 8 = plist, tag), then an XML plist.
    func request(_ fd: Int32, _ message: [String: Any]) -> [String: Any] {
        var m = message
        m["ProgName"] = "fake-pc"
        m["ClientVersionString"] = "fake-pc"
        m["kLibUSBMuxVersion"] = 3
        let body = try! PropertyListSerialization.data(fromPropertyList: m, format: .xml, options: 0)
        let packet = [UInt32(16 + body.count), 1, 8, 1].map { $0.littleEndian }.withUnsafeBytes { Data($0) } + body
        _ = packet.withUnsafeBytes { send(fd, $0.baseAddress!, packet.count, 0) }
        func readCount(_ n: Int) -> Data {
            var d = Data(count: n)
            var got = 0
            while got < n {
                let r = d.withUnsafeMutableBytes { recv(fd, $0.baseAddress! + got, n - got, 0) }
                if r <= 0 { fail("usbmuxd closed the connection") }
                got += r
            }
            return d
        }
        let length = readCount(16).withUnsafeBytes { $0.loadUnaligned(as: UInt32.self).littleEndian }
        let reply = readCount(Int(length) - 16)
        return (try? PropertyListSerialization.propertyList(from: reply, format: nil)) as? [String: Any] ?? [:]
    }

    let list = open()
    let devices = (request(list, ["MessageType": "ListDevices"])["DeviceList"] as? [[String: Any]] ?? [])
        .filter { ($0["Properties"] as? [String: Any])?["ConnectionType"] as? String == "USB" }
    close(list)
    func serial(_ d: [String: Any]) -> String { (d["Properties"] as? [String: Any])?["SerialNumber"] as? String ?? "?" }
    devices.forEach { log("usbmux: iPhone \(serial($0)) on USB, DeviceID \($0["DeviceID"] ?? "?")") }
    guard let device = devices.first(where: { udid == nil || serial($0) == udid }), let id = device["DeviceID"] as? Int else {
        fail("no iPhone on a USB cable\(udid.map { " with UDID \($0)" } ?? "")")
    }
    let fd = open()
    // PortNumber is in network byte order, read as a little-endian integer.
    let reply = request(fd, ["MessageType": "Connect", "DeviceID": id, "PortNumber": Int(port.byteSwapped)])
    guard reply["Number"] as? Int == 0 else {
        fail("usbmuxd Connect to port \(port) failed: result \(reply["Number"] ?? "?") (3 = refused: is the app open on the iPhone?)")
    }
    log("USB tunnel to \(serial(device)) port \(port)")
    return fd
}

// MARK: - framing (docs/protocol.md 2)

enum ProtocolError: Error, CustomStringConvertible {
    case closed
    case bad(String)
    var description: String {
        switch self {
        case .closed: return "connection closed"
        case .bad(let s): return s
        }
    }
}

final class Link {
    private let nw: NWConnection?
    private let fd: Int32 // a connected socket instead of nw (USB tunnel)
    private let queue = DispatchQueue(label: "link")

    init(_ endpoint: NWEndpoint) {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        nw = NWConnection(to: endpoint, using: NWParameters(tls: nil, tcp: tcp))
        fd = -1
    }

    init(fd: Int32) {
        nw = nil
        self.fd = fd
    }

    func close() {
        nw?.cancel()
        if fd >= 0 { Darwin.close(fd) }
    }

    func connect() throws {
        guard let nw else { return }
        let ready = DispatchSemaphore(value: 0)
        var failure: Error?
        nw.stateUpdateHandler = { state in
            switch state {
            case .ready: ready.signal()
            case .failed(let e): failure = e; ready.signal()
            case .waiting(let e): log("connect waiting: \(e)")
            default: break
            }
        }
        nw.start(queue: queue)
        if ready.wait(timeout: .now() + 10) == .timedOut { throw ProtocolError.bad("connect timed out") }
        if let failure { throw failure }
        if let path = nw.currentPath, let remote = path.remoteEndpoint { log("connected to \(remote)") }
    }

    func read(_ count: Int) throws -> Data {
        guard let nw else {
            var data = Data(count: count)
            var got = 0
            while got < count {
                let n = data.withUnsafeMutableBytes { recv(fd, $0.baseAddress! + got, count - got, 0) }
                if n <= 0 { throw ProtocolError.closed }
                got += n
            }
            return data
        }
        let done = DispatchSemaphore(value: 0)
        var result: Data?
        nw.receive(minimumIncompleteLength: count, maximumLength: count) { data, _, _, _ in
            if let data, data.count == count { result = data }
            done.signal()
        }
        done.wait()
        guard let result else { throw ProtocolError.closed }
        return result
    }

    func write(_ data: Data) throws {
        guard let nw else {
            var sent = 0
            while sent < data.count {
                let n = data.withUnsafeBytes { send(fd, $0.baseAddress! + sent, data.count - sent, 0) }
                if n <= 0 { throw ProtocolError.closed }
                sent += n
            }
            return
        }
        let done = DispatchSemaphore(value: 0)
        var failed: Error?
        nw.send(content: data, completion: .contentProcessed { e in failed = e; done.signal() })
        done.wait()
        if let failed { throw failed }
    }

    static func u32(_ v: UInt32) -> Data { withUnsafeBytes(of: v.bigEndian) { Data($0) } }
    static func u64(_ v: UInt64) -> Data { withUnsafeBytes(of: v.bigEndian) { Data($0) } }
    static func be<T: FixedWidthInteger>(_ d: Data) -> T { d.reduce(T(0)) { ($0 << 8) | T($1) } }

    func sendHandshake(_ json: [String: Any]) throws {
        let body = try JSONSerialization.data(withJSONObject: json)
        try write(Self.u32(UInt32(body.count + 1)) + Data([0x01]) + body)
    }

    func readHandshake() throws -> [String: Any] {
        let head = try read(5)
        let length: UInt32 = Self.be(head.prefix(4))
        guard head.last == 0x01, length >= 1, length <= 64 * 1024 else {
            throw ProtocolError.bad("expected a HANDSHAKE record, got type \(head.last!) length \(length)")
        }
        let body = try read(Int(length) - 1)
        guard let json = try JSONSerialization.jsonObject(with: body) as? [String: Any] else {
            throw ProtocolError.bad("handshake body is not a JSON object")
        }
        if json["type"] as? String == "error" {
            throw ProtocolError.bad("phone error \(json["code"] ?? ""): \(json["message"] ?? "")")
        }
        return json
    }
}

/// SEALED records, PC side: send with K_up (direction 1), receive with K_down (direction 2).
final class Channel {
    struct Message { let kind: UInt8; let ptsUs: Int64; let flags: UInt32; let payload: Data }

    private let link: Link
    private let up: Data
    private let down: Data
    private var sendCounter: UInt64 = 0
    private var receiveCounter: UInt64 = 0
    private let sendLock = NSLock()

    init(link: Link, up: Data, down: Data) {
        self.link = link
        self.up = up
        self.down = down
    }

    func sendControl(_ json: [String: Any]) throws {
        let payload = try JSONSerialization.data(withJSONObject: json)
        sendLock.lock()
        defer { sendLock.unlock() }
        let plain = Data([1]) + Link.u64(0) + Link.u32(0) + payload
        let sealed = try Crypto.seal(key: up, direction: Crypto.dirUp, counter: sendCounter, plaintext: plain)
        let body = Data([0x02]) + Link.u64(sendCounter) + sealed
        sendCounter += 1
        try link.write(Link.u32(UInt32(body.count)) + body)
    }

    func receive() throws -> Message {
        let head = try link.read(13)
        let length: UInt32 = Link.be(head.prefix(4))
        guard head[head.startIndex + 4] == 0x02, length >= 1 + 8 + 16 + 13, length <= 16 * 1024 * 1024 else {
            throw ProtocolError.bad("bad SEALED record header (type \(head[head.startIndex + 4]), length \(length))")
        }
        let counter: UInt64 = Link.be(head.suffix(8))
        guard counter == receiveCounter else { throw ProtocolError.bad("counter \(counter), expected \(receiveCounter)") }
        receiveCounter += 1
        let plain = try Crypto.open(key: down, direction: Crypto.dirDown, counter: counter, sealed: try link.read(Int(length) - 9))
        let p = plain.startIndex
        return Message(kind: plain[p], ptsUs: Int64(bitPattern: Link.be(plain[(p + 1)..<(p + 9)])),
                       flags: Link.be(plain[(p + 9)..<(p + 13)]), payload: Data(plain[(p + 13)...]))
    }
}

// MARK: - handshake and pairing (docs/protocol.md 3)

func handshake(_ link: Link, state: inout PcState) throws -> (Channel, deviceName: String) {
    let key = P256.KeyAgreement.PrivateKey()
    let pcPub = Crypto.encodePublic(key.publicKey)
    try link.sendHandshake([
        "type": "hello", "v": 1, "clientId": state.clientId, "clientName": "Fake PC (\(Host.current().localizedName ?? "Mac"))",
        "commit": Crypto.sha256(pcPub).base64EncodedString(),
    ])
    let hello = try link.readHandshake()
    guard hello["type"] as? String == "hello", let deviceId = hello["deviceId"] as? String,
          let pubB64 = hello["pub"] as? String, let phonePub = Data(base64Encoded: pubB64), phonePub.count == 65 else {
        throw ProtocolError.bad("bad phone hello: \(hello)")
    }
    let name = hello["name"] as? String ?? "?"
    let phonePaired = hello["paired"] as? Bool ?? false
    log("phone hello: \(name) model=\(hello["model"] ?? "?") os=\(hello["os"] ?? "?") deviceId=\(deviceId) paired=\(phonePaired)")
    try link.sendHandshake(["type": "reveal", "pub": pcPub.base64EncodedString()])

    let z = try Crypto.ecdh(key, Crypto.decodePublic(phonePub))
    let t = Crypto.transcript(clientId: state.clientId, deviceId: deviceId, pcPub: pcPub, phonePub: phonePub)

    var kPair = state.keys[deviceId]
    var newlyPaired = false
    if kPair == nil || !phonePaired {
        if kPair != nil { log("phone asks to pair again (old key kept until this succeeds)") }
        try link.sendHandshake(["type": "pairStart"])
        let code = Crypto.sas(t)
        log("PAIRING CODE  \(code.prefix(3)) \(code.suffix(3))  -> check it matches the phone and press \"Đồng ý\" there")
        let pair = try link.readHandshake()
        guard pair["type"] as? String == "pair", pair["accepted"] as? Bool == true else {
            throw ProtocolError.bad("pairing declined on the phone: \(pair)")
        }
        log("phone accepted pairing")
        kPair = Crypto.pairKey(z: z, t: t)
        newlyPaired = true
    } else {
        log("already paired: authenticating with the saved key")
    }

    try link.sendHandshake(["type": "auth", "mac": Crypto.pcMac(kPair: kPair!, t: t).base64EncodedString()])
    let auth = try link.readHandshake()
    guard auth["type"] as? String == "auth", let macB64 = auth["mac"] as? String, let mac = Data(base64Encoded: macB64),
          Crypto.equal(mac, Crypto.phoneMac(kPair: kPair!, t: t)) else {
        throw ProtocolError.bad("phone auth MAC is wrong: \(auth)")
    }
    log("mutual auth OK\(newlyPaired ? " (new pairing saved)" : "")")
    state.keys[deviceId] = kPair
    state.save()
    let channel = Channel(link: link, up: Crypto.upKey(z: z, kPair: kPair!, t: t), down: Crypto.downKey(z: z, kPair: kPair!, t: t))
    return (channel, name)
}

// MARK: - video decoding (VideoToolbox, like the PC's Media Foundation decoder)

final class Decoder {
    let hevc: Bool
    private var format: CMVideoFormatDescription?
    private var session: VTDecompressionSession?
    private(set) var decoded = 0
    private(set) var errors = 0
    private(set) var fullRange: Bool?
    private(set) var decodedSize: (Int, Int)?
    var firstImage: CVPixelBuffer?
    var lastImage: CVPixelBuffer?

    init(hevc: Bool) { self.hevc = hevc }

    static func nals(_ annexB: Data) -> [Data] {
        let b = [UInt8](annexB)
        var starts: [(Int, Int)] = [] // (start code index, payload index)
        var i = 0
        while i + 3 <= b.count {
            if b[i] == 0, b[i + 1] == 0, b[i + 2] == 1 {
                starts.append((i > 0 && b[i - 1] == 0 ? i - 1 : i, i + 3)); i += 3
            } else { i += 1 }
        }
        return starts.enumerated().map { n, s in
            let end = n + 1 < starts.count ? starts[n + 1].0 : b.count
            return Data(b[s.1..<end])
        }
    }

    func nalType(_ nal: Data) -> Int { hevc ? Int(nal.first! >> 1) & 0x3F : Int(nal.first!) & 0x1F }

    func configure(_ config: Data) {
        let sets = Self.nals(config).filter { !$0.isEmpty }
        var desc: CMFormatDescription?
        let status: OSStatus = withPointers(sets) { pointers, sizes in
            hevc
                ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(allocator: nil, parameterSetCount: sets.count, parameterSetPointers: pointers,
                                                                      parameterSetSizes: sizes, nalUnitHeaderLength: 4, extensions: nil, formatDescriptionOut: &desc)
                : CMVideoFormatDescriptionCreateFromH264ParameterSets(allocator: nil, parameterSetCount: sets.count, parameterSetPointers: pointers,
                                                                      parameterSetSizes: sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &desc)
        }
        guard status == noErr, let desc else {
            log("  !! VIDEO_CONFIG rejected by VideoToolbox (\(status)); NAL types \(sets.map(nalType))")
            errors += 1
            return
        }
        if let format, CMFormatDescriptionEqual(format, otherFormatDescription: desc) { return }
        let ext = CMFormatDescriptionGetExtension(desc, extensionKey: kCMFormatDescriptionExtension_FullRangeVideo) as? Bool
        fullRange = ext ?? false
        let dims = CMVideoFormatDescriptionGetDimensions(desc)
        log("  VIDEO_CONFIG: NAL types \(sets.map(nalType)), \(dims.width)×\(dims.height), full range: \(fullRange!)")
        format = desc
        if let session { VTDecompressionSessionInvalidate(session) }
        session = nil
        let attrs: [CFString: Any] = [kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA]
        let s = VTDecompressionSessionCreate(allocator: nil, formatDescription: desc, decoderSpecification: nil,
                                             imageBufferAttributes: attrs as CFDictionary, outputCallback: nil,
                                             decompressionSessionOut: &session)
        if s != noErr { log("  !! VTDecompressionSessionCreate \(s)"); errors += 1 }
    }

    func decode(_ frame: Data) {
        guard let format, let session else { errors += 1; return }
        var avcc = Data()
        for nal in Self.nals(frame) where !nal.isEmpty {
            avcc += Link.u32(UInt32(nal.count)) + nal
        }
        var block: CMBlockBuffer?
        let length = avcc.count
        guard CMBlockBufferCreateWithMemoryBlock(allocator: nil, memoryBlock: nil, blockLength: length, blockAllocator: nil,
                                                 customBlockSource: nil, offsetToData: 0, dataLength: length, flags: 0,
                                                 blockBufferOut: &block) == noErr, let block else { errors += 1; return }
        _ = avcc.withUnsafeBytes { CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block, offsetIntoDestination: 0, dataLength: length) }
        var sample: CMSampleBuffer?
        var size = length
        guard CMSampleBufferCreateReady(allocator: nil, dataBuffer: block, formatDescription: format, sampleCount: 1,
                                        sampleTimingEntryCount: 0, sampleTimingArray: nil, sampleSizeEntryCount: 1,
                                        sampleSizeArray: &size, sampleBufferOut: &sample) == noErr, let sample else { errors += 1; return }
        let status = VTDecompressionSessionDecodeFrame(session, sampleBuffer: sample, flags: [], infoFlagsOut: nil) { status, _, image, _, _ in
            if status == noErr, let image {
                self.decoded += 1
                self.decodedSize = (CVPixelBufferGetWidth(image), CVPixelBufferGetHeight(image))
                if self.firstImage == nil { self.firstImage = image }
                self.lastImage = image
            } else {
                self.errors += 1
            }
        }
        if status != noErr { errors += 1 }
    }

    func finish() {
        if let session {
            VTDecompressionSessionWaitForAsynchronousFrames(session)
            VTDecompressionSessionInvalidate(session)
        }
        session = nil
    }
}

func withPointers<R>(_ sets: [Data], _ body: ([UnsafePointer<UInt8>], [Int]) -> R) -> R {
    let buffers = sets.map { d -> UnsafeMutablePointer<UInt8> in
        let p = UnsafeMutablePointer<UInt8>.allocate(capacity: d.count)
        d.copyBytes(to: p, count: d.count)
        return p
    }
    defer { buffers.forEach { $0.deallocate() } }
    return body(buffers.map { UnsafePointer($0) }, sets.map(\.count))
}

/// Saves the image the way the PC shows it: rotated clockwise by the phone's "orientation".
func savePNG(_ image: CVPixelBuffer, orientation: Int, to url: URL) {
    var ci = CIImage(cvPixelBuffer: image)
    switch (orientation % 360 + 360) % 360 {
    case 90: ci = ci.oriented(.right)
    case 180: ci = ci.oriented(.down)
    case 270: ci = ci.oriented(.left)
    default: break
    }
    try? CIContext().writePNGRepresentation(of: ci, to: url, format: .RGBA8, colorSpace: CGColorSpace(name: CGColorSpace.sRGB)!)
}

// MARK: - session with the phone

final class PhoneLink {
    let channel: Channel
    private let lock = NSCondition()
    private var controls: [[String: Any]] = []
    private(set) var closedError: Error?

    // Per-stream counters, reset by beginStream().
    var decoder: Decoder?
    var rawFile: FileHandle?
    var frames = 0, keyframes = 0, bytes = 0, configs = 0
    var lastPts: Int64 = 0, firstPts: Int64 = 0
    var lastState: [String: Any] = [:]
    var stateCount = 0
    var orientation = 90

    init(channel: Channel) {
        self.channel = channel
        Thread { self.receiveLoop() }.start()
    }

    private func receiveLoop() {
        do {
            while true {
                let m = try channel.receive()
                lock.lock()
                switch m.kind {
                case 1:
                    if let json = try? JSONSerialization.jsonObject(with: m.payload) as? [String: Any] {
                        handleControl(json)
                    }
                case 2:
                    configs += 1
                    rawFile?.write(m.payload)
                    decoder?.configure(m.payload)
                case 3:
                    if frames == 0 { firstPts = m.ptsUs }
                    frames += 1
                    bytes += m.payload.count
                    lastPts = m.ptsUs
                    if m.flags & 1 != 0 { keyframes += 1 }
                    rawFile?.write(m.payload)
                    decoder?.decode(m.payload)
                default:
                    log("unknown record kind \(m.kind)")
                }
                lock.broadcast()
                lock.unlock()
            }
        } catch {
            lock.lock()
            closedError = error
            lock.broadcast()
            lock.unlock()
        }
    }

    private func handleControl(_ json: [String: Any]) {
        let type = json["type"] as? String ?? "?"
        switch type {
        case "state":
            stateCount += 1
            lastState = json
            if let o = json["orientation"] as? Int, o != orientation {
                log("  orientation -> \(o)")
                orientation = o
            }
        case "bitrate":
            log("  phone bitrate -> \(json["kbps"] ?? "?") kbps")
        default:
            if type != "caps" { log("  <- \(type) \(compact(json))") }
            controls.append(json)
        }
    }

    /// Waits for a control message of one of `types` (an "error" always ends the wait).
    func waitFor(_ types: Set<String>, timeout: TimeInterval) -> [String: Any]? {
        let deadline = Date().addingTimeInterval(timeout)
        lock.lock()
        defer { lock.unlock() }
        while true {
            if let i = controls.firstIndex(where: { types.contains($0["type"] as? String ?? "") || $0["type"] as? String == "error" }) {
                return controls.remove(at: i)
            }
            if closedError != nil || !lock.wait(until: deadline) { return nil }
        }
    }

    func send(_ json: [String: Any]) {
        do { try channel.sendControl(json) } catch { log("send failed: \(error)") }
    }

    func withLock<R>(_ body: () -> R) -> R {
        lock.lock(); defer { lock.unlock() }
        return body()
    }

    func beginStream(hevc: Bool, raw: URL) {
        withLock {
            decoder = Decoder(hevc: hevc)
            FileManager.default.createFile(atPath: raw.path, contents: nil)
            rawFile = try? FileHandle(forWritingTo: raw)
            frames = 0; keyframes = 0; bytes = 0; configs = 0; stateCount = 0; lastState = [:]
        }
    }
}

func compact(_ json: [String: Any]) -> String {
    guard let d = try? JSONSerialization.data(withJSONObject: json, options: [.sortedKeys]) else { return "\(json)" }
    let s = String(decoding: d, as: UTF8.self)
    return s.count > 300 ? String(s.prefix(300)) + "…" : s
}

// MARK: - caps report

func printCaps(_ caps: [String: Any]) {
    let device = caps["device"] as? [String: Any] ?? [:]
    log("caps: \(device["name"] ?? "?") \(device["model"] ?? "?") \(device["os"] ?? "?") \(device["osVersion"] ?? "")")
    let modes = caps["modes"] as? [[String: Any]] ?? []
    for lens in caps["lenses"] as? [[String: Any]] ?? [] {
        let id = lens["id"] as? String ?? "?"
        let zoom = lens["zoom"] as? [String: Any] ?? [:]
        let mine = modes.filter { $0["lens"] as? String == id }
        let sizes = mine.prefix(6).map { "\($0["width"] ?? 0)×\($0["height"] ?? 0)@\(($0["fps"] as? [Int])?.last ?? 0)" }
        let members = (lens["members"] as? [String]).map { " members=\($0.joined(separator: ","))" } ?? ""
        log("  lens \(id)  kind=\(lens["kind"] ?? "?") \"\(lens["label"] ?? "")\" f35=\(lens["focalLength35mm"] ?? "-")"
            + " cinematic=\(lens["cinematic"] ?? "-") zoom=\(compact(zoom))\(members)")
        log("     \(mine.count) modes: \(sizes.joined(separator: ", "))\(mine.count > 6 ? ", …" : "")")
        if let controls = lens["controls"] as? [String: Any] { log("     controls: \(compact(controls))") }
    }
    log("  effects: \(compact(caps["effects"] as? [String: Any] ?? [:]))")
    log("  encoder: \(compact(caps["encoder"] as? [String: Any] ?? [:]))")
}

/// The requested mode, or 1920×1080 (or the closest smaller size) at 30 fps with H.264, like the PC's default.
func pickMode(_ caps: [String: Any], lens: String, o: Options) -> (Int, Int, Int, String)? {
    let modes = (caps["modes"] as? [[String: Any]] ?? []).filter { $0["lens"] as? String == lens }
    func fits(_ m: [String: Any]) -> Bool {
        (o.width == nil || m["width"] as? Int == o.width) && (o.height == nil || m["height"] as? Int == o.height)
    }
    let candidates = modes.filter(fits)
    let preferred = candidates.first { $0["width"] as? Int == 1920 && $0["height"] as? Int == 1080 }
        ?? candidates.first { ($0["width"] as? Int ?? 0) <= 1920 }
        ?? candidates.first
    guard let m = preferred else { return nil }
    let fpsList = m["fps"] as? [Int] ?? [30]
    let fps = o.fps ?? (fpsList.contains(30) ? 30 : fpsList.last ?? 30)
    let codecs = m["codecs"] as? [String] ?? ["h264"]
    let codec = o.codec ?? (codecs.contains("h264") ? "h264" : codecs.first ?? "hevc")
    return (m["width"] as? Int ?? 1920, m["height"] as? Int ?? 1080, fps, codec)
}

// MARK: - main

@main
struct FakePc {
    static func main() {
        let o = Options.parse()
        var state = PcState.load()
        if o.forget { state.keys = [:]; state.save(); log("pairing keys forgotten") }
        let outDir = URL(fileURLWithPath: o.out, isDirectory: true)
        try? FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)

        let link: Link
        if o.usb {
            link = Link(fd: usbmuxTunnel(udid: o.udid, port: o.screen ? 47322 : o.port))
        } else {
            let endpoint: NWEndpoint = o.host.map { .hostPort(host: NWEndpoint.Host($0), port: NWEndpoint.Port(rawValue: o.port)!) }
                ?? discover(timeout: 15, screen: o.screen)
            link = Link(endpoint)
        }
        do {
            try link.connect()
            let (channel, _) = try handshake(link, state: &state)
            run(PhoneLink(channel: channel), o: o, outDir: outDir)
        } catch {
            fail("\(error)")
        }
        link.close()
    }

    static func run(_ phone: PhoneLink, o: Options, outDir: URL) {
        phone.send(["type": "getCaps"])
        guard let caps = phone.waitFor(["caps"], timeout: 10), caps["type"] as? String == "caps" else {
            fail("no caps from the phone \(phone.closedError.map { "(\($0))" } ?? "")")
        }
        if let d = try? JSONSerialization.data(withJSONObject: caps, options: [.prettyPrinted, .sortedKeys]) {
            try? d.write(to: outDir.appendingPathComponent("caps.json"))
        }
        printCaps(caps)

        let lensIds = (caps["lenses"] as? [[String: Any]] ?? []).compactMap { $0["id"] as? String }
        let targets: [String]
        switch o.lens {
        case "all": targets = lensIds
        case "first": targets = Array(lensIds.prefix(1))
        case "none": targets = []
        default: targets = o.lens.split(separator: ",").map(String.init)
        }

        var summary: [String] = []
        for lens in targets {
            guard let (w, h, fps, codec) = pickMode(caps, lens: lens, o: o) else {
                summary.append("\(lens): no matching mode in caps"); continue
            }
            summary.append(stream(phone, lens: lens, width: w, height: h, fps: fps, codec: codec, o: o, outDir: outDir))
            if phone.closedError != nil { break }
        }

        log("==== summary ====")
        summary.forEach { log($0) }
        log("output in \(outDir.path)")
        if let e = phone.closedError { log("connection ended: \(e)") }
    }

    static func stream(_ phone: PhoneLink, lens: String, width: Int, height: Int, fps: Int, codec: String,
                       o: Options, outDir: URL) -> String {
        let safe = lens.replacingOccurrences(of: "/", with: "_")
        let raw = outDir.appendingPathComponent("\(safe).\(codec.hasPrefix("hevc") ? "hevc" : "h264")")
        phone.beginStream(hevc: codec.hasPrefix("hevc"), raw: raw)
        log("-> start \(lens) \(width)×\(height)@\(fps) \(codec) \(o.bitrateKbps) kbps")
        let t0 = Date()
        phone.send(["type": "start", "lens": lens, "width": width, "height": height, "fps": fps, "codec": codec,
                    "bitrateKbps": o.bitrateKbps, "hdr": codec == "hevc10"])
        guard let started = phone.waitFor(["started"], timeout: 10), started["type"] as? String == "started" else {
            return "\(lens): FAILED to start \(phone.closedError.map { "(\($0))" } ?? "")"
        }
        phone.orientation = started["orientation"] as? Int ?? 90

        // First frame latency, then run, sending stats every second like the PC.
        var firstFrameMs: Int?
        var lastFrames = 0
        var lastStats = Date()
        var setSent = false
        var effectsSent = !o.effects
        let end = Date().addingTimeInterval(o.seconds)
        while Date() < end, phone.closedError == nil {
            Thread.sleep(forTimeInterval: 0.05)
            let frames = phone.withLock { phone.frames }
            if firstFrameMs == nil, frames > 0 { firstFrameMs = Int(Date().timeIntervalSince(t0) * 1000) }
            if !effectsSent, Date().timeIntervalSince(t0) > 1 {
                // Like the PC button: only useful while the camera runs, the panel shows its effects.
                effectsSent = true
                log("-> showSystemEffects (the panel should open on the phone)")
                phone.send(["type": "showSystemEffects"])
            }
            if let set = o.set, !setSent, Date() > end.addingTimeInterval(-o.seconds * 2 / 3) {
                setSent = true
                log("-> set \(compact(set))")
                phone.send(["type": "set"].merging(set) { a, _ in a })
            }
            if Date().timeIntervalSince(lastStats) >= 1 {
                lastStats = Date()
                phone.send(["type": "stats", "recvFps": frames - lastFrames, "decodeMs": 2, "queueMs": 0, "lost": 0])
                lastFrames = frames
            }
        }
        phone.send(["type": "stop"])
        _ = phone.waitFor(["stopped"], timeout: 5)

        return phone.withLock {
            phone.decoder?.finish()
            phone.rawFile?.closeFile()
            phone.rawFile = nil
            let d = phone.decoder!
            let span = Double(phone.lastPts - phone.firstPts) / 1_000_000
            let measuredFps = span > 0 ? Double(phone.frames - 1) / span : 0
            let kbps = span > 0 ? Double(phone.bytes) * 8 / span / 1000 : 0
            var png = "-"
            if let image = d.firstImage {
                let url = outDir.appendingPathComponent("\(safe).png")
                savePNG(image, orientation: phone.orientation, to: url)
                png = url.lastPathComponent
            }
            if let image = d.lastImage {
                savePNG(image, orientation: phone.orientation, to: outDir.appendingPathComponent("\(safe)-last.png"))
                png += " (+ -last.png)"
            }
            let st = phone.lastState
            let ok = phone.frames > 0 && d.errors == 0 && d.decoded > 0
            return "\(lens): \(ok ? "OK" : "PROBLEM") started=\(compact(started)) first frame \(firstFrameMs.map { "\($0) ms" } ?? "never")"
                + "\n     frames=\(phone.frames) key=\(phone.keyframes) configs=\(phone.configs) fps=\(String(format: "%.1f", measuredFps))"
                + " kbps=\(Int(kbps)) decoded=\(d.decoded) decodeErrors=\(d.errors)"
                + " size=\(d.decodedSize.map { "\($0.0)×\($0.1)" } ?? "-") fullRange=\(d.fullRange.map(String.init) ?? "-") png=\(png)"
                + "\n     state x\(phone.stateCount): zoom=\(st["zoom"] ?? "-") iso=\(st["iso"] ?? "-") exposureMs=\(st["exposureMs"] ?? "-")"
                + " orientation=\(st["orientation"] ?? "-") battery=\(st["battery"] ?? "-") thermal=\(st["thermal"] ?? "-")"
                + (o.set.map { s in " | set " + s.keys.sorted().map { "\($0)=\(st[$0] ?? "-")" }.joined(separator: " ") } ?? "")
        }
    }
}

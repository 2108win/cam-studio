"""Reference test vectors for docs/protocol.md (handshake, keys, one sealed record).

Pure Python (only hashlib/hmac from the standard library), independent of
the three real implementations: Windows BCrypt (C++), Android javax.crypto
(Kotlin) and iOS CryptoKit (Swift). Each of those must reproduce
docs/protocol-vectors.json exactly in its unit tests.

    python tools/protocol_vectors.py            # writes docs/protocol-vectors.json
"""
import hashlib
import hmac
import json
import os
import struct

# ---------------------------------------------------------------- P-256 ----
P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
A = P - 3
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
G = (0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
     0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5)


def ec_add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    (x1, y1), (x2, y2) = p1, p2
    if x1 == x2 and (y1 + y2) % P == 0:
        return None
    if p1 == p2:
        lam = (3 * x1 * x1 + A) * pow(2 * y1, -1, P) % P
    else:
        lam = (y2 - y1) * pow(x2 - x1, -1, P) % P
    x3 = (lam * lam - x1 - x2) % P
    return x3, (lam * (x1 - x3) - y1) % P


def ec_mul(k, point):
    result = None
    while k:
        if k & 1:
            result = ec_add(result, point)
        point = ec_add(point, point)
        k >>= 1
    return result


def pub_bytes(point):
    x, y = point
    return b"\x04" + x.to_bytes(32, "big") + y.to_bytes(32, "big")


# ------------------------------------------------------------- AES-256 -----
SBOX = [0] * 256


def _init_sbox():
    p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4)
        SBOX[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    SBOX[0] = 0x63


_init_sbox()


def xtime(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1


def aes256_expand(key):
    words = [list(key[i:i + 4]) for i in range(0, 32, 4)]
    rcon = 1
    for i in range(8, 60):
        t = list(words[i - 1])
        if i % 8 == 0:
            t = [SBOX[b] for b in t[1:] + t[:1]]
            t[0] ^= rcon
            rcon = xtime(rcon)
        elif i % 8 == 4:
            t = [SBOX[b] for b in t]
        words.append([a ^ b for a, b in zip(words[i - 8], t)])
    return [sum(words[r * 4:r * 4 + 4], []) for r in range(15)]


def aes256_block(round_keys, block):
    s = [b ^ k for b, k in zip(block, round_keys[0])]
    for rnd in range(1, 15):
        s = [SBOX[b] for b in s]
        s = [s[(i + 4 * (i % 4)) % 16] for i in range(16)]  # ShiftRows (column-major state)
        if rnd != 14:
            out = []
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                t = a[0] ^ a[1] ^ a[2] ^ a[3]
                out += [a[i] ^ t ^ xtime(a[i] ^ a[(i + 1) % 4]) for i in range(4)]
            s = out
        s = [b ^ k for b, k in zip(s, round_keys[rnd])]
    return bytes(s)


# ------------------------------------------------------------------ GCM ----
def gf_mul(x, y):
    r = 0xE1 << 120
    z = 0
    for i in range(127, -1, -1):
        if (y >> i) & 1:
            z ^= x
        x = (x >> 1) ^ r if x & 1 else x >> 1
    return z


def ghash(h, aad, ct):
    def blocks(data):
        for i in range(0, len(data), 16):
            yield int.from_bytes(data[i:i + 16].ljust(16, b"\0"), "big")
    y = 0
    for blk in list(blocks(aad)) + list(blocks(ct)):
        y = gf_mul(y ^ blk, h)
    lengths = (len(aad) * 8).to_bytes(8, "big") + (len(ct) * 8).to_bytes(8, "big")
    return gf_mul(y ^ int.from_bytes(lengths, "big"), h)


def aes_gcm_seal(key, nonce, aad, plaintext):
    rk = aes256_expand(key)
    h = int.from_bytes(aes256_block(rk, bytes(16)), "big")
    j0 = nonce + b"\0\0\0\1"
    ct = bytearray()
    for i in range(0, len(plaintext), 16):
        counter = nonce + struct.pack(">I", 2 + i // 16)
        ks = aes256_block(rk, counter)
        ct += bytes(a ^ b for a, b in zip(plaintext[i:i + 16], ks))
    s = ghash(h, aad, bytes(ct))
    tag = (int.from_bytes(aes256_block(rk, j0), "big") ^ s).to_bytes(16, "big")
    return bytes(ct) + tag


# ------------------------------------------------------------ protocol -----
def lp(b):
    return struct.pack(">I", len(b)) + b


def hkdf(ikm, salt, info, length=32):
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, t, i = b"", b"", 1
    while len(out) < length:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        out += t
        i += 1
    return out[:length]


def main():
    pc_priv = int.from_bytes(hashlib.sha256(b"camstudio test pc key").digest(), "big") % N
    phone_priv = int.from_bytes(hashlib.sha256(b"camstudio test phone key").digest(), "big") % N
    client_id = "3f2a1c4e-5b6d-4e7f-8a9b-0c1d2e3f4a5b"   # lowercase, as Java/C# write UUIDs
    device_id = "8C1D2E3F-4A5B-4C6D-9E7F-0A1B2C3D4E5F"   # uppercase, as Swift writes UUIDs

    pc_pub = pub_bytes(ec_mul(pc_priv, G))
    phone_pub = pub_bytes(ec_mul(phone_priv, G))
    z1 = ec_mul(pc_priv, ec_mul(phone_priv, G))[0].to_bytes(32, "big")
    z2 = ec_mul(phone_priv, ec_mul(pc_priv, G))[0].to_bytes(32, "big")
    assert z1 == z2
    z = z1

    commit = hashlib.sha256(pc_pub).digest()
    t = hashlib.sha256(lp(b"camstudio-v1") + lp(client_id.encode()) + lp(device_id.encode())
                       + lp(pc_pub) + lp(phone_pub)).digest()
    sas_hash = hashlib.sha256(b"camstudio-sas" + t).digest()
    sas = "%06d" % (struct.unpack(">I", sas_hash[:4])[0] % 1_000_000)
    k_pair = hkdf(z, t, b"camstudio pair")
    mac_pc = hmac.new(k_pair, b"pc" + t, hashlib.sha256).digest()
    mac_phone = hmac.new(k_pair, b"phone" + t, hashlib.sha256).digest()
    k_up = hkdf(z + k_pair, t, b"camstudio pc->phone")
    k_down = hkdf(z + k_pair, t, b"camstudio phone->pc")

    # One sealed record PC -> phone: CONTROL {"type":"getCaps"}, counter 0.
    payload = b'{"type":"getCaps"}'
    plaintext = bytes([1]) + struct.pack(">q", 0) + struct.pack(">I", 0) + payload
    counter = 0
    nonce = struct.pack(">I", 1) + struct.pack(">Q", counter)
    aad = bytes([0x02]) + struct.pack(">Q", counter)
    sealed = aes_gcm_seal(k_up, nonce, aad, plaintext)
    body = bytes([0x02]) + struct.pack(">Q", counter) + sealed
    record = struct.pack(">I", len(body)) + body

    vectors = {
        "comment": "Generated by tools/protocol_vectors.py; see docs/protocol.md appendix A.",
        "pcPrivate": pc_priv.to_bytes(32, "big").hex(),
        "phonePrivate": phone_priv.to_bytes(32, "big").hex(),
        "clientId": client_id,
        "deviceId": device_id,
        "pcPublic": pc_pub.hex(),
        "phonePublic": phone_pub.hex(),
        "commit": commit.hex(),
        "Z": z.hex(),
        "T": t.hex(),
        "SAS": sas,
        "K_pair": k_pair.hex(),
        "macPc": mac_pc.hex(),
        "macPhone": mac_phone.hex(),
        "K_up": k_up.hex(),
        "K_down": k_down.hex(),
        "sealedPlaintext": plaintext.hex(),
        "sealedRecord": record.hex(),
    }
    out = os.path.join(os.path.dirname(__file__), "..", "docs", "protocol-vectors.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(vectors, f, indent=2)
        f.write("\n")
    for k, v in vectors.items():
        print(f"{k:16} {v}")


if __name__ == "__main__":
    main()

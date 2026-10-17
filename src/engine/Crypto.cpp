#include "Crypto.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <stdexcept>

#pragma comment(lib, "bcrypt")
#pragma comment(lib, "crypt32")

#ifndef STATUS_AUTH_TAG_MISMATCH
#define STATUS_AUTH_TAG_MISMATCH ((NTSTATUS)0xC000A002L) // ntstatus.h
#endif

namespace crypto
{
namespace
{
void Check(NTSTATUS st, const char* what)
{
    if (!BCRYPT_SUCCESS(st)) throw std::runtime_error(std::string("BCrypt ") + what + " failed");
}

struct Alg
{
    BCRYPT_ALG_HANDLE h = nullptr;
    Alg(const wchar_t* id, ULONG flags = 0) { Check(BCryptOpenAlgorithmProvider(&h, id, nullptr, flags), "open"); }
    ~Alg() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};

// Providers are thread-safe and costly to open: keep one of each.
Alg& ShaAlg() { static Alg a(BCRYPT_SHA256_ALGORITHM); return a; }
Alg& HmacAlg() { static Alg a(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG); return a; }
Alg& EcdhAlg() { static Alg a(BCRYPT_ECDH_P256_ALGORITHM); return a; }
Alg& AesAlg()
{
    static Alg a = [] {
        Alg x(BCRYPT_AES_ALGORITHM);
        Check(BCryptSetProperty(x.h, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
            sizeof(BCRYPT_CHAIN_MODE_GCM), 0), "gcm mode");
        return x;
    }();
    return a;
}

Bytes Hash(BCRYPT_ALG_HANDLE alg, const Bytes* key, std::initializer_list<const Bytes*> parts)
{
    BCRYPT_HASH_HANDLE h = nullptr;
    Check(BCryptCreateHash(alg, &h, nullptr, 0, key ? (PUCHAR)key->data() : nullptr, key ? (ULONG)key->size() : 0, 0), "hash");
    for (const Bytes* p : parts)
        if (!p->empty()) Check(BCryptHashData(h, (PUCHAR)p->data(), (ULONG)p->size(), 0), "hash data");
    Bytes out(32);
    Check(BCryptFinishHash(h, out.data(), 32, 0), "finish hash");
    BCryptDestroyHash(h);
    return out;
}

Bytes Concat(const Bytes& a, const Bytes& b)
{
    Bytes r(a);
    r.insert(r.end(), b.begin(), b.end());
    return r;
}

void Gcm(bool encrypt, const Bytes& key, uint32_t direction, uint64_t counter,
         const uint8_t* in, size_t inSize, uint8_t* out, uint8_t* tag)
{
    uint8_t nonce[12];
    uint8_t aad[9];
    for (int i = 0; i < 4; ++i) nonce[i] = uint8_t(direction >> (24 - 8 * i));
    aad[0] = 0x02;
    for (int i = 0; i < 8; ++i) nonce[4 + i] = aad[1 + i] = uint8_t(counter >> (56 - 8 * i));

    BCRYPT_KEY_HANDLE k = nullptr;
    Check(BCryptGenerateSymmetricKey(AesAlg().h, &k, nullptr, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0), "aes key");
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = sizeof(nonce);
    info.pbAuthData = aad;
    info.cbAuthData = sizeof(aad);
    info.pbTag = tag;
    info.cbTag = 16;
    ULONG done = 0;
    NTSTATUS st = encrypt
        ? BCryptEncrypt(k, (PUCHAR)in, (ULONG)inSize, &info, nullptr, 0, out, (ULONG)inSize, &done, 0)
        : BCryptDecrypt(k, (PUCHAR)in, (ULONG)inSize, &info, nullptr, 0, out, (ULONG)inSize, &done, 0);
    BCryptDestroyKey(k);
    if (!encrypt && st == STATUS_AUTH_TAG_MISMATCH) throw std::invalid_argument("tag");
    Check(st, encrypt ? "encrypt" : "decrypt");
}
} // namespace

Bytes Sha256(std::initializer_list<const Bytes*> parts) { return Hash(ShaAlg().h, nullptr, parts); }
Bytes HmacSha256(const Bytes& key, std::initializer_list<const Bytes*> parts) { return Hash(HmacAlg().h, &key, parts); }

Bytes Hkdf(const Bytes& ikm, const Bytes& salt, const Bytes& info, size_t length)
{
    const Bytes prk = HmacSha256(salt, { &ikm });
    Bytes out, t;
    for (uint8_t i = 1; out.size() < length; ++i)
    {
        const Bytes counter{ i };
        t = HmacSha256(prk, { &t, &info, &counter });
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(length);
    return out;
}

Bytes LP(const Bytes& data)
{
    const uint32_t n = static_cast<uint32_t>(data.size());
    Bytes r{ uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n) };
    r.insert(r.end(), data.begin(), data.end());
    return r;
}

Bytes Str(const std::string& s) { return Bytes(s.begin(), s.end()); }

// ---- ECDH ----------------------------------------------------------------------

EcdhKey::EcdhKey()
{
    BCRYPT_KEY_HANDLE k = nullptr;
    Check(BCryptGenerateKeyPair(EcdhAlg().h, &k, 256, 0), "gen key");
    Check(BCryptFinalizeKeyPair(k, 0), "finalize key");
    m_key = k;
    // BCRYPT_ECCPUBLIC_BLOB = header (magic, cbKey) || X || Y, big-endian.
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    ULONG size = 0;
    Check(BCryptExportKey(k, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &size, 0), "export");
    m_public.assign(1, 0x04);
    m_public.insert(m_public.end(), blob + sizeof(BCRYPT_ECCKEY_BLOB), blob + sizeof(BCRYPT_ECCKEY_BLOB) + 64);
}

EcdhKey::EcdhKey(const Bytes& publicKey, const Bytes& d)
{
    if (publicKey.size() != 65 || d.size() != 32) throw std::invalid_argument("key size");
    Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB));
    auto* h = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    h->dwMagic = BCRYPT_ECDH_PRIVATE_P256_MAGIC;
    h->cbKey = 32;
    blob.insert(blob.end(), publicKey.begin() + 1, publicKey.end());
    blob.insert(blob.end(), d.begin(), d.end());
    BCRYPT_KEY_HANDLE k = nullptr;
    Check(BCryptImportKeyPair(EcdhAlg().h, nullptr, BCRYPT_ECCPRIVATE_BLOB, &k, blob.data(), (ULONG)blob.size(), 0), "import");
    m_key = k;
    m_public = publicKey;
}

EcdhKey::~EcdhKey()
{
    if (m_key) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(m_key));
}

Bytes EcdhKey::Agree(const Bytes& peerPublic) const
{
    if (peerPublic.size() != 65 || peerPublic[0] != 0x04) throw std::invalid_argument("peer key");
    Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB));
    auto* h = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    h->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    h->cbKey = 32;
    blob.insert(blob.end(), peerPublic.begin() + 1, peerPublic.end());
    BCRYPT_KEY_HANDLE peer = nullptr;
    Check(BCryptImportKeyPair(EcdhAlg().h, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peer, blob.data(), (ULONG)blob.size(), 0), "import peer");

    BCRYPT_SECRET_HANDLE secret = nullptr;
    NTSTATUS st = BCryptSecretAgreement(static_cast<BCRYPT_KEY_HANDLE>(m_key), peer, &secret, 0);
    BCryptDestroyKey(peer);
    Check(st, "agreement");
    Bytes z(32);
    ULONG size = 0;
    st = BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, z.data(), 32, &size, 0);
    BCryptDestroySecret(secret);
    Check(st, "raw secret");
    // CNG returns the raw secret little-endian; the protocol uses big-endian X.
    std::reverse(z.begin(), z.end());
    return z;
}

// ---- protocol values -------------------------------------------------------------

Bytes Transcript(const std::string& clientId, const std::string& deviceId, const Bytes& pcPub, const Bytes& phonePub)
{
    const Bytes a = LP(Str("camstudio-v1")), b = LP(Str(clientId)), c = LP(Str(deviceId)), d = LP(pcPub), e = LP(phonePub);
    return Sha256({ &a, &b, &c, &d, &e });
}

std::string Sas(const Bytes& t)
{
    const Bytes label = Str("camstudio-sas");
    const Bytes h = Sha256({ &label, &t });
    const uint32_t v = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
    char buf[8];
    snprintf(buf, sizeof(buf), "%06u", v % 1000000u);
    return buf;
}

Bytes PairKey(const Bytes& z, const Bytes& t) { return Hkdf(z, t, Str("camstudio pair")); }
Bytes UpKey(const Bytes& z, const Bytes& kPair, const Bytes& t) { return Hkdf(Concat(z, kPair), t, Str("camstudio pc->phone")); }
Bytes DownKey(const Bytes& z, const Bytes& kPair, const Bytes& t) { return Hkdf(Concat(z, kPair), t, Str("camstudio phone->pc")); }
Bytes PcMac(const Bytes& kPair, const Bytes& t) { const Bytes l = Str("pc"); return HmacSha256(kPair, { &l, &t }); }
Bytes PhoneMac(const Bytes& kPair, const Bytes& t) { const Bytes l = Str("phone"); return HmacSha256(kPair, { &l, &t }); }

Bytes Seal(const Bytes& key, uint32_t direction, uint64_t counter, const uint8_t* plain, size_t size)
{
    Bytes out(size + 16);
    Gcm(true, key, direction, counter, plain, size, out.data(), out.data() + size);
    return out;
}

bool Open(const Bytes& key, uint32_t direction, uint64_t counter, const uint8_t* sealed, size_t size, Bytes& plain)
{
    if (size < 16) return false;
    plain.resize(size - 16);
    Bytes tag(sealed + size - 16, sealed + size);
    try
    {
        Gcm(false, key, direction, counter, sealed, size - 16, plain.data(), tag.data());
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

// ---- encodings --------------------------------------------------------------------

Bytes FromHex(const std::string& hex)
{
    Bytes out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    return out;
}

std::string ToHex(const Bytes& b)
{
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (uint8_t v : b) { s += digits[v >> 4]; s += digits[v & 15]; }
    return s;
}

std::string Base64(const Bytes& b)
{
    DWORD n = 0;
    CryptBinaryToStringA(b.data(), (DWORD)b.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
    std::string s(n, '\0');
    CryptBinaryToStringA(b.data(), (DWORD)b.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s.data(), &n);
    s.resize(n);
    return s;
}

Bytes FromBase64(const std::string& s)
{
    DWORD n = 0;
    if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr)) return {};
    Bytes b(n);
    CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, b.data(), &n, nullptr, nullptr);
    b.resize(n);
    return b;
}

bool ConstantTimeEqual(const Bytes& a, const Bytes& b)
{
    if (a.size() != b.size()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}
} // namespace crypto

// The primitives of docs/protocol.md section 3 on Windows CNG (BCrypt).
// Checked byte for byte against docs/protocol-vectors.json by
// camstudio-protocol-test.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace crypto
{
using Bytes = std::vector<uint8_t>;

Bytes Sha256(std::initializer_list<const Bytes*> parts);
Bytes HmacSha256(const Bytes& key, std::initializer_list<const Bytes*> parts);
Bytes Hkdf(const Bytes& ikm, const Bytes& salt, const Bytes& info, size_t length = 32); // RFC 5869
Bytes LP(const Bytes& data);                                                           // u32 BE length || data
Bytes Str(const std::string& s);                                                       // UTF-8 bytes

// Ephemeral or imported P-256 key pair.
class EcdhKey
{
public:
    EcdhKey();                                       // fresh random key
    EcdhKey(const Bytes& publicKey, const Bytes& d); // import (tests)
    ~EcdhKey();
    EcdhKey(const EcdhKey&) = delete;
    EcdhKey& operator=(const EcdhKey&) = delete;

    const Bytes& PublicKey() const { return m_public; } // 65 bytes 04||X||Y
    Bytes Agree(const Bytes& peerPublic) const;         // Z: X coordinate, 32 bytes big-endian

private:
    void* m_key = nullptr; // BCRYPT_KEY_HANDLE
    Bytes m_public;
};

Bytes Transcript(const std::string& clientId, const std::string& deviceId, const Bytes& pcPub, const Bytes& phonePub);
std::string Sas(const Bytes& t);
Bytes PairKey(const Bytes& z, const Bytes& t);
Bytes UpKey(const Bytes& z, const Bytes& kPair, const Bytes& t);
Bytes DownKey(const Bytes& z, const Bytes& kPair, const Bytes& t);
Bytes PcMac(const Bytes& kPair, const Bytes& t);
Bytes PhoneMac(const Bytes& kPair, const Bytes& t);

constexpr uint32_t kDirUp = 1;   // PC -> phone
constexpr uint32_t kDirDown = 2; // phone -> PC

// AES-256-GCM with the protocol's nonce (direction||counter) and AAD
// (0x02||counter). Seal returns ciphertext||tag; Open returns false if the
// tag does not verify.
Bytes Seal(const Bytes& key, uint32_t direction, uint64_t counter, const uint8_t* plain, size_t size);
bool Open(const Bytes& key, uint32_t direction, uint64_t counter, const uint8_t* sealed, size_t size, Bytes& plain);

Bytes FromHex(const std::string& hex);
std::string ToHex(const Bytes& b);
std::string Base64(const Bytes& b);
Bytes FromBase64(const std::string& s);
bool ConstantTimeEqual(const Bytes& a, const Bytes& b);
} // namespace crypto

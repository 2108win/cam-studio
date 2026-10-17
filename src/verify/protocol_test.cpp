// Reproduces docs/protocol-vectors.json with the PC's BCrypt implementation
// (protocol appendix A). Exit code 0 = every value matches.
#include "../engine/Crypto.h"

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

using crypto::Bytes;

static std::string g_json;
static int g_failures = 0;

static std::string V(const char* name)
{
    std::smatch m;
    std::regex re(std::string("\"") + name + "\"\\s*:\\s*\"([^\"]*)\"");
    if (!std::regex_search(g_json, m, re)) { printf("missing %s\n", name); exit(2); }
    return m[1];
}

static void Expect(const char* what, const std::string& actual, const std::string& expected)
{
    const bool ok = actual == expected;
    printf("%s  %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok)
    {
        printf("      got      %s\n      expected %s\n", actual.c_str(), expected.c_str());
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "docs/protocol-vectors.json";
    std::ifstream f(path);
    if (!f) { printf("cannot open %s\n", path); return 2; }
    std::stringstream ss;
    ss << f.rdbuf();
    g_json = ss.str();

    const Bytes pcPub = crypto::FromHex(V("pcPublic"));
    const Bytes phonePub = crypto::FromHex(V("phonePublic"));
    crypto::EcdhKey pc(pcPub, crypto::FromHex(V("pcPrivate")));

    Expect("commit", crypto::ToHex(crypto::Sha256({ &pcPub })), V("commit"));
    const Bytes z = pc.Agree(phonePub);
    Expect("Z (BCrypt ECDH, byte order)", crypto::ToHex(z), V("Z"));
    const Bytes t = crypto::Transcript(V("clientId"), V("deviceId"), pcPub, phonePub);
    Expect("T", crypto::ToHex(t), V("T"));
    Expect("SAS", crypto::Sas(t), V("SAS"));
    const Bytes kPair = crypto::PairKey(z, t);
    Expect("K_pair", crypto::ToHex(kPair), V("K_pair"));
    Expect("macPc", crypto::ToHex(crypto::PcMac(kPair, t)), V("macPc"));
    Expect("macPhone", crypto::ToHex(crypto::PhoneMac(kPair, t)), V("macPhone"));
    const Bytes up = crypto::UpKey(z, kPair, t);
    Expect("K_up", crypto::ToHex(up), V("K_up"));
    Expect("K_down", crypto::ToHex(crypto::DownKey(z, kPair, t)), V("K_down"));

    const Bytes plain = crypto::FromHex(V("sealedPlaintext"));
    const Bytes sealed = crypto::Seal(up, crypto::kDirUp, 0, plain.data(), plain.size());
    Bytes record{ 0, 0, 0, uint8_t(1 + 8 + sealed.size()), 0x02, 0, 0, 0, 0, 0, 0, 0, 0 };
    record.insert(record.end(), sealed.begin(), sealed.end());
    Expect("sealed record (BCrypt AES-GCM)", crypto::ToHex(record), V("sealedRecord"));

    Bytes opened;
    Expect("open sealed record", crypto::Open(up, crypto::kDirUp, 0, sealed.data(), sealed.size(), opened) ? crypto::ToHex(opened) : "tag mismatch",
           V("sealedPlaintext"));
    Bytes tampered = sealed;
    tampered[3] ^= 1;
    Expect("reject tampered record", crypto::Open(up, crypto::kDirUp, 0, tampered.data(), tampered.size(), opened) ? "accepted" : "rejected", "rejected");

    // A fresh key pair agrees with itself through the protocol encoding.
    crypto::EcdhKey a, b;
    Expect("fresh ECDH round trip", crypto::ToHex(a.Agree(b.PublicKey())), crypto::ToHex(b.Agree(a.PublicKey())));

    printf(g_failures ? "\n%d FAILED\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}

package vn.camstudio.phone

import java.math.BigInteger
import java.nio.ByteBuffer
import java.security.AlgorithmParameters
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * The exact primitives of docs/protocol.md section 3. Byte layouts matter:
 * the PC (BCrypt) and iPhone (CryptoKit) must derive identical values, which
 * CryptoTest checks against docs/protocol-vectors.json.
 */
object Crypto {
    private val p256: ECParameterSpec by lazy {
        AlgorithmParameters.getInstance("EC").run {
            init(ECGenParameterSpec("secp256r1"))
            getParameterSpec(ECParameterSpec::class.java)
        }
    }

    fun newKeyPair(): KeyPair = KeyPairGenerator.getInstance("EC").run {
        initialize(ECGenParameterSpec("secp256r1"))
        generateKeyPair()
    }

    /** 65 bytes: 04 || X || Y, each coordinate exactly 32 bytes big-endian. */
    fun encodePublic(key: ECPublicKey): ByteArray =
        byteArrayOf(4) + fixed32(key.w.affineX) + fixed32(key.w.affineY)

    fun decodePublic(bytes: ByteArray): ECPublicKey {
        require(bytes.size == 65 && bytes[0] == 4.toByte()) { "bad public key" }
        val point = ECPoint(BigInteger(1, bytes.copyOfRange(1, 33)), BigInteger(1, bytes.copyOfRange(33, 65)))
        return KeyFactory.getInstance("EC").generatePublic(ECPublicKeySpec(point, p256)) as ECPublicKey
    }

    /** Z = X coordinate of the shared point, 32 bytes big-endian. */
    fun ecdh(own: java.security.PrivateKey, peer: ECPublicKey): ByteArray =
        KeyAgreement.getInstance("ECDH").run {
            init(own)
            doPhase(peer, true)
            fixed32(BigInteger(1, generateSecret()))
        }

    fun sha256(vararg parts: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").run {
        parts.forEach { update(it) }
        digest()
    }

    fun hmac(key: ByteArray, vararg parts: ByteArray): ByteArray = Mac.getInstance("HmacSHA256").run {
        init(SecretKeySpec(key, "HmacSHA256"))
        parts.forEach { update(it) }
        doFinal()
    }

    /** RFC 5869 HKDF-SHA256 (Android has no platform HKDF). */
    fun hkdf(ikm: ByteArray, salt: ByteArray, info: ByteArray, length: Int = 32): ByteArray {
        val prk = hmac(salt, ikm)
        val out = ByteArray(length)
        var t = ByteArray(0)
        var pos = 0
        var i = 1
        while (pos < length) {
            t = hmac(prk, t, info, byteArrayOf(i.toByte()))
            val n = minOf(t.size, length - pos)
            System.arraycopy(t, 0, out, pos, n)
            pos += n
            i++
        }
        return out
    }

    /** LP(x) = u32 big-endian length || x */
    fun lp(bytes: ByteArray): ByteArray = ByteBuffer.allocate(4 + bytes.size).putInt(bytes.size).put(bytes).array()

    fun transcript(clientId: String, deviceId: String, pcPub: ByteArray, phonePub: ByteArray): ByteArray =
        sha256(
            lp("camstudio-v1".toByteArray()), lp(clientId.toByteArray()), lp(deviceId.toByteArray()),
            lp(pcPub), lp(phonePub),
        )

    fun sas(t: ByteArray): String {
        val h = sha256("camstudio-sas".toByteArray(), t)
        val v = ByteBuffer.wrap(h, 0, 4).int.toLong() and 0xFFFFFFFFL
        return "%06d".format(v % 1_000_000)
    }

    fun pairKey(z: ByteArray, t: ByteArray) = hkdf(z, t, "camstudio pair".toByteArray())
    fun upKey(z: ByteArray, kPair: ByteArray, t: ByteArray) = hkdf(z + kPair, t, "camstudio pc->phone".toByteArray())
    fun downKey(z: ByteArray, kPair: ByteArray, t: ByteArray) = hkdf(z + kPair, t, "camstudio phone->pc".toByteArray())
    fun pcMac(kPair: ByteArray, t: ByteArray) = hmac(kPair, "pc".toByteArray(), t)
    fun phoneMac(kPair: ByteArray, t: ByteArray) = hmac(kPair, "phone".toByteArray(), t)

    const val DIR_UP = 1   // PC -> phone
    const val DIR_DOWN = 2 // phone -> PC

    /** Seals one record body: returns ciphertext||tag. */
    fun seal(key: ByteArray, direction: Int, counter: Long, plaintext: ByteArray, offset: Int = 0, length: Int = plaintext.size): ByteArray =
        gcm(Cipher.ENCRYPT_MODE, key, direction, counter).doFinal(plaintext, offset, length)

    fun open(key: ByteArray, direction: Int, counter: Long, sealed: ByteArray, offset: Int, length: Int): ByteArray =
        gcm(Cipher.DECRYPT_MODE, key, direction, counter).doFinal(sealed, offset, length)

    private fun gcm(mode: Int, key: ByteArray, direction: Int, counter: Long): Cipher {
        val nonce = ByteBuffer.allocate(12).putInt(direction).putLong(counter).array()
        val aad = ByteBuffer.allocate(9).put(0x02).putLong(counter).array()
        return Cipher.getInstance("AES/GCM/NoPadding").apply {
            init(mode, SecretKeySpec(key, "AES"), GCMParameterSpec(128, nonce))
            updateAAD(aad)
        }
    }

    private fun fixed32(v: BigInteger): ByteArray {
        val raw = v.toByteArray()
        return when {
            raw.size == 32 -> raw
            raw.size > 32 -> raw.copyOfRange(raw.size - 32, raw.size) // drop sign byte
            else -> ByteArray(32 - raw.size) + raw
        }
    }
}

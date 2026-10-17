package vn.camstudio.phone

import java.io.File
import java.math.BigInteger
import java.nio.ByteBuffer
import java.security.AlgorithmParameters
import java.security.KeyFactory
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPrivateKeySpec
import org.junit.Assert.assertEquals
import org.junit.Test

/** Reproduces docs/protocol-vectors.json byte for byte (protocol appendix A). */
class CryptoTest {
    private val json = File("../../docs/protocol-vectors.json").readText()
    private fun v(name: String) = Regex("\"$name\"\\s*:\\s*\"([^\"]*)\"").find(json)!!.groupValues[1]
    private fun hex(s: String) = ByteArray(s.length / 2) { s.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
    private fun ByteArray.hex() = joinToString("") { "%02x".format(it) }

    @Test
    fun handshakeMatchesVectors() {
        val spec = AlgorithmParameters.getInstance("EC").run {
            init(ECGenParameterSpec("secp256r1")); getParameterSpec(ECParameterSpec::class.java)
        }
        val phonePriv = KeyFactory.getInstance("EC").generatePrivate(ECPrivateKeySpec(BigInteger(v("phonePrivate"), 16), spec))
        val pcPub = hex(v("pcPublic"))
        val phonePub = hex(v("phonePublic"))

        // Our encoding round-trips the reference public key exactly.
        assertEquals(v("phonePublic"), Crypto.encodePublic(Crypto.decodePublic(phonePub)).hex())
        assertEquals(v("commit"), Crypto.sha256(pcPub).hex())

        val z = Crypto.ecdh(phonePriv, Crypto.decodePublic(pcPub))
        assertEquals(v("Z"), z.hex())
        val t = Crypto.transcript(v("clientId"), v("deviceId"), pcPub, phonePub)
        assertEquals(v("T"), t.hex())
        assertEquals(v("SAS"), Crypto.sas(t))
        val kPair = Crypto.pairKey(z, t)
        assertEquals(v("K_pair"), kPair.hex())
        assertEquals(v("macPc"), Crypto.pcMac(kPair, t).hex())
        assertEquals(v("macPhone"), Crypto.phoneMac(kPair, t).hex())
        val up = Crypto.upKey(z, kPair, t)
        assertEquals(v("K_up"), up.hex())
        assertEquals(v("K_down"), Crypto.downKey(z, kPair, t).hex())

        // The phone opens the reference record the PC sealed.
        val record = hex(v("sealedRecord"))
        val counter = ByteBuffer.wrap(record, 5, 8).long
        val plain = Crypto.open(up, Crypto.DIR_UP, counter, record, 13, record.size - 13)
        assertEquals(v("sealedPlaintext"), plain.hex())
        // And sealing it again gives the identical bytes.
        assertEquals(record.copyOfRange(13, record.size).hex(), Crypto.seal(up, Crypto.DIR_UP, 0, plain).hex())
    }
}

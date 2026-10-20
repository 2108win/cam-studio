package vn.camstudio.phone

import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.EOFException
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import org.json.JSONObject

/** Record framing of docs/protocol.md section 2. */
object Wire {
    const val HANDSHAKE = 0x01
    const val SEALED = 0x02

    const val KIND_CONTROL = 1
    const val KIND_VIDEO_CONFIG = 2
    const val KIND_VIDEO_FRAME = 3
    const val KIND_AUDIO = 4 // docs/protocol.md 5.1: the microphone, only when the PC asked

    const val FLAG_KEYFRAME = 1

    const val MAX_HANDSHAKE = 64 * 1024
    const val MAX_SEALED = 16 * 1024 * 1024
}

class ProtocolException(message: String) : IOException(message)

/** Plain JSON records, used only during the handshake. */
class HandshakeIo(input: InputStream, output: OutputStream) {
    private val inp = DataInputStream(input)
    private val out = DataOutputStream(output)

    fun read(): JSONObject {
        val length = inp.readInt()
        if (length < 1 || length > Wire.MAX_HANDSHAKE) throw ProtocolException("handshake record too large")
        val type = inp.readUnsignedByte()
        val body = ByteArray(length - 1).also { inp.readFully(it) }
        if (type != Wire.HANDSHAKE) throw ProtocolException("expected handshake record")
        return JSONObject(String(body, Charsets.UTF_8))
    }

    fun write(json: JSONObject) {
        val body = json.toString().toByteArray(Charsets.UTF_8)
        synchronized(out) {
            out.writeInt(body.size + 1)
            out.writeByte(Wire.HANDSHAKE)
            out.write(body)
            out.flush()
        }
    }
}

/** AES-GCM sealed records with strict per-direction counters. */
class SecureChannel(
    input: InputStream,
    output: OutputStream,
    private val sendKey: ByteArray,
    private val receiveKey: ByteArray,
) {
    class Message(val kind: Int, val ptsUs: Long, val flags: Int, val payload: ByteArray, val offset: Int, val length: Int) {
        fun json() = JSONObject(String(payload, offset, length, Charsets.UTF_8))
    }

    private val inp = DataInputStream(input)
    private val out = DataOutputStream(output.buffered(256 * 1024))
    private var sendCounter = 0L
    private var receiveCounter = 0L
    private var scratch = ByteArray(64 * 1024)

    fun send(kind: Int, payload: ByteArray, offset: Int = 0, length: Int = payload.size, ptsUs: Long = 0, flags: Int = 0) {
        synchronized(out) {
            val needed = 13 + length
            if (scratch.size < needed) scratch = ByteArray(needed * 2)
            val head = ByteBuffer.wrap(scratch, 0, 13)
            head.put(kind.toByte()).putLong(ptsUs).putInt(flags)
            System.arraycopy(payload, offset, scratch, 13, length)
            val counter = sendCounter++
            val sealed = Crypto.seal(sendKey, Crypto.DIR_DOWN, counter, scratch, 0, needed)
            out.writeInt(1 + 8 + sealed.size)
            out.writeByte(Wire.SEALED)
            out.writeLong(counter)
            out.write(sealed)
            out.flush()
        }
    }

    fun sendJson(json: JSONObject) = send(Wire.KIND_CONTROL, json.toString().toByteArray(Charsets.UTF_8))

    fun receive(): Message {
        val length = try { inp.readInt() } catch (e: EOFException) { throw e }
        if (length < 1 + 8 + 16 || length > Wire.MAX_SEALED) throw ProtocolException("bad record length")
        val type = inp.readUnsignedByte()
        if (type != Wire.SEALED) throw ProtocolException("expected sealed record")
        val counter = inp.readLong()
        if (counter != receiveCounter) throw ProtocolException("record counter out of order")
        receiveCounter++
        val sealed = ByteArray(length - 9).also { inp.readFully(it) }
        val plain = Crypto.open(receiveKey, Crypto.DIR_UP, counter, sealed, 0, sealed.size)
        val head = ByteBuffer.wrap(plain, 0, 13)
        return Message(head.get().toInt() and 0xFF, head.long, head.int, plain, 13, plain.size - 13)
    }
}

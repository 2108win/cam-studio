package vn.camstudio.phone

import android.content.Context
import android.os.Build
import android.provider.Settings
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * This phone's identity and the PCs it is paired with. Pairing keys are
 * encrypted with a hardware-backed Android Keystore key, so a copy of the
 * app's data alone cannot impersonate a paired PC.
 */
class Store(context: Context) {
    private val prefs = context.getSharedPreferences("camstudio", Context.MODE_PRIVATE)
    private val appContext = context.applicationContext

    /** Generated once; written exactly like this on the wire forever after. */
    val deviceId: String = prefs.getString("deviceId", null) ?: UUID.randomUUID().toString().also {
        prefs.edit().putString("deviceId", it).apply()
    }

    val deviceName: String
        get() = Settings.Global.getString(appContext.contentResolver, Settings.Global.DEVICE_NAME)
            ?.takeIf { it.isNotBlank() } ?: Build.MODEL

    /** "Chạy nền": the camera keeps serving the PC while the app is hidden (CameraService). On unless turned off. */
    var cameraInBackground: Boolean
        get() = prefs.getBoolean("cameraInBackground", true)
        set(value) = prefs.edit().putBoolean("cameraInBackground", value).apply()

    fun pairKey(clientId: String): ByteArray? {
        val stored = prefs.getString("pair.$clientId", null) ?: return null
        return runCatching {
            val raw = Base64.decode(stored, Base64.NO_WRAP)
            Cipher.getInstance("AES/GCM/NoPadding").run {
                init(Cipher.DECRYPT_MODE, wrappingKey(), GCMParameterSpec(128, raw, 0, 12))
                doFinal(raw, 12, raw.size - 12)
            }
        }.getOrNull()
    }

    fun savePairKey(clientId: String, clientName: String, key: ByteArray) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, wrappingKey()) }
        val sealed = cipher.iv + cipher.doFinal(key)
        prefs.edit()
            .putString("pair.$clientId", Base64.encodeToString(sealed, Base64.NO_WRAP))
            .putString("name.$clientId", clientName)
            .apply()
    }

    fun forget(clientId: String) {
        prefs.edit().remove("pair.$clientId").remove("name.$clientId").apply()
    }

    private fun wrappingKey(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
            init(
                KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setKeySize(256)
                    .build()
            )
            generateKey()
        }
    }

    private companion object {
        const val ALIAS = "camstudio-pairing"
    }
}

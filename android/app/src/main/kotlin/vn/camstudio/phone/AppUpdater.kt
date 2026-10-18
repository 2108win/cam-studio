package vn.camstudio.phone

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.Build
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * Updates from the GitHub releases (docs/updates.md), same feed as the PC: the newest published
 * release with a CamStudio-<version>-android.apk whose size and SHA-256 (GitHub's "digest") match.
 * Android always asks the person to confirm the install (the first time also to allow installing
 * from Cam Studio); later updates may go through without asking on Android 12+. The new APK must be
 * signed like the installed one, or Android refuses it.
 */
object AppUpdater {
    data class Release(val version: String, val apkName: String, val size: Long, val sha256: String, val url: String)

    private const val FEED = "https://api.github.com/repos/2108win/cam-studio/releases/latest"
    private const val ACTION_STATUS = "vn.camstudio.phone.UPDATE_STATUS"

    /** Install results that need the person's attention (failure text), on any thread. */
    @Volatile var onStatus: ((String) -> Unit)? = null

    fun currentVersion(context: Context): String =
        context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "0"

    /** a > b, comparing dotted numbers ("0.1.10" > "0.1.9"). */
    fun isNewer(a: String, b: String): Boolean {
        val x = a.split('.').map { it.toIntOrNull() ?: 0 }
        val y = b.split('.').map { it.toIntOrNull() ?: 0 }
        for (i in 0 until maxOf(x.size, y.size)) {
            val d = x.getOrElse(i) { 0 } - y.getOrElse(i) { 0 }
            if (d != 0) return d > 0
        }
        return false
    }

    /** Network: the newer release for this phone, or null. Throws when the feed cannot be read. */
    fun fetchNewer(current: String): Release? {
        val connection = (URL(FEED).openConnection() as HttpURLConnection).apply {
            connectTimeout = 15000
            readTimeout = 15000
            setRequestProperty("Accept", "application/vnd.github+json")
            setRequestProperty("User-Agent", "CamStudio-Android/$current")
        }
        try {
            if (connection.responseCode != 200) throw IllegalStateException("HTTP ${connection.responseCode}")
            val root = JSONObject(connection.inputStream.bufferedReader().use { it.readText() })
            if (root.optBoolean("draft") || root.optBoolean("prerelease")) return null
            val tag = root.getString("tag_name").trimStart('v', 'V')
            if (tag.contains('-') || !isNewer(tag, current)) return null
            val wanted = "CamStudio-$tag-android.apk"
            val assets = root.getJSONArray("assets")
            for (i in 0 until assets.length()) {
                val a = assets.getJSONObject(i)
                if (a.getString("name") != wanted) continue
                val digest = a.optString("digest")
                val url = a.getString("browser_download_url")
                if (!digest.startsWith("sha256:") || digest.length != 7 + 64 || !url.startsWith("https://")) return null
                return Release(tag, wanted, a.getLong("size"), digest.substring(7).lowercase(), url)
            }
            return null
        } finally {
            connection.disconnect()
        }
    }

    /** Network: the APK in the app's cache, checked; throws (and deletes it) when it does not match. */
    fun download(context: Context, release: Release, progress: (Int) -> Unit): File {
        val dir = File(context.cacheDir, "updates").apply { mkdirs() }
        dir.listFiles()?.filter { it.name != release.apkName }?.forEach { it.delete() }
        val file = File(dir, release.apkName)
        if (file.length() == release.size && sha256(file) == release.sha256) return file
        val connection = (URL(release.url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 15000
            readTimeout = 30000
            instanceFollowRedirects = true
        }
        try {
            if (connection.responseCode != 200) throw IllegalStateException("HTTP ${connection.responseCode}")
            val digest = MessageDigest.getInstance("SHA-256")
            var done = 0L
            connection.inputStream.use { input ->
                file.outputStream().use { output ->
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        val n = input.read(buffer)
                        if (n < 0) break
                        done += n
                        if (done > release.size) break
                        digest.update(buffer, 0, n)
                        output.write(buffer, 0, n)
                        progress((done * 100 / release.size).toInt())
                    }
                }
            }
            val hash = digest.digest().joinToString("") { "%02x".format(it) }
            if (done != release.size || hash != release.sha256) {
                file.delete()
                throw IllegalStateException("Bản tải về bị hỏng (sai mã SHA-256), đã bỏ")
            }
            return file
        } finally {
            connection.disconnect()
        }
    }

    /** Hands the APK to Android's installer; the result comes back to [StatusReceiver]. */
    fun install(context: Context, apk: File) {
        val installer = context.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(context.packageName)
            setSize(apk.length())
            // Once Cam Studio installed itself, Android 12+ may update it without asking again.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
        }
        val id = installer.createSession(params)
        installer.openSession(id).use { session ->
            session.openWrite("base.apk", 0, apk.length()).use { out ->
                apk.inputStream().use { it.copyTo(out) }
                session.fsync(out)
            }
            val intent = Intent(context, StatusReceiver::class.java).setAction(ACTION_STATUS)
            // Mutable: the installer adds the status to it.
            val pending = PendingIntent.getBroadcast(context, id, intent, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE)
            session.commit(pending.intentSender)
        }
    }

    private fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(64 * 1024)
            while (true) {
                val n = input.read(buffer)
                if (n < 0) break
                digest.update(buffer, 0, n)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    /** Android's answer to a session: show its confirmation screen, or report why it failed. */
    class StatusReceiver : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
                PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                    @Suppress("DEPRECATION")
                    val confirm = intent.getParcelableExtra<Intent>(Intent.EXTRA_INTENT) ?: return
                    context.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                }
                PackageInstaller.STATUS_SUCCESS -> Unit // the app restarts as the new version
                else -> {
                    val message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: ""
                    onStatus?.invoke(
                        if (status == PackageInstaller.STATUS_FAILURE_ABORTED) "Đã hủy cập nhật"
                        else if (status == PackageInstaller.STATUS_FAILURE_CONFLICT || status == PackageInstaller.STATUS_FAILURE_INCOMPATIBLE)
                            "Không cài được: bản mới ký khác bản đang cài ($message)"
                        else "Không cài được bản mới ($message)")
                }
            }
        }
    }
}

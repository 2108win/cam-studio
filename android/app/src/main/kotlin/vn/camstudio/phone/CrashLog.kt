package vn.camstudio.phone

import android.content.Context
import android.os.Build
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter

/**
 * Keeps the last crash in the app's files so the next launch can show it: without a cable
 * and adb, that is the only way the person can tell us what went wrong.
 */
object CrashLog {
    private const val FILE = "last-crash.txt"
    @Volatile private var installed = false

    fun install(context: Context) {
        if (installed) return
        installed = true
        val file = File(context.applicationContext.filesDir, FILE)
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            runCatching {
                val trace = StringWriter().also { error.printStackTrace(PrintWriter(it)) }.toString()
                file.writeText("${Build.MANUFACTURER} ${Build.MODEL}, Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})\n" +
                    "thread ${thread.name}\n$trace")
            }
            previous?.uncaughtException(thread, error)
        }
    }

    /** The last crash (first lines), once; null if there was none. */
    fun takeLast(context: Context): String? {
        val file = File(context.filesDir, FILE)
        if (!file.exists()) return null
        val text = runCatching { file.readLines().take(14).joinToString("\n") }.getOrNull()
        file.delete()
        return text
    }
}

package vn.camstudio.phone

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.IBinder

/**
 * Screen sharing (docs/protocol.md 1.1). Android hands out the screen only to
 * a foreground service of type mediaProjection, after the person agreed in the
 * system dialog (MainActivity asks). The service keeps one MediaProjection for
 * the whole sharing session and serves it to the PC as a second source on its
 * own port; it keeps running while other apps are in front. It ends from its
 * notification, from the app, or when Android stops the projection.
 */
class ScreenService : Service(), Session.Ui {
    private var capture: ScreenCapture? = null
    private var source: ScreenSource? = null
    private var server: PhoneServer? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        CrashLog.install(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSharing()
            return START_NOT_STICKY
        }
        // Foreground first: Android 14 refuses the projection to a service that is not yet.
        startForegroundCompat(notification("Đang chuẩn bị chia sẻ màn hình…"))
        val code = intent?.getIntExtra(EXTRA_CODE, 0) ?: 0
        val data: Intent? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent?.getParcelableExtra(EXTRA_DATA, Intent::class.java)
        } else {
            @Suppress("DEPRECATION") intent?.getParcelableExtra(EXTRA_DATA)
        }
        if (capture != null || data == null) return START_NOT_STICKY // already sharing, or nothing to share
        try {
            val projection = getSystemService(MediaProjectionManager::class.java).getMediaProjection(code, data)
                ?: throw IllegalStateException("Android không cấp quyền ghi màn hình")
            val c = ScreenCapture(this, projection) { message -> stopSharing(message) }
            capture = c
            val s = ScreenSource(c)
            source = s
            server = PhoneServer(this, this, s, PhoneServer.Kind.SCREEN).also { it.start() }
            running = true
            status(false, "Đang chờ máy tính chọn \"${server?.txtName()}\" trong danh sách camera")
        } catch (e: Exception) {
            stopSharing("Không chia sẻ được màn hình: ${e.message ?: e.javaClass.simpleName}")
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        stopSharing()
        super.onDestroy()
    }

    /** `message`: what the app shows afterwards ("" = nothing, sharing simply ended). */
    private fun stopSharing(message: String = "") {
        if (capture == null && server == null && !running) {
            stopSelf()
            return
        }
        running = false
        server?.stop()
        server = null
        source?.release()
        source = null
        capture?.release()
        capture = null
        listener?.invoke(message)
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    // ---- Session.Ui (the screen never pairs and works in the background) ----

    override val inForeground: Boolean get() = true
    override val deviceRotation: Int get() = 0
    override fun confirmPairing(pcName: String, code: String): Boolean = false

    override fun status(streaming: Boolean, text: String) {
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification(text))
        listener?.invoke(text)
    }

    // ---- notification ----

    private fun notification(text: String): Notification {
        val manager = getSystemService(NotificationManager::class.java)
        if (manager.getNotificationChannel(CHANNEL) == null) {
            manager.createNotificationChannel(NotificationChannel(CHANNEL, "Chia sẻ màn hình", NotificationManager.IMPORTANCE_LOW))
        }
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, 1, Intent(this, ScreenService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle("Cam Studio đang chia sẻ màn hình")
            .setContentText(text)
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, "Dừng", stop).build())
            .build()
    }

    private fun startForegroundCompat(n: Notification) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        } else {
            startForeground(NOTIFICATION_ID, n)
        }
    }

    companion object {
        private const val CHANNEL = "screen"
        private const val NOTIFICATION_ID = 1
        private const val EXTRA_CODE = "code"
        private const val EXTRA_DATA = "data"
        private const val ACTION_STOP = "vn.camstudio.phone.STOP_SCREEN"

        /** True while the screen is being shared. */
        @Volatile var running = false
            private set

        /** The activity shows the sharing status ("" when sharing ended). */
        @Volatile var listener: ((String) -> Unit)? = null

        /** After the person agreed in the system dialog (`resultCode`, `data` from MediaProjectionManager). */
        fun start(context: Context, resultCode: Int, data: Intent) {
            val intent = Intent(context, ScreenService::class.java).putExtra(EXTRA_CODE, resultCode).putExtra(EXTRA_DATA, data)
            context.startForegroundService(intent)
        }

        fun stop(context: Context) {
            context.startService(Intent(context, ScreenService::class.java).setAction(ACTION_STOP))
        }
    }
}

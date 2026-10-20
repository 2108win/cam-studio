package vn.camstudio.phone

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.view.OrientationEventListener

/**
 * The phone's cameras as a PC source, also while the app is hidden: a foreground
 * service of type camera. Android lets it keep using the camera only when it was
 * started while the app was on screen, so MainActivity starts it every time it
 * shows. It owns the camera PhoneServer, the CameraSource and the orientation
 * listener. Pairing still needs the app on screen (someone has to compare the
 * code). It ends from its notification, or when the app leaves with "Chạy nền" off.
 */
class CameraService : Service(), Session.Ui {
    private var server: PhoneServer? = null
    private var orientation: OrientationEventListener? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    @Volatile private var rotation = 0

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        CrashLog.install(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            shutDown(STOPPED_TEXT)
            return START_NOT_STICKY
        }
        // Every startForegroundService() needs this, even when the service already runs.
        try {
            startForegroundCompat(if (running) notification(lastStreaming, lastStatus) else notification(false, STARTING_TEXT))
        } catch (e: Exception) {
            // Android refused: the app had already left the screen, or the camera permission is missing.
            shutDown("Không bật được camera: ${e.message ?: e.javaClass.simpleName}")
            return START_NOT_STICKY
        }
        if (server != null) return START_NOT_STICKY // already serving

        running = true
        val camera = CameraSource(this, this)
        Thread({ runCatching { camera.prepare() } }, "lenses").start()
        val srv = PhoneServer(this, this, camera)
        try {
            srv.start()
        } catch (e: Exception) {
            shutDown("Không mở được cổng cho máy tính: ${e.message ?: e.javaClass.simpleName}")
            return START_NOT_STICKY
        }
        server = srv
        port = srv.port
        orientation = object : OrientationEventListener(this) {
            override fun onOrientationChanged(degrees: Int) {
                if (degrees == ORIENTATION_UNKNOWN) return
                val snapped = ((degrees + 45) / 90 * 90) % 360
                if (snapped != rotation) {
                    rotation = snapped
                    server?.session?.onRotationChanged()
                }
            }
        }.also { it.enable() }
        status(false, "Đang chờ máy tính kết nối…")
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        if (running) shutDown(STOPPED_TEXT) // else already shut down, with its own message
        super.onDestroy()
    }

    /** `message`: the status shown afterwards. */
    private fun shutDown(message: String) {
        running = false
        port = 0
        orientation?.disable()
        orientation = null
        server?.stop() // closes the session, which stops the camera
        server = null
        keepAwake(false)
        lastStreaming = false
        lastStatus = message
        listener?.invoke(false, message)
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    // ---- Session.Ui ----------------------------------------------------------------

    /** Android lets a camera foreground service started from the screen keep the camera. */
    override val inForeground: Boolean get() = true
    override val canAskPairing: Boolean get() = pairing != null
    override val deviceRotation: Int get() = rotation

    override fun confirmPairing(pcName: String, code: String): Boolean = pairing?.invoke(pcName, code) ?: false

    override fun status(streaming: Boolean, text: String) {
        if (!running) return // a session that ended with the service
        keepAwake(streaming)
        lastStreaming = streaming
        lastStatus = text
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification(streaming, text))
        listener?.invoke(streaming, text)
    }

    /**
     * While streaming, the stream must go on with the screen off: the CPU stays awake
     * and Wi‑Fi stays out of power saving.
     */
    @Suppress("DEPRECATION") // WIFI_MODE_FULL_LOW_LATENCY works only with the screen on
    @Synchronized private fun keepAwake(on: Boolean) {
        if (on && wakeLock == null) {
            wakeLock = getSystemService(PowerManager::class.java)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "CamStudio:camera")
                .apply { setReferenceCounted(false); acquire() }
            wifiLock = applicationContext.getSystemService(WifiManager::class.java)
                .createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "CamStudio:camera")
                .apply { setReferenceCounted(false); acquire() }
        } else if (!on) {
            wakeLock?.let { if (it.isHeld) it.release() }
            wakeLock = null
            wifiLock?.let { if (it.isHeld) it.release() }
            wifiLock = null
        }
    }

    // ---- notification ----------------------------------------------------------------

    private fun notification(streaming: Boolean, text: String): Notification {
        val manager = getSystemService(NotificationManager::class.java)
        if (manager.getNotificationChannel(CHANNEL) == null) {
            manager.createNotificationChannel(NotificationChannel(CHANNEL, "Camera chạy nền", NotificationManager.IMPORTANCE_LOW))
        }
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, 2, Intent(this, CameraService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(if (streaming) "Cam Studio đang phát camera" else "Cam Studio sẵn sàng làm camera")
            .setContentText(text)
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, "Dừng", stop).build())
            .build()
    }

    private fun startForegroundCompat(n: Notification) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // The microphone too once it is allowed: the PC may ask for the phone's sound, also
            // with the screen off. Android checks the permission here; without it, camera alone.
            if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
                try {
                    startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
                    return
                } catch (e: Exception) {
                    // refused for the microphone: the camera still works
                }
            }
            startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
        } else {
            startForeground(NOTIFICATION_ID, n) // before Android 11 any foreground service may use the camera
        }
    }

    companion object {
        private const val CHANNEL = "camera"
        private const val NOTIFICATION_ID = 2 // ScreenService uses 1
        private const val ACTION_STOP = "vn.camstudio.phone.STOP_CAMERA"
        private const val STOPPED_TEXT = "Camera đã tắt: máy tính không thấy điện thoại này."
        private const val STARTING_TEXT = "Đang bật camera…"

        /** True while the camera is served to the PC. */
        @Volatile var running = false
            private set

        /** The camera server's port (the fixed one unless it was taken), 0 when stopped. */
        @Volatile var port = 0
            private set

        /** The last status, so a new activity shows it (and dims while streaming). */
        @Volatile var lastStatus = STARTING_TEXT
            private set
        @Volatile var lastStreaming = false
            private set

        /** The activity shows the status. Called from any thread. */
        @Volatile var listener: ((streaming: Boolean, text: String) -> Unit)? = null

        /** Set by the activity while it is on screen: asks the person to compare the code (blocks). */
        @Volatile var pairing: ((pcName: String, code: String) -> Boolean)? = null

        /**
         * From the activity while it is on screen (Android forbids starting a camera service
         * from the background). False when Android refused anyway.
         */
        fun start(context: Context): Boolean = try {
            context.startForegroundService(Intent(context, CameraService::class.java))
            true
        } catch (e: Exception) {
            false
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, CameraService::class.java))
        }
    }
}

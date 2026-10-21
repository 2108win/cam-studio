package vn.camstudio.phone

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.media.projection.MediaProjectionManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.math.roundToInt

/**
 * The phone side of Cam Studio, in two parts. Camera: status, the address for
 * manual pairing, the pairing code, and "Chạy nền" (CameraService keeps serving
 * the PC while the app is hidden; off, the camera stops when the app leaves).
 * Màn hình: screen sharing (ScreenService). The PC picks lens, size and
 * controls. While the camera streams, the screen dims itself.
 */
class MainActivity : Activity() {
    private lateinit var store: Store
    private lateinit var cameraStatus: TextView
    private lateinit var address: TextView
    private lateinit var cameraButton: Button
    private lateinit var backgroundHint: TextView
    private lateinit var screenButton: Button
    private lateinit var screenStatus: TextView
    private lateinit var updateCard: LinearLayout
    private lateinit var updateText: TextView
    private lateinit var updateButton: Button
    /** A newer version found on GitHub (AppUpdater), not installed yet. */
    private var newer: AppUpdater.Release? = null
    private var updating = false
    private val pairingDialogs = mutableListOf<AlertDialog>()
    private var visible = false
    /** The microphone was asked for since the app came on screen: not again until it leaves. */
    private var micAsked = false
    /** CameraService was asked to start and has not reported yet. */
    private var cameraStarting = false
    private val cameraListener: (Boolean, String) -> Unit = { streaming, text ->
        runOnUiThread {
            cameraStarting = false
            showCameraState(streaming, text)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        CrashLog.install(this)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        store = Store(this)

        val title = TextView(this).apply {
            text = "Cam Studio"
            textSize = 26f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
            setPadding(0, 0, 0, dp(8))
        }

        // ---- Camera ----
        cameraStatus = TextView(this).apply {
            textSize = 16f
            setTextColor(Color.rgb(0xDD, 0xDD, 0xDD))
            gravity = Gravity.CENTER
        }
        address = TextView(this).apply {
            textSize = 14f
            setTextColor(Color.rgb(0x9A, 0xC8, 0xFF))
            gravity = Gravity.CENTER
            setPadding(0, dp(12), 0, 0)
            setTextIsSelectable(true)
        }
        cameraButton = Button(this).apply {
            text = "Bật camera"
            visibility = View.GONE
            // Also the way to stop when Android hides the notification (no permission for it).
            setOnClickListener { if (CameraService.running) CameraService.stop(this@MainActivity) else startCamera(askPermission = true) }
        }
        val backgroundSwitch = Switch(this).apply {
            text = "Chạy nền"
            textSize = 16f
            setTextColor(Color.WHITE)
            isChecked = store.cameraInBackground
            setOnCheckedChangeListener { _, checked ->
                store.cameraInBackground = checked
                showBackgroundHint()
            }
        }
        backgroundHint = TextView(this).apply {
            textSize = 13f
            setTextColor(Color.rgb(0x88, 0x88, 0x88))
        }
        val cameraHint = hint(
            "Trên PC: mở Cam Studio, chọn điện thoại này trong danh sách camera.\n" +
            "Nếu không thấy, chọn \"Thêm theo địa chỉ IP\" và nhập địa chỉ ở trên.")
        val camera = section("Camera").apply {
            addView(cameraStatus)
            addView(address)
            addView(cameraButton, centered().apply { topMargin = dp(12) })
            addView(backgroundSwitch, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(20) })
            addView(backgroundHint)
            addView(cameraHint)
        }

        // ---- Màn hình ----
        screenButton = Button(this).apply {
            setOnClickListener { toggleScreenSharing() }
        }
        screenStatus = TextView(this).apply {
            textSize = 13f
            setTextColor(Color.rgb(0x9A, 0xC8, 0xFF))
            gravity = Gravity.CENTER
            setPadding(0, dp(8), 0, 0)
        }
        val screen = section("Màn hình").apply {
            addView(screenButton, centered())
            addView(screenStatus)
            addView(hint("Máy tính thấy màn hình như một nguồn riêng. Vẫn chia sẻ khi bạn mở ứng dụng khác."))
        }

        // ---- Cập nhật (hidden until a newer version is found) ----
        updateText = TextView(this).apply {
            textSize = 14f
            setTextColor(Color.rgb(0xDD, 0xDD, 0xDD))
            gravity = Gravity.CENTER
        }
        updateButton = Button(this).apply {
            text = "Cập nhật"
            setOnClickListener { startUpdate() }
        }
        updateCard = section("Có bản mới").apply {
            addView(updateText)
            addView(updateButton, centered().apply { topMargin = dp(8) })
            visibility = View.GONE
        }
        val version = TextView(this).apply {
            textSize = 12f
            setTextColor(Color.rgb(0x66, 0x66, 0x66))
            gravity = Gravity.CENTER
            setPadding(0, dp(16), 0, 0)
            text = "Phiên bản ${AppUpdater.currentVersion(this@MainActivity)}"
        }

        val crash = TextView(this).apply {
            textSize = 13f
            setTextColor(Color.rgb(0xFF, 0x9A, 0x9A))
            setTextIsSelectable(true)
            setPadding(0, dp(16), 0, 0)
            visibility = View.GONE
        }
        CrashLog.takeLast(this)?.let {
            crash.text = "Lần trước app bị lỗi (chụp màn hình này gửi cho người hỗ trợ):\n$it"
            crash.visibility = View.VISIBLE
        }

        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(dp(16), dp(24), dp(16), dp(24))
            addView(title)
            addView(updateCard, card())
            addView(camera, card())
            addView(screen, card())
            addView(crash)
            addView(version)
            setOnClickListener { setDimmed(false) } // tap to light the screen up
        }
        setContentView(ScrollView(this).apply {
            isFillViewport = true
            setBackgroundColor(Color.BLACK)
            addView(content)
        })
        showBackgroundHint()

        CameraService.listener = cameraListener
        val wanted = mutableListOf<String>()
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            wanted += Manifest.permission.CAMERA
        }
        // The microphone, for when the PC asks for the phone's sound (Cam Studio on the PC,
        // Cài đặt › Micro điện thoại). Asked here: the PC may ask while the phone is locked.
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            wanted += Manifest.permission.RECORD_AUDIO
            micAsked = true
        }
        // The notifications ("Cam Studio sẵn sàng làm camera", with Dừng) need this on Android 13+.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            wanted += Manifest.permission.POST_NOTIFICATIONS
        }
        if (wanted.isNotEmpty()) requestPermissions(wanted.toTypedArray(), REQUEST_PERMISSIONS)
    }

    override fun onStart() {
        super.onStart()
        visible = true
        CameraService.pairing = ::confirmPairing // only while someone can see the code
        CameraService.micRequest = ::askMicrophone
        AppUpdater.onStatus = { text -> runOnUiThread { updating = false; showUpdate(text) } }
        checkForUpdate()
        if (CameraService.running) {
            showCameraState(CameraService.lastStreaming, CameraService.lastStatus)
        } else {
            startCamera(askPermission = false)
        }
    }

    override fun onResume() {
        super.onResume()
        showAddress() // the network may have changed
        ScreenService.listener = { text -> runOnUiThread { showScreenState(text) } }
        if (ScreenService.running) showScreenState(screenStatus.text.toString()) else screenButton.text = "Chia sẻ màn hình sang máy tính"
    }

    override fun onStop() {
        visible = false
        CameraService.pairing = null
        CameraService.micRequest = null
        micAsked = false
        pairingDialogs.toList().forEach { it.dismiss() } // the PC hears "declined"
        // "Chạy nền" off: the camera stops with the app, as it always did. A rotation or theme change is not leaving.
        if (!store.cameraInBackground && !isChangingConfigurations) CameraService.stop(this)
        super.onStop()
    }

    override fun onDestroy() {
        if (CameraService.listener === cameraListener) CameraService.listener = null
        ScreenService.listener = null
        AppUpdater.onStatus = null
        super.onDestroy()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        if (requestCode == REQUEST_NOTIFICATIONS) {
            askScreenCapture() // the notification is optional: share either way
            return
        }
        if (requestCode != REQUEST_PERMISSIONS) return
        // Allowed now: the running service takes the microphone type too (startForegroundCompat).
        val mic = permissions.indexOf(Manifest.permission.RECORD_AUDIO)
        if (mic >= 0 && grantResults.getOrNull(mic) == PackageManager.PERMISSION_GRANTED && CameraService.running) CameraService.start(this)
        val camera = permissions.indexOf(Manifest.permission.CAMERA)
        if (camera < 0) return // only the notifications were asked
        if (grantResults.getOrNull(camera) == PackageManager.PERMISSION_GRANTED) {
            startCamera(askPermission = false)
        } else {
            showCameraState(false, "Cần quyền Camera để gửi hình sang máy tính (Cài đặt → Ứng dụng → Cam Studio → Quyền)")
        }
    }

    /**
     * The PC asked for the phone's sound and the microphone is not allowed: ask now, at most
     * once while the app is on screen (a PC that starts the stream again must not nag).
     * Allowed, onRequestPermissionsResult starts the service again and the sound follows.
     */
    private fun askMicrophone() = runOnUiThread {
        if (micAsked || !visible || isFinishing) return@runOnUiThread
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) return@runOnUiThread
        micAsked = true
        requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQUEST_PERMISSIONS)
    }

    // ---- camera ------------------------------------------------------------------------

    private fun startCamera(askPermission: Boolean) {
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            if (askPermission) {
                requestPermissions(arrayOf(Manifest.permission.CAMERA), REQUEST_PERMISSIONS)
            } else {
                showCameraState(false, "Cần quyền Camera để gửi hình sang máy tính")
            }
            return
        }
        if (CameraService.running) return
        // The camera service may only start while the app is on screen: now.
        cameraStarting = CameraService.start(this)
        showCameraState(false, if (cameraStarting) "Đang bật camera…" else "Android không cho bật camera. Hãy mở lại app.")
    }

    private fun showCameraState(streaming: Boolean, text: String) {
        cameraStatus.text = text
        cameraButton.visibility = if (cameraStarting) View.GONE else View.VISIBLE
        cameraButton.text = if (CameraService.running) "Tắt camera" else "Bật camera"
        showAddress()
        setDimmed(streaming)
    }

    private fun showAddress() {
        val port = CameraService.port
        address.text = if (port == 0) "" else PhoneServer.addresses(this).joinToString("\n") { "$it:$port" }
    }

    private fun showBackgroundHint() {
        backgroundHint.text = if (store.cameraInBackground) {
            "Camera vẫn phát khi bạn rời app hoặc tắt màn hình. Tắt bằng nút \"Dừng\" trong thông báo, hoặc \"Tắt camera\" ở trên."
        } else {
            "Camera chỉ chạy khi app đang mở: rời app thì máy tính mất hình."
        }
    }

    /** Runs on a session thread (CameraService.pairing); blocks until the person answers. */
    private fun confirmPairing(pcName: String, code: String): Boolean {
        val latch = CountDownLatch(1)
        var accepted = false
        var dialog: AlertDialog? = null
        runOnUiThread {
            if (!visible || isFinishing || isDestroyed) {
                latch.countDown()
                return@runOnUiThread
            }
            setDimmed(false)
            dialog = AlertDialog.Builder(this)
                .setTitle("Ghép đôi với $pcName?")
                .setMessage("Kiểm tra mã trên máy tính có giống mã này không:\n\n${code.substring(0, 3)} ${code.substring(3)}\n\nChỉ đồng ý nếu hai mã giống nhau.")
                .setPositiveButton("Đồng ý") { _, _ -> accepted = true; latch.countDown() }
                .setNegativeButton("Từ chối") { _, _ -> latch.countDown() }
                .setOnDismissListener { d -> pairingDialogs.removeAll { it === d }; latch.countDown() }
                .show()
                .also { pairingDialogs += it }
        }
        if (!latch.await(2, TimeUnit.MINUTES)) {
            runOnUiThread { dialog?.dismiss() } // the PC gave up waiting
        }
        return accepted
    }

    // ---- updates (AppUpdater, docs/updates.md) ------------------------------------------

    /** Looks on GitHub at most once an hour, when the app is opened. */
    private fun checkForUpdate() {
        val now = System.currentTimeMillis()
        if (newer != null || now - store.lastUpdateCheck < 60 * 60 * 1000L) return
        store.lastUpdateCheck = now
        val current = AppUpdater.currentVersion(this)
        Thread {
            val found = try { AppUpdater.fetchNewer(current) } catch (e: Exception) { null }
            if (found != null) runOnUiThread {
                newer = found
                showUpdate("Bản ${found.version} đã có (đang dùng $current).")
            }
        }.start()
    }

    private fun showUpdate(text: String) {
        updateCard.visibility = if (newer != null) View.VISIBLE else View.GONE
        updateText.text = text
        updateButton.isEnabled = !updating
    }

    private fun startUpdate() {
        val release = newer ?: return
        if (updating) return
        // Installing restarts the app: the PC loses the picture for a moment.
        if (CameraService.lastStreaming || ScreenService.running) {
            AlertDialog.Builder(this)
                .setTitle("Đang phát sang máy tính")
                .setMessage("Cập nhật sẽ ngắt hình trên máy tính khoảng nửa phút. Cập nhật ngay?")
                .setPositiveButton("Cập nhật") { _, _ -> downloadAndInstall(release) }
                .setNegativeButton("Để sau", null)
                .show()
            return
        }
        downloadAndInstall(release)
    }

    private fun downloadAndInstall(release: AppUpdater.Release) {
        // The first time Android asks to allow installing apps from Cam Studio.
        if (!packageManager.canRequestPackageInstalls()) {
            showUpdate("Cho phép \"Cài ứng dụng không rõ nguồn\" cho Cam Studio, rồi quay lại bấm Cập nhật.")
            startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:$packageName")))
            return
        }
        updating = true
        showUpdate("Đang tải bản ${release.version}…")
        Thread {
            try {
                val apk = AppUpdater.download(this, release) { p -> runOnUiThread { showUpdate("Đang tải bản ${release.version}… $p%") } }
                runOnUiThread { showUpdate("Đang cài bản ${release.version}…") }
                AppUpdater.install(this, apk)
            } catch (e: Exception) {
                runOnUiThread {
                    updating = false
                    showUpdate(e.message ?: "Không tải được bản mới")
                }
            }
        }.start()
    }

    // ---- screen sharing ----------------------------------------------------------------

    private fun toggleScreenSharing() {
        if (ScreenService.running) {
            ScreenService.stop(this)
            return
        }
        // The "sharing" notification (with its Stop button) needs this on Android 13+.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQUEST_NOTIFICATIONS)
            return
        }
        askScreenCapture()
    }

    private fun askScreenCapture() {
        val manager = getSystemService(MediaProjectionManager::class.java)
        @Suppress("DEPRECATION")
        startActivityForResult(manager.createScreenCaptureIntent(), REQUEST_SCREEN)
    }

    @Deprecated("Activity result API")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_SCREEN) return
        if (resultCode == RESULT_OK && data != null) {
            ScreenService.start(this, resultCode, data)
            showScreenState("Đang chuẩn bị chia sẻ màn hình…")
        } else {
            showScreenState("")
        }
    }

    private fun showScreenState(text: String) {
        screenButton.text = if (ScreenService.running) "Dừng chia sẻ màn hình" else "Chia sẻ màn hình sang máy tính"
        screenStatus.text = text
    }

    // ---- layout ------------------------------------------------------------------------

    private fun dp(v: Int) = (v * resources.displayMetrics.density).roundToInt()

    /** A titled part of the screen, on a dark card. */
    private fun section(name: String) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(20), dp(16), dp(20), dp(20))
        background = GradientDrawable().apply {
            setColor(Color.rgb(0x1A, 0x1A, 0x1A))
            cornerRadius = dp(16).toFloat()
        }
        addView(TextView(this@MainActivity).apply {
            text = name
            textSize = 18f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.WHITE)
            setPadding(0, 0, 0, dp(10))
        })
    }

    private fun card() = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
        topMargin = dp(16)
    }

    private fun centered() = LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
        gravity = Gravity.CENTER_HORIZONTAL
    }

    private fun hint(text: String) = TextView(this).apply {
        this.text = text
        textSize = 13f
        setTextColor(Color.rgb(0x88, 0x88, 0x88))
        setPadding(0, dp(12), 0, 0)
    }

    private companion object {
        const val REQUEST_PERMISSIONS = 1
        const val REQUEST_SCREEN = 10
        const val REQUEST_NOTIFICATIONS = 11
    }

    private fun setDimmed(dim: Boolean) {
        window.attributes = window.attributes.apply {
            screenBrightness = if (dim) 0.02f else WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        }
    }
}

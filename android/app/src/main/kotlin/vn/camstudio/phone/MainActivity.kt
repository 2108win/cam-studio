package vn.camstudio.phone

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.view.Gravity
import android.view.OrientationEventListener
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * The phone side of Cam Studio. The PC picks lens, size and controls; this
 * screen shows status, the address for manual pairing and the pairing code.
 * It must stay open while streaming (Android denies the camera to
 * background apps), so the screen dims itself.
 */
class MainActivity : Activity(), Session.Ui {
    private lateinit var status: TextView
    private lateinit var address: TextView
    private lateinit var screenButton: Button
    private lateinit var screenStatus: TextView
    private lateinit var server: PhoneServer
    private lateinit var orientation: OrientationEventListener
    @Volatile private var resumed = false
    @Volatile private var rotation = 0

    override val inForeground: Boolean get() = resumed
    override val deviceRotation: Int get() = rotation

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        CrashLog.install(this)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        val title = TextView(this).apply {
            text = "Cam Studio"
            textSize = 26f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
        }
        status = TextView(this).apply {
            textSize = 16f
            setTextColor(Color.rgb(0xDD, 0xDD, 0xDD))
            gravity = Gravity.CENTER
            setPadding(48, 24, 48, 0)
        }
        address = TextView(this).apply {
            textSize = 14f
            setTextColor(Color.rgb(0x9A, 0xC8, 0xFF))
            gravity = Gravity.CENTER
            setPadding(48, 32, 48, 0)
            setTextIsSelectable(true)
        }
        val hint = TextView(this).apply {
            text = "Trên PC: mở Cam Studio, chọn điện thoại này trong danh sách camera.\n" +
                "Nếu không thấy, chọn \"Thêm theo địa chỉ IP\" và nhập địa chỉ ở trên.\n" +
                "Giữ ứng dụng này mở trong lúc dùng camera.\n" +
                "Chia sẻ màn hình thì vẫn chạy khi bạn mở ứng dụng khác."
            textSize = 13f
            setTextColor(Color.rgb(0x88, 0x88, 0x88))
            gravity = Gravity.CENTER
            setPadding(48, 40, 48, 0)
        }
        screenButton = Button(this).apply {
            setOnClickListener { toggleScreenSharing() }
        }
        screenStatus = TextView(this).apply {
            textSize = 13f
            setTextColor(Color.rgb(0x9A, 0xC8, 0xFF))
            gravity = Gravity.CENTER
            setPadding(48, 16, 48, 0)
        }
        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setBackgroundColor(Color.BLACK)
            addView(title)
            addView(status)
            addView(address)
            addView(hint)
            addView(screenButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                gravity = Gravity.CENTER_HORIZONTAL
                topMargin = 48
            })
            addView(screenStatus)
            setOnClickListener { setDimmed(false) } // tap to light the screen up
        })

        orientation = object : OrientationEventListener(this) {
            override fun onOrientationChanged(degrees: Int) {
                if (degrees == ORIENTATION_UNKNOWN) return
                val snapped = ((degrees + 45) / 90 * 90) % 360
                if (snapped != rotation) {
                    rotation = snapped
                    server.session?.onRotationChanged()
                }
            }
        }

        server = PhoneServer(this, this, CameraSource(this, this))
        server.start()
        status("Đang chờ máy tính kết nối…")
        CrashLog.takeLast(this)?.let { crash ->
            screenStatus.setTextIsSelectable(true)
            screenStatus.setTextColor(Color.rgb(0xFF, 0x9A, 0x9A))
            screenStatus.text = "Lần trước app bị lỗi (chụp màn hình này gửi cho người hỗ trợ):\n$crash"
        }

        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), 1)
        }
    }

    override fun onResume() {
        super.onResume()
        resumed = true
        orientation.enable()
        address.text = server.addresses().joinToString("\n") { "$it:${server.port}" }
        ScreenService.listener = { text -> runOnUiThread { showScreenState(text) } }
        if (ScreenService.running) showScreenState(screenStatus.text.toString()) else screenButton.text = "Chia sẻ màn hình sang máy tính"
    }

    override fun onPause() {
        super.onPause()
        resumed = false
        orientation.disable()
    }

    override fun onDestroy() {
        ScreenService.listener = null
        server.stop()
        super.onDestroy()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        if (requestCode == REQUEST_NOTIFICATIONS) {
            askScreenCapture() // the notification is optional: share either way
            return
        }
        if (grantResults.firstOrNull() != PackageManager.PERMISSION_GRANTED) {
            status("Cần quyền Camera để gửi hình sang máy tính")
        }
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

    // ---- Session.Ui ------------------------------------------------------------------

    override fun status(streaming: Boolean, text: String) = runOnUiThread {
        status.text = text
        setDimmed(streaming)
    }

    private fun status(text: String) = status(false, text)

    override fun confirmPairing(pcName: String, code: String): Boolean {
        val latch = CountDownLatch(1)
        var accepted = false
        runOnUiThread {
            setDimmed(false)
            AlertDialog.Builder(this)
                .setTitle("Ghép đôi với $pcName?")
                .setMessage("Kiểm tra mã trên máy tính có giống mã này không:\n\n${code.substring(0, 3)} ${code.substring(3)}\n\nChỉ đồng ý nếu hai mã giống nhau.")
                .setPositiveButton("Đồng ý") { _, _ -> accepted = true; latch.countDown() }
                .setNegativeButton("Từ chối") { _, _ -> latch.countDown() }
                .setOnCancelListener { latch.countDown() }
                .show()
        }
        latch.await(2, TimeUnit.MINUTES)
        return accepted
    }

    private companion object {
        const val REQUEST_SCREEN = 10
        const val REQUEST_NOTIFICATIONS = 11
    }

    private fun setDimmed(dim: Boolean) {
        window.attributes = window.attributes.apply {
            screenBrightness = if (dim) 0.02f else WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        }
    }
}

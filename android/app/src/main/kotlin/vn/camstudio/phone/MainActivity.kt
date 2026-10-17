package vn.camstudio.phone

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.view.Gravity
import android.view.OrientationEventListener
import android.view.WindowManager
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
    private lateinit var server: PhoneServer
    private lateinit var orientation: OrientationEventListener
    @Volatile private var resumed = false
    @Volatile private var rotation = 0

    override val inForeground: Boolean get() = resumed
    override val deviceRotation: Int get() = rotation

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
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
                "Giữ ứng dụng này mở trong lúc dùng camera."
            textSize = 13f
            setTextColor(Color.rgb(0x88, 0x88, 0x88))
            gravity = Gravity.CENTER
            setPadding(48, 40, 48, 0)
        }
        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setBackgroundColor(Color.BLACK)
            addView(title)
            addView(status)
            addView(address)
            addView(hint)
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

        server = PhoneServer(this, this)
        server.start()
        status("Đang chờ máy tính kết nối…")

        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), 1)
        }
    }

    override fun onResume() {
        super.onResume()
        resumed = true
        orientation.enable()
        address.text = server.addresses().joinToString("\n") { "$it:${server.port}" }
    }

    override fun onPause() {
        super.onPause()
        resumed = false
        orientation.disable()
    }

    override fun onDestroy() {
        server.stop()
        super.onDestroy()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        if (grantResults.firstOrNull() != PackageManager.PERMISSION_GRANTED) {
            status("Cần quyền Camera để gửi hình sang máy tính")
        }
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

    private fun setDimmed(dim: Boolean) {
        window.attributes = window.attributes.apply {
            screenBrightness = if (dim) 0.02f else WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        }
    }
}

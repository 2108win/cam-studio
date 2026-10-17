package vn.camstudio.phone

import android.content.Context
import android.graphics.SurfaceTexture
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.projection.MediaProjection
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.os.Handler
import android.os.HandlerThread
import android.util.DisplayMetrics
import android.view.Display
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * The phone screen through MediaProjection, drawn into an encoder's input surface.
 *
 * One VirtualDisplay lives for the whole sharing session (Android 14 allows a single one per
 * consent) and renders into a SurfaceTexture; streams attach and detach their encoder surface.
 * An OpenGL pass copies the latest picture into the encoder:
 *  - always at the stream's portrait size: a landscape screen is turned a quarter turn
 *    clockwise into it and the PC turns it back ("orientation" 270), like the iPhone;
 *  - again every 250 ms while the screen is still (the PC drops a source that is silent for 6 s,
 *    and one that joins needs a picture now);
 *  - never faster than the stream's frame rate.
 */
class ScreenCapture(context: Context, private val projection: MediaProjection, onStopped: () -> Unit) {
    private val displays = context.getSystemService(DisplayManager::class.java)
    private val display: Display = displays.getDisplay(Display.DEFAULT_DISPLAY)
    private val thread = HandlerThread("screen-gl").apply { start() }
    private val handler = Handler(thread.looper)

    /** The screen's native size in its natural (portrait) orientation. */
    val portraitSize: Pair<Int, Int>

    /** Clockwise degrees the PC applies to the stream (0 portrait, 270 landscape). */
    @Volatile var orientation = 0
        private set

    // ---- GL thread state ----
    private var eglDisplay = EGL14.EGL_NO_DISPLAY
    private var eglContext = EGL14.EGL_NO_CONTEXT
    private var eglConfig: EGLConfig? = null
    private var idleSurface = EGL14.EGL_NO_SURFACE // 1x1 pbuffer, or none (surfaceless)
    private var program = 0
    private var texture = 0
    private lateinit var surfaceTexture: SurfaceTexture
    private lateinit var displaySurface: Surface
    private var virtualDisplay: VirtualDisplay? = null
    private val texMatrix = FloatArray(16)
    private var hasFrame = false
    private var contentW = 0
    private var contentH = 0
    private var dpi = 320

    private var target: EGLSurface = EGL14.EGL_NO_SURFACE
    private var targetW = 0
    private var targetH = 0
    private var minIntervalNs = 33_333_333L
    private var lastDrawNs = 0L
    private var drawPending = false
    private var onOrientation: ((Int) -> Unit)? = null

    private val quad: FloatBuffer = floatBuffer(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f)
    private val st: FloatBuffer = floatBuffer(0f, 0f, 1f, 0f, 0f, 1f, 1f, 1f)

    private val tick = object : Runnable {
        override fun run() {
            val now = System.nanoTime()
            if (target != EGL14.EGL_NO_SURFACE && hasFrame && now - lastDrawNs > 250_000_000L) draw(now) // still screen
            handler.postDelayed(this, 100)
        }
    }

    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(id: Int) {}
        override fun onDisplayRemoved(id: Int) {}
        override fun onDisplayChanged(id: Int) {
            if (id == Display.DEFAULT_DISPLAY) followDisplay()
        }
    }

    init {
        val (w, h) = realSize()
        portraitSize = minOf(w, h) to maxOf(w, h)
        // Android 14 requires the callback before the virtual display exists.
        projection.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() = onStopped()
        }, handler)
        onGl {
            setupGl()
            surfaceTexture.setDefaultBufferSize(w, h)
            contentW = w
            contentH = h
            orientation = if (w > h) 270 else 0
            virtualDisplay = projection.createVirtualDisplay("Cam Studio", w, h, dpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, displaySurface, null, handler)
        }
        displays.registerDisplayListener(displayListener, handler)
    }

    /** Starts drawing into an encoder's input surface (portrait `width`×`height`). */
    fun attach(surface: Surface, width: Int, height: Int, fps: Int, onOrientation: (Int) -> Unit) = onGl {
        detachOnGl()
        target = EGL14.eglCreateWindowSurface(eglDisplay, eglConfig, surface, intArrayOf(EGL14.EGL_NONE), 0)
        if (target == EGL14.EGL_NO_SURFACE) throw IllegalStateException("eglCreateWindowSurface 0x${Integer.toHexString(EGL14.eglGetError())}")
        targetW = width
        targetH = height
        minIntervalNs = 1_000_000_000L / fps.coerceIn(1, 60)
        lastDrawNs = 0
        this.onOrientation = onOrientation
        if (hasFrame) draw(System.nanoTime()) // the screen may be still: start from what is there
        handler.removeCallbacks(tick)
        handler.postDelayed(tick, 100)
    }

    fun detach() = onGl { detachOnGl() }

    /** A frame now (a keyframe request: the screen may be still). */
    fun redraw() = handler.post { if (target != EGL14.EGL_NO_SURFACE && hasFrame) draw(System.nanoTime()) }

    fun release() {
        displays.unregisterDisplayListener(displayListener)
        onGl {
            detachOnGl()
            virtualDisplay?.release()
            virtualDisplay = null
            if (::surfaceTexture.isInitialized) surfaceTexture.release()
            if (::displaySurface.isInitialized) displaySurface.release()
            if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
                EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                if (idleSurface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(eglDisplay, idleSurface)
                EGL14.eglDestroyContext(eglDisplay, eglContext)
                EGL14.eglTerminate(eglDisplay)
            }
            eglDisplay = EGL14.EGL_NO_DISPLAY
        }
        runCatching { projection.stop() }
        thread.quitSafely()
    }

    // ---- GL thread -----------------------------------------------------------------------

    private fun detachOnGl() {
        handler.removeCallbacks(tick)
        onOrientation = null
        if (target != EGL14.EGL_NO_SURFACE) {
            makeIdleCurrent()
            EGL14.eglDestroySurface(eglDisplay, target)
            target = EGL14.EGL_NO_SURFACE
        }
    }

    /** The phone was turned: follow it so the mirrored screen fills the virtual display. */
    private fun followDisplay() {
        val (w, h) = realSize()
        if (w == contentW && h == contentH) return
        contentW = w
        contentH = h
        surfaceTexture.setDefaultBufferSize(w, h)
        virtualDisplay?.resize(w, h, dpi)
        val o = if (w > h) 270 else 0
        if (o != orientation) {
            orientation = o
            onOrientation?.invoke(o)
        }
    }

    private fun onFrameAvailable() {
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) return
        if (target != EGL14.EGL_NO_SURFACE) EGL14.eglMakeCurrent(eglDisplay, target, target, eglContext) else makeIdleCurrent()
        surfaceTexture.updateTexImage()
        surfaceTexture.getTransformMatrix(texMatrix)
        hasFrame = true
        if (target == EGL14.EGL_NO_SURFACE) return
        val now = System.nanoTime()
        val wait = lastDrawNs + minIntervalNs * 9 / 10 - now
        if (wait <= 0) {
            draw(now)
        } else if (!drawPending) {
            // Faster than the stream's rate: draw the newest picture when it is due.
            drawPending = true
            handler.postDelayed({
                drawPending = false
                if (target != EGL14.EGL_NO_SURFACE) draw(System.nanoTime())
            }, wait / 1_000_000 + 1)
        }
    }

    private fun draw(now: Long) {
        EGL14.eglMakeCurrent(eglDisplay, target, target, eglContext)
        GLES20.glViewport(0, 0, targetW, targetH)
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texture)
        GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(program, "uTex"), 1, false, texMatrix, 0)
        // A landscape screen into the portrait stream: a quarter turn clockwise.
        val turn = contentW > contentH && targetW < targetH
        GLES20.glUniform1f(GLES20.glGetUniformLocation(program, "uTurn"), if (turn) 1f else 0f)
        val aPos = GLES20.glGetAttribLocation(program, "aPos")
        val aSt = GLES20.glGetAttribLocation(program, "aSt")
        GLES20.glEnableVertexAttribArray(aPos)
        GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, quad)
        GLES20.glEnableVertexAttribArray(aSt)
        GLES20.glVertexAttribPointer(aSt, 2, GLES20.GL_FLOAT, false, 0, st)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        EGLExt.eglPresentationTimeANDROID(eglDisplay, target, now)
        EGL14.eglSwapBuffers(eglDisplay, target)
        lastDrawNs = now
    }

    private fun setupGl() {
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        val version = IntArray(2)
        check(EGL14.eglInitialize(eglDisplay, version, 0, version, 1)) { "eglInitialize" }
        // Recordable (feeds a MediaCodec surface); a pbuffer as well if the driver allows both.
        eglConfig = chooseConfig(EGL14.EGL_WINDOW_BIT or EGL14.EGL_PBUFFER_BIT) ?: chooseConfig(EGL14.EGL_WINDOW_BIT)
            ?: throw IllegalStateException("no EGL config")
        eglContext = EGL14.eglCreateContext(eglDisplay, eglConfig, EGL14.EGL_NO_CONTEXT,
            intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0)
        idleSurface = runCatching {
            EGL14.eglCreatePbufferSurface(eglDisplay, eglConfig, intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0)
        }.getOrDefault(EGL14.EGL_NO_SURFACE)
        makeIdleCurrent()

        program = buildProgram()
        val tex = IntArray(1)
        GLES20.glGenTextures(1, tex, 0)
        texture = tex[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texture)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        surfaceTexture = SurfaceTexture(texture)
        surfaceTexture.setOnFrameAvailableListener({ onFrameAvailable() }, handler)
        displaySurface = Surface(surfaceTexture)
    }

    private fun chooseConfig(surfaceType: Int): EGLConfig? {
        val attribs = intArrayOf(
            EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL_RECORDABLE_ANDROID, 1,
            EGL14.EGL_SURFACE_TYPE, surfaceType,
            EGL14.EGL_NONE,
        )
        val configs = arrayOfNulls<EGLConfig>(1)
        val count = IntArray(1)
        return if (EGL14.eglChooseConfig(eglDisplay, attribs, 0, configs, 0, 1, count, 0) && count[0] > 0) configs[0] else null
    }

    /** Current without drawing anywhere: the pbuffer, or no surface at all (EGL_KHR_surfaceless_context). */
    private fun makeIdleCurrent() {
        EGL14.eglMakeCurrent(eglDisplay, idleSurface, idleSurface, eglContext)
    }

    private fun buildProgram(): Int {
        fun shader(type: Int, src: String): Int = GLES20.glCreateShader(type).also {
            GLES20.glShaderSource(it, src)
            GLES20.glCompileShader(it)
            val ok = IntArray(1)
            GLES20.glGetShaderiv(it, GLES20.GL_COMPILE_STATUS, ok, 0)
            check(ok[0] != 0) { "shader: " + GLES20.glGetShaderInfoLog(it) }
        }
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, shader(GLES20.GL_VERTEX_SHADER, VERTEX))
        GLES20.glAttachShader(p, shader(GLES20.GL_FRAGMENT_SHADER, FRAGMENT))
        GLES20.glLinkProgram(p)
        val ok = IntArray(1)
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0)
        check(ok[0] != 0) { "program: " + GLES20.glGetProgramInfoLog(p) }
        return p
    }

    @Suppress("DEPRECATION") // getRealMetrics: the whole screen, in its current orientation
    private fun realSize(): Pair<Int, Int> {
        val m = DisplayMetrics()
        display.getRealMetrics(m)
        dpi = m.densityDpi
        return m.widthPixels to m.heightPixels
    }

    /** Runs `block` on the GL thread and waits for it (rethrowing its failure). */
    private fun onGl(block: () -> Unit) {
        if (Thread.currentThread() === thread) return block()
        var error: Throwable? = null
        val done = CountDownLatch(1)
        handler.post {
            try { block() } catch (t: Throwable) { error = t } finally { done.countDown() }
        }
        done.await(5, TimeUnit.SECONDS)
        error?.let { throw it }
    }

    private companion object {
        const val EGL_RECORDABLE_ANDROID = 0x3142

        // uTurn: output (s, t) shows source (1 - t, s), i.e. the picture turned 90° clockwise.
        const val VERTEX = """
            attribute vec4 aPos;
            attribute vec2 aSt;
            uniform mat4 uTex;
            uniform float uTurn;
            varying vec2 vTex;
            void main() {
                gl_Position = aPos;
                vec2 uv = uTurn > 0.5 ? vec2(1.0 - aSt.y, aSt.x) : aSt;
                vTex = (uTex * vec4(uv, 0.0, 1.0)).xy;
            }
        """
        const val FRAGMENT = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 vTex;
            uniform samplerExternalOES sTex;
            void main() { gl_FragColor = texture2D(sTex, vTex); }
        """

        fun floatBuffer(vararg v: Float): FloatBuffer =
            ByteBuffer.allocateDirect(v.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer().apply { put(v); position(0) }
    }
}

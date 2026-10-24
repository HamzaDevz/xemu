/*
 * Adapted from hakuX's NativeCoreActivity (https://github.com/rfandango/hakuX,
 * GPLv3 or later; see android/hakux/core/COPYING).
 */
package com.izzy2lost.x1box.hakux

import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Typeface
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.util.TypedValue
import android.view.InputDevice
import android.view.KeyEvent
import android.view.PixelCopy
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.RelativeLayout
import android.widget.TextView
import android.window.OnBackInvokedDispatcher
import com.izzy2lost.x1box.ControllerSettings
import com.izzy2lost.x1box.GameLibraryActivity
import com.izzy2lost.x1box.GpuDriverHelper
import com.izzy2lost.x1box.OnScreenController
import com.izzy2lost.x1box.OrientationLocker
import com.izzy2lost.x1box.PerGameSettingsManager
import com.izzy2lost.x1box.R
import java.io.File
import java.util.TimeZone
import org.libsdl.app.SDLActivity

/**
 * The hakuX core (libhakuxcore.so: FEX, an HLE Xbox kernel, the HLE GPU) in
 * its own process, :hakux, for one run. The core takes everything as
 * arguments (getArguments) and is polled for what it reports: the frame
 * rate, a fatal error, the title leaving or rebooting into another XBE.
 */
class HakuxCoreActivity : SDLActivity() {
  private val handler = Handler(Looper.getMainLooper())
  private val prefs by lazy { getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE) }

  private var discUri: Uri? = null
  private var discArg = ""
  private var onScreenController: OnScreenController? = null
  private var controlsShown: Boolean? = null
  private var fpsView: TextView? = null
  private var shaderLine: TextView? = null

  override fun getLibraries(): Array<String> = arrayOf("SDL3", "hakuxcore")

  /** The setting for this game: its per-game override, else the global one. */
  private fun setting(key: String): String? =
    PerGameSettingsManager.getRuntimeOverride(this, key) ?: prefs.all[key]?.toString()

  override fun getArguments(): Array<String> {
    val args = mutableListOf(discArg, File(filesDir, "hakux/hdd").absolutePath)
    if (intent.getBooleanExtra(EXTRA_RELAUNCH, false)) {
      args += "relaunch"
    }
    runCatching { packageManager.getPackageInfo(packageName, 0).versionName }.getOrNull()?.let {
      args += "version=$it"
    }
    /* The app's own display settings: the resolution scale (1-3x) and the
     * picture's shape (0 stretch, 1 4:3, 2 16:9). */
    val scale = setting("setting_surface_scale")?.toIntOrNull()?.coerceIn(1, 4) ?: 1
    args += "scale=$scale"
    args += "aspect=" + when (setting("setting_display_mode")?.toIntOrNull() ?: 0) {
      1 -> "4:3"
      2 -> "16:9"
      else -> "stretch"
    }
    args += "tz=${TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60_000}"
    /* Player 1 only, from the first gamepad SDL sees plus the on-screen controls. */
    args += "pads=1"
    args += "padinput=sdl"
    vkDriverArg()?.let { args += it }
    return args.toTypedArray()
  }

  /**
   * The custom Vulkan driver (Turnip...) the core loads through adrenotools,
   * chosen as libxemu's is: the game's override ("system" or a driver ZIP),
   * else the installed one. Null: the system driver.
   */
  private fun vkDriverArg(): String? {
    GpuDriverHelper.init(this)
    if (!GpuDriverHelper.supportsCustomDriverLoading()) return null
    val override = PerGameSettingsManager.getRuntimeOverride(this, "setting_gpu_driver")
    if (override == "system") return null
    if (override != null) {
      val zip = File(GpuDriverHelper.driverStorageDir, override)
      if (!zip.exists() || !GpuDriverHelper.installDriver(zip)) {
        Log.w(TAG, "GPU driver: per-game $override not found or install failed, using the system driver")
        return null
      }
    }
    val lib = GpuDriverHelper.getInstalledDriverLibrary() ?: return null
    return "vkdriver=${GpuDriverHelper.driverInstallDir}|$lib|${GpuDriverHelper.hookLibDir}"
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    discArg = openDisc()
    super.onCreate(savedInstanceState)
    OrientationLocker(this, landscapeOnly = true).enable()
    setupOnScreenController()
    setupOverlayText()
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT) { showMenu() }
    }
  }

  /** The disc, opened in this process: the core reads it by descriptor. */
  private fun openDisc(): String {
    val uri = intent.getStringExtra(EXTRA_DVD_URI)
    discUri = uri?.takeIf { it.isNotBlank() }?.let(Uri::parse)
    val u = discUri ?: run {
      Log.e(TAG, "no game to run")
      return ""
    }
    return try {
      "fd:${contentResolver.openFileDescriptor(u, "r")!!.detachFd()}"
    } catch (e: Exception) {
      Log.e(TAG, "can't open $u: ${e.message}")
      "" /* the core reports NC_FATAL_BOOT and the dialog goes back */
    }
  }

  // ---------------------------------------------------------------- pause

  private var activityPaused = false
  private var menuDialogs = 0
  private var nativePaused: Boolean? = null

  /** The emulation stops while the activity is paused or a menu dialog is open. */
  private fun syncPause() {
    val want = activityPaused || menuDialogs > 0
    if (nativePaused != want) {
      nativePaused = want
      nativeSetPaused(want)
    }
  }

  private fun showMenuDialog(b: AlertDialog.Builder): AlertDialog {
    menuDialogs++
    syncPause()
    b.setOnDismissListener {
      menuDialogs--
      syncPause()
    }
    return b.show()
  }

  override fun onResume() {
    super.onResume()
    activityPaused = false
    syncPause()
    hideSystemBars()
    fpsView?.visibility = if (prefs.getBoolean("show_fps", false)) View.VISIBLE else View.GONE
    handler.removeCallbacks(poll)
    handler.postDelayed(poll, POLL_MS)
  }

  override fun onPause() {
    handler.removeCallbacks(poll)
    activityPaused = true
    syncPause()
    super.onPause()
  }

  override fun onDestroy() {
    handler.removeCallbacksAndMessages(null)
    super.onDestroy()
  }

  override fun onWindowFocusChanged(hasFocus: Boolean) {
    super.onWindowFocusChanged(hasFocus)
    if (hasFocus) hideSystemBars()
  }

  private fun hideSystemBars() {
    if (Build.VERSION.SDK_INT >= 30) {
      window.insetsController?.let {
        it.hide(WindowInsets.Type.systemBars())
        it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
      }
    }
  }

  // ---------------------------------------------------------------- what the core reports

  private var fatalShown = false

  private val poll = object : Runnable {
    override fun run() {
      if (fpsView?.visibility == View.VISIBLE) {
        val s = nativeGetFrameStats()
        if (s != null && s.size >= 3) {
          fpsView?.text = "hakuX  FPS: %.0f  |  %.1fms avg  /  %.1fms last".format(s[0], s[1], s[2])
        }
      }
      val why = nativeGetFatal()
      if (why != null && !fatalShown) {
        fatalShown = true
        showFatal(why, nativeGetFatalKind())
        return
      }
      /* The title rebooted into another of its XBEs: a new core process. */
      if (nativeGetRelaunch()) {
        relaunchCore()
        return
      }
      /* The title left (to the dashboard, a reboot, a shutdown): the game closes, as on a console. */
      if (nativeGetQuit()) {
        Log.i(TAG, "the title left: closing the game")
        exitGame()
        return
      }
      handler.postDelayed(this, POLL_MS)
    }
  }

  private fun showFatal(why: String, kind: Int) {
    val (title, message) = when (kind) {
      FATAL_DRIVER -> R.string.hakux_fatal_driver_title to getString(R.string.hakux_fatal_driver, why)
      FATAL_GPU_LOST -> R.string.hakux_fatal_gpu_lost_title to getString(R.string.hakux_fatal_gpu_lost, why)
      FATAL_BOOT -> R.string.hakux_fatal_boot_title to getString(R.string.hakux_fatal_boot, why)
      else -> R.string.hakux_fatal_incompatible_title to getString(R.string.hakux_fatal_incompatible)
    }
    AlertDialog.Builder(this)
      .setTitle(title)
      .setMessage(message)
      .setCancelable(false)
      .setPositiveButton(android.R.string.ok) { _, _ -> exitGame() }
      .show()
  }

  /** "Building shaders n / N" while the core builds the game's cached shaders before it starts. */
  private fun setupOverlayText() {
    val fps = TextView(this).apply {
      text = "hakuX  FPS: --"
      setTextColor(Color.WHITE)
      setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
      typeface = Typeface.MONOSPACE
      setShadowLayer(2f, 1f, 1f, Color.BLACK)
      setPadding(16, 8, 16, 8)
      setBackgroundColor(Color.argb(100, 0, 0, 0))
      maxLines = 1
      visibility = View.GONE
    }
    mLayout?.addView(fps, RelativeLayout.LayoutParams(
      RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT,
    ).apply {
      addRule(RelativeLayout.ALIGN_PARENT_TOP)
      addRule(RelativeLayout.ALIGN_PARENT_START)
    })
    fpsView = fps

    val line = TextView(this).apply {
      visibility = View.GONE
      setTextColor(Color.WHITE)
      setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
      setShadowLayer(2f, 1f, 1f, Color.BLACK)
      setPadding(24, 12, 24, 12)
      setBackgroundColor(Color.argb(140, 0, 0, 0))
    }
    mLayout?.addView(line, RelativeLayout.LayoutParams(
      RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT,
    ).apply { addRule(RelativeLayout.CENTER_IN_PARENT) })
    shaderLine = line
    val started = SystemClock.uptimeMillis()
    var seen = false
    handler.post(object : Runnable {
      override fun run() {
        val p = nativeGetShaderBuild()
        val total = p?.getOrNull(1) ?: 0
        if (total > 0) {
          seen = true
          line.text = getString(R.string.hakux_shader_build, p!![0], total)
          line.visibility = View.VISIBLE
        } else {
          line.visibility = View.GONE
          if (seen || SystemClock.uptimeMillis() - started > 30_000) return
        }
        handler.postDelayed(this, 200)
      }
    })
  }

  // ---------------------------------------------------------------- controls and menu

  private fun setupOnScreenController() {
    val settings = ControllerSettings(this)
    val overlay = OnScreenController(this).apply {
      layoutParams = ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
      alpha = settings.controllerOpacity
      setLayout(settings.touchLayout)
      onLayoutEdited = { settings.touchLayout = it }
    }
    overlay.setControllerListener(OverlayBridge())
    overlay.onMenuButtonTapped = { showMenu() }
    mLayout?.addView(overlay)
    onScreenController = overlay
    updateControllerVisibility()
  }

  /** The controls show while no gamepad is attached, unless the menu said otherwise. */
  private fun updateControllerVisibility() {
    val shown = controlsShown ?: !hasGamepad()
    onScreenController?.visibility = if (shown) View.VISIBLE else View.GONE
  }

  private fun hasGamepad(): Boolean = InputDevice.getDeviceIds().any { id ->
    val d = InputDevice.getDevice(id) ?: return@any false
    !d.isVirtual && (d.sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
      d.sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK)
  }

  private fun showMenu() {
    onScreenController?.resetAllInputs()
    val shown = controlsShown ?: !hasGamepad()
    val items = listOf<Pair<String, () -> Unit>>(
      getString(if (shown) R.string.hakux_menu_hide_controls else R.string.hakux_menu_show_controls) to {
        controlsShown = !shown
        updateControllerVisibility()
      },
      getString(R.string.hakux_menu_exit) to ::exitGame,
    )
    showMenuDialog(
      AlertDialog.Builder(this)
        .setItems(items.map { it.first }.toTypedArray()) { _, which -> items[which].second() }
        .setNegativeButton(R.string.hakux_menu_resume, null)
    )
  }

  @Deprecated("Android 12 and older; newer ones call the OnBackInvokedCallback.")
  override fun onBackPressed() = showMenu()

  /*
   * The system's back (navigation button, back swipe, a keyboard) opens the
   * menu before SDL takes it and closes the game; a gamepad's own Back stays
   * the game's, and its guide button opens the menu.
   */
  override fun dispatchKeyEvent(event: KeyEvent): Boolean {
    val fromPad = event.device?.let { it.sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD } == true
    if (event.keyCode == KeyEvent.KEYCODE_BACK && !fromPad) {
      if (event.action == KeyEvent.ACTION_UP && !event.isCanceled) showMenu()
      return true
    }
    if (event.keyCode == KeyEvent.KEYCODE_BUTTON_MODE && fromPad) {
      if (event.action == KeyEvent.ACTION_UP && !event.isCanceled) showMenu()
      return true
    }
    return super.dispatchKeyEvent(event)
  }

  // ---------------------------------------------------------------- leaving

  /** Back to the library; the core keeps its state in this process for one run, so the process goes too. */
  private fun exitGame() {
    startActivity(
      Intent(applicationContext, GameLibraryActivity::class.java)
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
    )
    finish()
    android.os.Process.killProcess(android.os.Process.myPid())
  }

  private var relaunchStarted = false

  /**
   * The same disc again in a new core process, booting the XBE the title
   * launched (the core saved its launch data page). HakuxRelaunchActivity, in
   * the main process, starts it once this one is gone, showing the frame on
   * screen now meanwhile.
   */
  private fun relaunchCore() {
    if (relaunchStarted) return
    Log.i(TAG, "relaunch: the title launched another XBE; starting the core again")
    val surface = mSurface
    if (surface != null && surface.width > 0 && surface.height > 0) {
      val frame = Bitmap.createBitmap(surface.width, surface.height, Bitmap.Config.ARGB_8888)
      val timeout = Runnable { startRelaunch() }
      try {
        PixelCopy.request(surface, frame, { result ->
          handler.removeCallbacks(timeout)
          if (result != PixelCopy.SUCCESS) {
            startRelaunch()
          } else {
            Thread {
              val file = HakuxRelaunchActivity.heldFrame(this)
              runCatching { file.outputStream().use { frame.compress(Bitmap.CompressFormat.JPEG, 92, it) } }
              runOnUiThread { startRelaunch() }
            }.start()
          }
        }, handler)
        handler.postDelayed(timeout, RELAUNCH_COPY_MS)
        return
      } catch (e: IllegalArgumentException) {
        Log.w(TAG, "relaunch: the frame can't be copied: ${e.message}")
      }
    }
    startRelaunch()
  }

  private fun startRelaunch() {
    if (relaunchStarted) return
    relaunchStarted = true
    startActivity(
      Intent(this, HakuxRelaunchActivity::class.java)
        .putExtra(EXTRA_DVD_URI, discUri?.toString() ?: intent.getStringExtra(EXTRA_DVD_URI))
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION),
      HakuxRelaunchActivity.noTransition(this).toBundle(),
    )
    finish()
    HakuxRelaunchActivity.noCloseTransition(this)
    android.os.Process.killProcess(android.os.Process.myPid())
  }

  // ---------------------------------------------------------------- the on-screen controller's pad

  /** The on-screen controller's whole state, sent to the core (player 1) on every change. */
  private class OverlayBridge : OnScreenController.ControllerListener {
    private var buttons = 0
    private var analog = 0
    private var lt = 0f
    private var rt = 0f
    private var lx = 0f
    private var ly = 0f
    private var rx = 0f
    private var ry = 0f

    override fun onButtonPressed(button: OnScreenController.Button) = set(button, true)
    override fun onButtonReleased(button: OnScreenController.Button) = set(button, false)

    override fun onStickMoved(stick: OnScreenController.Stick, x: Float, y: Float) {
      if (stick == OnScreenController.Stick.LEFT) { lx = x; ly = y } else { rx = x; ry = y }
      send()
    }

    override fun onStickPressed(stick: OnScreenController.Stick) =
      bit(if (stick == OnScreenController.Stick.LEFT) PAD_LTHUMB else PAD_RTHUMB, true)

    override fun onStickReleased(stick: OnScreenController.Stick) =
      bit(if (stick == OnScreenController.Stick.LEFT) PAD_LTHUMB else PAD_RTHUMB, false)

    private fun set(button: OnScreenController.Button, down: Boolean) {
      when (button) {
        OnScreenController.Button.A -> analogBit(0, down)
        OnScreenController.Button.B -> analogBit(1, down)
        OnScreenController.Button.X -> analogBit(2, down)
        OnScreenController.Button.Y -> analogBit(3, down)
        OnScreenController.Button.BLACK -> analogBit(4, down)
        OnScreenController.Button.WHITE -> analogBit(5, down)
        OnScreenController.Button.LEFT_TRIGGER -> { lt = if (down) 1f else 0f; send() }
        OnScreenController.Button.RIGHT_TRIGGER -> { rt = if (down) 1f else 0f; send() }
        OnScreenController.Button.DPAD_UP -> bit(PAD_UP, down)
        OnScreenController.Button.DPAD_DOWN -> bit(PAD_DOWN, down)
        OnScreenController.Button.DPAD_LEFT -> bit(PAD_LEFT, down)
        OnScreenController.Button.DPAD_RIGHT -> bit(PAD_RIGHT, down)
        OnScreenController.Button.START -> bit(PAD_START, down)
        OnScreenController.Button.BACK -> bit(PAD_BACK, down)
        OnScreenController.Button.LEFT_STICK_BUTTON -> bit(PAD_LTHUMB, down)
        OnScreenController.Button.RIGHT_STICK_BUTTON -> bit(PAD_RTHUMB, down)
      }
    }

    private fun analogBit(i: Int, down: Boolean) {
      analog = if (down) analog or (1 shl i) else analog and (1 shl i).inv()
      send()
    }

    private fun bit(mask: Int, down: Boolean) {
      buttons = if (down) buttons or mask else buttons and mask.inv()
      send()
    }

    private fun send() = nativeSetOverlayPad(buttons, analog, lx, ly, rx, ry, lt, rt)
  }

  companion object {
    private const val TAG = "x1box-hakux"
    const val EXTRA_DVD_URI = "dvdUri"
    /** Boot the XBE of the saved launch data page (a title's XLaunchNewImage). */
    const val EXTRA_RELAUNCH = "relaunch"

    /* hakux/core/native/dev/usb.h NC_PAD_* */
    private const val PAD_UP = 0x01
    private const val PAD_DOWN = 0x02
    private const val PAD_LEFT = 0x04
    private const val PAD_RIGHT = 0x08
    private const val PAD_START = 0x10
    private const val PAD_BACK = 0x20
    private const val PAD_LTHUMB = 0x40
    private const val PAD_RTHUMB = 0x80

    /* hakux/core/native/hle/hle_native.h NC_FATAL_* */
    private const val FATAL_DRIVER = 1
    private const val FATAL_GPU_LOST = 4
    private const val FATAL_BOOT = 5

    private const val POLL_MS = 500L
    private const val RELAUNCH_COPY_MS = 1000L

    /** fps, average frame time (ms), last frame time (ms), 1 if push-buffer decoding renders. */
    @JvmStatic external fun nativeGetFrameStats(): FloatArray?
    /** Why the game can't run here (the GPU driver, or the game), or null. */
    @JvmStatic external fun nativeGetFatal(): String?
    @JvmStatic external fun nativeGetFatalKind(): Int
    /** The core stopped to boot another of the title's XBEs. */
    @JvmStatic external fun nativeGetRelaunch(): Boolean
    /** The core stopped as the title left (to the dashboard, a reboot, a shutdown). */
    @JvmStatic external fun nativeGetQuit(): Boolean
    /** The cached shaders' build before the game: {built, to build}, {0, 0} when none runs. */
    @JvmStatic external fun nativeGetShaderBuild(): IntArray?
    @JvmStatic external fun nativeSetOverlayPad(
      buttons: Int, analog: Int, lx: Float, ly: Float, rx: Float, ry: Float, lt: Float, rt: Float,
    )
    /** The emulation paused (onPause, a menu) or resumed. */
    @JvmStatic external fun nativeSetPaused(paused: Boolean)
  }
}

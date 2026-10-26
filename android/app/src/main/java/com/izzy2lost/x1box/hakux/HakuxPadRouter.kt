package com.izzy2lost.x1box.hakux

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import com.izzy2lost.x1box.ControllerRemap
import com.izzy2lost.x1box.ControllerRemap.Source
import com.izzy2lost.x1box.ControllerRemap.XboxInput
import kotlin.math.abs
import kotlin.math.max

/**
 * Physical controllers for the hakuX core: every gamepad's keys and axes (a handheld's
 * built-in controls and any paired pad alike) merged into player 1, through the app's
 * controller remap (ControllerRemap: the same table xemu's input takes), and sent as the
 * Duke's state (nativeSetPad). The core adds the on-screen controller's to it.
 */
class HakuxPadRouter(
  private var remap: Map<XboxInput, Source>,
  /** buttons (NC_PAD_* bits), analog bytes (A B X Y Black White LT RT), sticks (up positive). */
  private val send: (buttons: Int, analog: Long, lx: Int, ly: Int, rx: Int, ry: Int) -> Unit,
) {
  private class Device {
    val held = HashSet<Source>()
    var lt = 0f
    var rt = 0f
    var hatX = 0f
    var hatY = 0f
    var lx = 0f
    var ly = 0f
    var rx = 0f
    var ry = 0f
  }

  private val devices = HashMap<Int, Device>()
  private var last: List<Number>? = null

  /** The device that moved last (rumble goes to it). */
  var lastDeviceId = -1
    private set

  fun setRemap(map: Map<XboxInput, Source>) {
    remap = map
    publish()
  }

  /** Everything released (focus lost, a menu over the game). */
  fun reset() {
    devices.clear()
    publish()
  }

  fun forget(deviceId: Int) {
    if (devices.remove(deviceId) != null) publish()
  }

  /** A gamepad's key: true when taken (it doesn't go on to SDL or the system). */
  fun onKey(event: KeyEvent): Boolean {
    if (!isGamepad(event.device)) return false
    val source = Source.fromKeyCode(event.keyCode) ?: return false
    if (source == Source.GUIDE) return false        /* the activity's: its menu */
    val d = devices.getOrPut(event.deviceId, ::Device)
    when (event.action) {
      KeyEvent.ACTION_DOWN -> d.held.add(source)
      KeyEvent.ACTION_UP -> d.held.remove(source)
      else -> return true
    }
    lastDeviceId = event.deviceId
    publish()
    return true
  }

  fun onMotion(event: MotionEvent): Boolean {
    val dev = event.device ?: return false
    if (!isGamepad(dev) || event.action != MotionEvent.ACTION_MOVE) return false
    val d = devices.getOrPut(event.deviceId, ::Device)
    fun axis(a: Int): Float {
      val range = dev.getMotionRange(a, event.source) ?: return 0f
      val v = event.getAxisValue(a)
      return if (abs(v) <= max(range.flat, 0.08f)) 0f else v
    }
    d.lx = axis(MotionEvent.AXIS_X)
    d.ly = axis(MotionEvent.AXIS_Y)
    d.rx = axis(MotionEvent.AXIS_Z)
    d.ry = axis(MotionEvent.AXIS_RZ)
    d.hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
    d.hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
    d.lt = max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE))
    d.rt = max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS))
    lastDeviceId = event.deviceId
    publish()
    return true
  }

  /** A source's value over every device, 0..1. */
  private fun value(source: Source): Float {
    var v = 0f
    for (d in devices.values) {
      val x = when (source) {
        Source.NONE -> 0f
        Source.LEFT_TRIGGER -> max(d.lt, if (Source.LEFT_TRIGGER in d.held) 1f else 0f)
        Source.RIGHT_TRIGGER -> max(d.rt, if (Source.RIGHT_TRIGGER in d.held) 1f else 0f)
        Source.DPAD_LEFT -> if (Source.DPAD_LEFT in d.held || d.hatX < -0.5f) 1f else 0f
        Source.DPAD_RIGHT -> if (Source.DPAD_RIGHT in d.held || d.hatX > 0.5f) 1f else 0f
        Source.DPAD_UP -> if (Source.DPAD_UP in d.held || d.hatY < -0.5f) 1f else 0f
        Source.DPAD_DOWN -> if (Source.DPAD_DOWN in d.held || d.hatY > 0.5f) 1f else 0f
        else -> if (source in d.held) 1f else 0f
      }
      v = max(v, x)
    }
    return v
  }

  private fun input(i: XboxInput): Float = value(remap[i] ?: i.defaultSource)

  private fun publish() {
    var buttons = 0
    if (input(XboxInput.DPAD_UP) > 0.5f) buttons = buttons or PAD_UP
    if (input(XboxInput.DPAD_DOWN) > 0.5f) buttons = buttons or PAD_DOWN
    if (input(XboxInput.DPAD_LEFT) > 0.5f) buttons = buttons or PAD_LEFT
    if (input(XboxInput.DPAD_RIGHT) > 0.5f) buttons = buttons or PAD_RIGHT
    if (input(XboxInput.START) > 0.5f) buttons = buttons or PAD_START
    if (input(XboxInput.BACK) > 0.5f) buttons = buttons or PAD_BACK
    if (input(XboxInput.LEFT_STICK) > 0.5f) buttons = buttons or PAD_LTHUMB
    if (input(XboxInput.RIGHT_STICK) > 0.5f) buttons = buttons or PAD_RTHUMB
    val analogInputs = listOf(
      XboxInput.A, XboxInput.B, XboxInput.X, XboxInput.Y, XboxInput.BLACK, XboxInput.WHITE,
      XboxInput.LEFT_TRIGGER, XboxInput.RIGHT_TRIGGER,
    )
    var analog = 0L
    analogInputs.forEachIndexed { i, x ->
      analog = analog or ((input(x) * 255f).toLong().coerceIn(0L, 255L) shl (8 * i))
    }
    /* the stick pushed furthest across devices; the Duke's y is up-positive */
    var lx = 0f; var ly = 0f; var rx = 0f; var ry = 0f
    for (d in devices.values) {
      if (abs(d.lx) + abs(d.ly) > abs(lx) + abs(ly)) { lx = d.lx; ly = d.ly }
      if (abs(d.rx) + abs(d.ry) > abs(rx) + abs(ry)) { rx = d.rx; ry = d.ry }
    }
    val state = listOf(buttons, analog, s16(lx), s16(-ly), s16(rx), s16(-ry))
    if (state == last) return
    last = state
    send(buttons, analog, s16(lx), s16(-ly), s16(rx), s16(-ry))
  }

  private fun s16(v: Float) = (v.coerceIn(-1f, 1f) * 32767f).toInt()

  companion object {
    /* hakux/core/native/dev/usb.h NC_PAD_* */
    private const val PAD_UP = 0x01
    private const val PAD_DOWN = 0x02
    private const val PAD_LEFT = 0x04
    private const val PAD_RIGHT = 0x08
    private const val PAD_START = 0x10
    private const val PAD_BACK = 0x20
    private const val PAD_LTHUMB = 0x40
    private const val PAD_RTHUMB = 0x80

    fun isGamepad(device: InputDevice?): Boolean {
      val s = device?.sources ?: return false
      return !device.isVirtual && ((s and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
        (s and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
    }
  }
}

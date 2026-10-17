package com.izzy2lost.x1box

import android.app.Activity
import android.content.Context
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.ListView
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import com.google.android.material.dialog.MaterialAlertDialogBuilder

/**
 * Physical controller remap. Each Xbox input is driven by one source on the
 * connected pad; the table is handed to xemu-input.c through
 * MainActivity.nativeSetControllerRemap(). The on-screen controller is exempt
 * natively, since its buttons are labelled and rearranged instead.
 */
object ControllerRemap {

  /** Xbox inputs, in native slot order (gp.buttons bits, then LT and RT). */
  enum class XboxInput(val label: String, val defaultSource: Source) {
    A("A", Source.SOUTH),
    B("B", Source.EAST),
    X("X", Source.WEST),
    Y("Y", Source.NORTH),
    DPAD_LEFT("D-Pad Left", Source.DPAD_LEFT),
    DPAD_UP("D-Pad Up", Source.DPAD_UP),
    DPAD_RIGHT("D-Pad Right", Source.DPAD_RIGHT),
    DPAD_DOWN("D-Pad Down", Source.DPAD_DOWN),
    BACK("Back", Source.BACK),
    START("Start", Source.START),
    WHITE("White", Source.LEFT_SHOULDER),
    BLACK("Black", Source.RIGHT_SHOULDER),
    LEFT_STICK("Left Stick Click", Source.LEFT_STICK),
    RIGHT_STICK("Right Stick Click", Source.RIGHT_STICK),
    LEFT_TRIGGER("Left Trigger", Source.LEFT_TRIGGER),
    RIGHT_TRIGGER("Right Trigger", Source.RIGHT_TRIGGER),
  }

  /**
   * Inputs on the physical pad. [sdlCode] is the SDL_GamepadButton value, or
   * 100/101 for the trigger axes, matching xemu-input.c.
   */
  enum class Source(val sdlCode: Int, val label: String) {
    NONE(-1, "Unbound"),
    SOUTH(0, "A (bottom)"),
    EAST(1, "B (right)"),
    WEST(2, "X (left)"),
    NORTH(3, "Y (top)"),
    BACK(4, "Back / Select"),
    GUIDE(5, "Guide / Home"),
    START(6, "Start"),
    LEFT_STICK(7, "L3"),
    RIGHT_STICK(8, "R3"),
    LEFT_SHOULDER(9, "LB / L1"),
    RIGHT_SHOULDER(10, "RB / R1"),
    DPAD_UP(11, "D-Pad Up"),
    DPAD_DOWN(12, "D-Pad Down"),
    DPAD_LEFT(13, "D-Pad Left"),
    DPAD_RIGHT(14, "D-Pad Right"),
    LEFT_TRIGGER(100, "LT / L2"),
    RIGHT_TRIGGER(101, "RT / R2");

    companion object {
      fun fromSdlCode(code: Int): Source? = entries.firstOrNull { it.sdlCode == code }

      /** The source SDL reports for an Android gamepad keycode. */
      fun fromKeyCode(keyCode: Int): Source? = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A -> SOUTH
        KeyEvent.KEYCODE_BUTTON_B -> EAST
        KeyEvent.KEYCODE_BUTTON_X -> WEST
        KeyEvent.KEYCODE_BUTTON_Y -> NORTH
        KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BACK -> BACK
        KeyEvent.KEYCODE_BUTTON_MODE -> GUIDE
        KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> START
        KeyEvent.KEYCODE_BUTTON_THUMBL -> LEFT_STICK
        KeyEvent.KEYCODE_BUTTON_THUMBR -> RIGHT_STICK
        KeyEvent.KEYCODE_BUTTON_L1 -> LEFT_SHOULDER
        KeyEvent.KEYCODE_BUTTON_R1 -> RIGHT_SHOULDER
        KeyEvent.KEYCODE_BUTTON_L2 -> LEFT_TRIGGER
        KeyEvent.KEYCODE_BUTTON_R2 -> RIGHT_TRIGGER
        KeyEvent.KEYCODE_DPAD_UP -> DPAD_UP
        KeyEvent.KEYCODE_DPAD_DOWN -> DPAD_DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> DPAD_LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> DPAD_RIGHT
        else -> null
      }
    }
  }

  private const val PREFS = "controller_remap"
  private const val KEY_TABLE = "table"

  fun load(context: Context): MutableMap<XboxInput, Source> {
    val stored = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
      .getString(KEY_TABLE, null)
      ?.split(',')
      ?.mapNotNull { it.trim().toIntOrNull() }
    val map = mutableMapOf<XboxInput, Source>()
    XboxInput.entries.forEachIndexed { i, input ->
      map[input] = stored?.getOrNull(i)?.let(Source::fromSdlCode) ?: input.defaultSource
    }
    return map
  }

  fun save(context: Context, map: Map<XboxInput, Source>) {
    val editor = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
    if (isDefault(map)) {
      editor.remove(KEY_TABLE)
    } else {
      editor.putString(KEY_TABLE, XboxInput.entries.joinToString(",") { map.getValue(it).sdlCode.toString() })
    }
    editor.apply()
  }

  fun isDefault(map: Map<XboxInput, Source>): Boolean =
    XboxInput.entries.all { map[it] == it.defaultSource }

  /**
   * The native table, or null for the default layout so xemu keeps using
   * the per-pad gamepad_mappings from xemu.toml.
   */
  fun toNativeTable(map: Map<XboxInput, Source>): IntArray? {
    if (isDefault(map)) return null
    return IntArray(XboxInput.entries.size) { map.getValue(XboxInput.entries[it]).sdlCode }
  }

  /**
   * Lists every Xbox input with its source. Tapping one waits for a press on
   * the controller, with a picker as the fallback for pads that report odd
   * keycodes. [onChanged] gets every edit, already saved.
   */
  fun showEditor(
    activity: Activity,
    onChanged: (Map<XboxInput, Source>) -> Unit,
    onDismiss: () -> Unit,
  ) {
    val map = load(activity)
    val listView = ListView(activity)
    val adapter = object : BaseAdapter() {
      override fun getCount() = XboxInput.entries.size
      override fun getItem(position: Int) = XboxInput.entries[position]
      override fun getItemId(position: Int) = position.toLong()
      override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
        val view = convertView ?: activity.layoutInflater.inflate(
          android.R.layout.simple_list_item_2, parent, false
        )
        val input = getItem(position)
        val source = map.getValue(input)
        view.findViewById<TextView>(android.R.id.text1).text = input.label
        view.findViewById<TextView>(android.R.id.text2).text =
          if (source == input.defaultSource) source.label
          else activity.getString(R.string.remap_changed_source, source.label)
        return view
      }
    }
    listView.adapter = adapter

    fun commit() {
      save(activity, map)
      adapter.notifyDataSetChanged()
      onChanged(map)
    }

    listView.setOnItemClickListener { _, _, position, _ ->
      val input = XboxInput.entries[position]
      showCaptureDialog(activity, input) { source ->
        map[input] = source
        commit()
      }
    }

    MaterialAlertDialogBuilder(activity, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(R.string.remap_title)
      .setView(listView)
      .setPositiveButton(R.string.remap_done, null)
      .setNeutralButton(R.string.remap_reset, null)
      .setOnDismissListener { onDismiss() }
      .create()
      .also { dialog ->
        dialog.setOnShowListener {
          // Keep the dialog open on reset so the user sees the result.
          dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener {
            XboxInput.entries.forEach { map[it] = it.defaultSource }
            commit()
          }
        }
        dialog.show()
      }
  }

  private fun showCaptureDialog(activity: Activity, input: XboxInput, onPicked: (Source) -> Unit) {
    lateinit var dialog: AlertDialog
    var done = false
    fun pick(source: Source) {
      if (done) return
      done = true
      onPicked(source)
      dialog.dismiss()
    }

    dialog = MaterialAlertDialogBuilder(activity, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(activity.getString(R.string.remap_capture_title, input.label))
      .setMessage(R.string.remap_capture_message)
      .setNegativeButton(android.R.string.cancel, null)
      .setNeutralButton(R.string.remap_unbind) { _, _ -> pick(Source.NONE) }
      .setPositiveButton(R.string.remap_choose) { _, _ ->
        showSourcePicker(activity, input, ::pick)
      }
      .create()

    // Gamepad presses land on the focused dialog, not the game, so nothing
    // leaks into the emulator while capturing. Swallow every pad key so B or
    // Back bind instead of closing the dialog.
    dialog.setOnKeyListener { _, keyCode, event ->
      if (!isGamepadEvent(event)) return@setOnKeyListener false
      if (event.action == KeyEvent.ACTION_UP && !done) {
        Source.fromKeyCode(keyCode)?.let { pick(it) }
      }
      true
    }
    dialog.setOnShowListener {
      // Analog triggers that do not also send L2/R2 keys.
      dialog.window?.decorView?.setOnGenericMotionListener { _, event ->
        if ((event.source and InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK) {
          return@setOnGenericMotionListener false
        }
        val left = maxOf(
          event.getAxisValue(MotionEvent.AXIS_LTRIGGER),
          event.getAxisValue(MotionEvent.AXIS_BRAKE)
        )
        val right = maxOf(
          event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
          event.getAxisValue(MotionEvent.AXIS_GAS)
        )
        when {
          left > 0.6f -> pick(Source.LEFT_TRIGGER)
          right > 0.6f -> pick(Source.RIGHT_TRIGGER)
        }
        true
      }
    }
    dialog.show()
  }

  private fun showSourcePicker(activity: Activity, input: XboxInput, onPicked: (Source) -> Unit) {
    val sources = Source.entries
    MaterialAlertDialogBuilder(activity, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(activity.getString(R.string.remap_capture_title, input.label))
      .setItems(sources.map { it.label }.toTypedArray()) { _, which -> onPicked(sources[which]) }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun isGamepadEvent(event: KeyEvent): Boolean {
    val source = event.source
    return (source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
      (source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK ||
      KeyEvent.isGamepadButton(event.keyCode)
  }
}

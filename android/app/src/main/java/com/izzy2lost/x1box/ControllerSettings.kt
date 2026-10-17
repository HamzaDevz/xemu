package com.izzy2lost.x1box

import android.content.Context
import android.content.SharedPreferences

class ControllerSettings(context: Context) {
  private val prefs: SharedPreferences = context.getSharedPreferences(
    "controller_settings",
    Context.MODE_PRIVATE
  )

  companion object {
    private const val KEY_SHOW_ON_SCREEN_CONTROLLER = "show_on_screen_controller"
    private const val KEY_CONTROLLER_OPACITY = "controller_opacity"
    private const val KEY_CONTROLLER_SCALE = "controller_scale"
    private const val KEY_VIBRATION_ENABLED = "vibration_enabled"
    private const val KEY_TOUCH_LAYOUT = "touch_layout"
  }

  var showOnScreenController: Boolean
    get() = prefs.getBoolean(KEY_SHOW_ON_SCREEN_CONTROLLER, true)
    set(value) = prefs.edit().putBoolean(KEY_SHOW_ON_SCREEN_CONTROLLER, value).apply()

  var controllerOpacity: Float
    get() = prefs.getFloat(KEY_CONTROLLER_OPACITY, 1.0f)
    set(value) = prefs.edit().putFloat(KEY_CONTROLLER_OPACITY, value.coerceIn(0f, 1f)).apply()

  var controllerScale: Float
    get() = prefs.getFloat(KEY_CONTROLLER_SCALE, 1.0f)
    set(value) = prefs.edit().putFloat(KEY_CONTROLLER_SCALE, value.coerceIn(0.5f, 2.0f)).apply()

  var vibrationEnabled: Boolean
    get() = prefs.getBoolean(KEY_VIBRATION_ENABLED, true)
    set(value) = prefs.edit().putBoolean(KEY_VIBRATION_ENABLED, value).apply()

  /** Per-element placement from the layout editor; missing elements use the default. */
  var touchLayout: Map<OnScreenController.Element, OnScreenController.ElementLayout>
    get() {
      val raw = prefs.getString(KEY_TOUCH_LAYOUT, null) ?: return emptyMap()
      return raw.split(';').mapNotNull { entry ->
        val f = entry.split(':')
        if (f.size != 5) return@mapNotNull null
        val element = OnScreenController.Element.entries.firstOrNull { it.name == f[0] }
          ?: return@mapNotNull null
        val x = f[1].toFloatOrNull() ?: return@mapNotNull null
        val y = f[2].toFloatOrNull() ?: return@mapNotNull null
        val scale = f[3].toFloatOrNull() ?: return@mapNotNull null
        element to OnScreenController.ElementLayout(x, y, scale, f[4] == "1")
      }.toMap()
    }
    set(value) {
      val raw = value.entries.joinToString(";") { (e, l) ->
        "${e.name}:${l.centerX}:${l.centerY}:${l.scale}:${if (l.visible) 1 else 0}"
      }
      prefs.edit().apply {
        if (raw.isEmpty()) remove(KEY_TOUCH_LAYOUT) else putString(KEY_TOUCH_LAYOUT, raw)
      }.apply()
    }
}

package com.izzy2lost.x1box

import android.app.Activity
import android.content.Context
import android.content.Intent
import com.izzy2lost.x1box.hakux.HakuxCoreActivity

/**
 * Which core runs a game: xemu (the whole console under QEMU, :xemu) or the
 * hakuX core (an HLE Xbox kernel on FEX with an HLE GPU, android/hakux, in
 * :hakux). The global choice is PREF_CORE; a game's own override comes in as
 * its runtime override, like every other per-game setting.
 */
object HakuxLaunch {
  const val PREF_CORE = "setting_core"
  const val CORE_XEMU = "xemu"
  const val CORE_HAKUX = "hakux"

  /** The core for the game whose runtime overrides were just applied (startGame). */
  fun selectedCore(context: Context): String {
    val prefs = context.getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE)
    return PerGameSettingsManager.getRuntimeOverride(context, PREF_CORE)
      ?: prefs.getString(PREF_CORE, CORE_XEMU)
      ?: CORE_XEMU
  }

  /** Starts the game at dvdUri in the hakuX core, the library's process handed off as for xemu. */
  fun launch(activity: Activity, dvdUri: String) {
    val intent = Intent(activity, HakuxCoreActivity::class.java)
      .putExtra(HakuxCoreActivity.EXTRA_DVD_URI, dvdUri)
    EmulationProcessHandoff.launch(activity, intent)
  }
}

package com.izzy2lost.x1box

import android.app.Activity
import android.app.ActivityManager
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Process
import android.widget.Toast
import java.io.File

class LauncherActivity : Activity() {
  companion object {
    private const val TAG = "LauncherActivity"
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    DebugLog.initialize(this)
    OrientationLocker(this).enable()

    /* Runs here, not only in Settings, so existing installs get it even if
     * the user never opens the settings screen. */
    SettingsActivity.applyDrawReorderOffMigration(this)

    val prefs = getSharedPreferences("x1box_prefs", MODE_PRIVATE)
    var setupComplete = prefs.getBoolean("setup_complete", false)
    val mcpxUriStr = prefs.getString("mcpxUri", null)
    val flashUriStr = prefs.getString("flashUri", null)
    val hddUriStr = prefs.getString("hddUri", null)
    val dvdUriStr = prefs.getString("dvdUri", null)
    val gamesFolderUriStr = prefs.getString("gamesFolderUri", null)
    val mcpxPath = prefs.getString("mcpxPath", null)
    val flashPath = prefs.getString("flashPath", null)
    val hddPath = prefs.getString("hddPath", null)
    val dvdPath = prefs.getString("dvdPath", null)

    val mcpxUri = mcpxUriStr?.let(Uri::parse)
    val flashUri = flashUriStr?.let(Uri::parse)
    val hddUri = hddUriStr?.let(Uri::parse)
    val dvdUri = dvdUriStr?.let(Uri::parse)
    val gamesFolderUri = gamesFolderUriStr?.let(Uri::parse)
    val frontendLaunch = FrontendLaunchHelper.resolve(this, intent, gamesFolderUri)

    val hasMcpx = hasLocalFile(mcpxPath) || (mcpxUri != null && hasPersistedReadPermission(mcpxUri))
    val hasFlash = hasLocalFile(flashPath) || (flashUri != null && hasPersistedReadPermission(flashUri))
    val hasHdd = hasLocalFile(hddPath) || (hddUri != null && hasPersistedReadPermission(hddUri))
    val hasDvd = hasLocalFile(dvdPath) || (dvdUri != null && hasPersistedReadPermission(dvdUri))
    val hasGamesFolder = gamesFolderUri != null && hasPersistedReadPermission(gamesFolderUri)

    val editor = prefs.edit()
    var clearedCore = false
    var clearedOptional = false
    if (!hasMcpx && mcpxUriStr != null) {
      editor.remove("mcpxUri")
      clearedCore = true
    }
    if (!hasFlash && flashUriStr != null) {
      editor.remove("flashUri")
      clearedCore = true
    }
    if (!hasHdd && hddUriStr != null) {
      editor.remove("hddUri")
      clearedCore = true
    }
    if (!hasDvd && dvdUriStr != null) {
      editor.remove("dvdUri")
      clearedOptional = true
    }
    if (!hasMcpx && mcpxPath != null) {
      editor.remove("mcpxPath")
      clearedCore = true
    }
    if (!hasFlash && flashPath != null) {
      editor.remove("flashPath")
      clearedCore = true
    }
    if (!hasHdd && hddPath != null) {
      editor.remove("hddPath")
      clearedCore = true
    }
    if (!hasDvd && dvdPath != null) {
      editor.remove("dvdPath")
      clearedOptional = true
    }
    if (!hasGamesFolder && gamesFolderUriStr != null) {
      editor.remove("gamesFolderUri")
      clearedCore = true
    }
    if (clearedCore) {
      setupComplete = false
      editor.putBoolean("setup_complete", false)
      editor.putBoolean("skip_game_picker", false)
      editor.apply()
    } else if (clearedOptional) {
      editor.apply()
    }

    if (frontendLaunch != null) {
      // Frontends launch with NEW_TASK only, so this also runs while a game is
      // still open: asking for that same game just returns to it.
      val sameGame = (frontendLaunch.dvdUri != null && frontendLaunch.dvdUri.toString() == dvdUriStr) ||
        (frontendLaunch.dvdPath != null && frontendLaunch.dvdPath == dvdPath)
      if (sameGame && emulatorProcesses().any { it.processName == "$packageName:xemu" }) {
        finish()
        return
      }
      if (frontendLaunch.dvdUri != null) {
        FrontendLaunchHelper.persistReadPermission(this, intent, frontendLaunch.dvdUri)
      }
      // MainActivity runs in :xemu, so launch data must be flushed before
      // handing off to the emulator process.
      val launchEditor = prefs.edit()
      launchEditor.putBoolean("skip_game_picker", false)
      PerGameSettingsManager.applyRuntimeOverridesToEditor(
        context = this,
        editor = launchEditor,
        relativePath = frontendLaunch.relativePath,
      )
      when {
        frontendLaunch.dvdUri != null -> {
          launchEditor.putString("dvdUri", frontendLaunch.dvdUri.toString())
          launchEditor.remove("dvdPath")
        }
        frontendLaunch.dvdPath != null -> {
          launchEditor.putString("dvdPath", frontendLaunch.dvdPath)
          launchEditor.remove("dvdUri")
        }
      }
      launchEditor.commit()

      if (hasMcpx && hasFlash && hasHdd) {
        DebugLog.i(TAG) { "Frontend launch resolved via ${frontendLaunch.source}" }
        // A core runs one game per process: stop any running one so the new game
        // starts in a fresh process instead of landing in the old session.
        stopEmulatorProcesses()
        EmulationProcessHandoff.launch(
          this,
          Intent(this, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK),
        )
        return
      }

      DebugLog.i(TAG) { "Frontend launch queued, but core setup is incomplete" }
      Toast.makeText(this, R.string.frontend_launch_setup_required, Toast.LENGTH_SHORT).show()
    } else if (hasExternalLaunchPayload(intent)) {
      DebugLog.w(TAG) { "Frontend intent received but no accessible game target was resolved" }
      Toast.makeText(this, R.string.frontend_launch_unresolved, Toast.LENGTH_LONG).show()
    }

    val needsSetup = !setupComplete || !hasMcpx || !hasFlash || !hasHdd || !hasGamesFolder
    val next = if (needsSetup) SetupWizardActivity::class.java else GameLibraryActivity::class.java

    startActivity(Intent(this, next))
    finish()
  }

  private fun emulatorProcesses(): List<ActivityManager.RunningAppProcessInfo> {
    val names = setOf("$packageName:xemu", "$packageName:hakux")
    return getSystemService(ActivityManager::class.java).runningAppProcesses
      .orEmpty()
      .filter { it.processName in names }
  }

  private fun stopEmulatorProcesses() {
    val running = emulatorProcesses()
    if (running.isEmpty()) {
      return
    }
    running.forEach { Process.killProcess(it.pid) }
    // Wait for the system to see them gone, so the game is not started in a dying process.
    repeat(50) {
      if (emulatorProcesses().isEmpty()) {
        return
      }
      Thread.sleep(20)
    }
  }

  private fun hasPersistedReadPermission(uri: Uri): Boolean {
    return contentResolver.persistedUriPermissions.any { perm ->
      perm.uri == uri && perm.isReadPermission
    }
  }

  private fun hasLocalFile(path: String?): Boolean {
    return path != null && File(path).isFile
  }

  private fun hasExternalLaunchPayload(intent: Intent?): Boolean {
    if (intent == null) {
      return false
    }
    if (intent.data != null || intent.clipData != null) {
      return true
    }
    return sequenceOf(
      Intent.EXTRA_STREAM,
      "rom",
      "ROM",
      "path",
      "PATH",
      "file",
      "FILE",
      "filename",
      "FILENAME",
      "romPath",
      "ROM_PATH",
      "uri",
      "URI",
    ).any { key -> intent.hasExtra(key) }
  }
}

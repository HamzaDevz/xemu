/*
 * Adapted from hakuX's SaveStates and SaveStateStore (https://github.com/rfandango/hakuX,
 * GPLv3 or later; see android/hakux/core/COPYING).
 */
package com.izzy2lost.x1box.hakux

import android.app.AlertDialog
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import com.izzy2lost.x1box.R
import java.io.File
import java.security.MessageDigest
import java.text.DateFormat
import java.util.Date

/**
 * A game's hakuX savestates: ten slots, each the core's state file (hakux/core/native/state,
 * with what the game's cache drive held) and the frame on screen when it was saved, kept
 * per game by its disc.
 */
class HakuxStateStore(context: Context, gameKey: String) {
  private val dir = File(context.filesDir, "hakux/states/${hash(gameKey)}")

  fun state(slot: Int) = File(dir, "slot$slot.hxs")
  fun thumb(slot: Int) = File(dir, "slot$slot.jpg")

  /** The slots holding a state. */
  fun saved(): List<Int> = (1..SLOTS).filter { state(it).isFile }

  fun prepare() {
    dir.mkdirs()
  }

  /** A slot's state and frame go; the cache copies (blobs/) with the game's last state. */
  fun delete(slot: Int) {
    state(slot).delete()
    thumb(slot).delete()
    if (saved().isEmpty()) File(dir, "blobs").deleteRecursively()
  }

  companion object {
    const val SLOTS = 10

    private fun hash(s: String): String =
      MessageDigest.getInstance("SHA-1").digest(s.toByteArray()).take(8).joinToString("") { "%02x".format(it) }
  }
}

/**
 * The menu's Save state and Load state. Saving goes through the core (nativeSaveState,
 * off the UI thread); loading starts the core again into the state (restartInto), with the
 * state's frame held meanwhile. Load state's list deletes states too.
 */
class HakuxSaveStates(
  private val activity: HakuxCoreActivity,
  gameKey: String,
  /** Dialogs the game stays stopped under (HakuxCoreActivity.showMenuDialog). */
  private val showDialog: (AlertDialog.Builder) -> AlertDialog,
  /** The frame on screen (null: none could be copied). */
  private val captureFrame: ((Bitmap?) -> Unit) -> Unit,
  /** Starts the core again into the state at this path. */
  private val restartInto: (String) -> Unit,
) {
  private val store = HakuxStateStore(activity, gameKey)

  fun showSlots(save: Boolean) {
    val slots = if (save) (1..HakuxStateStore.SLOTS).toMutableList() else store.saved().toMutableList()
    if (slots.isEmpty()) {
      Toast.makeText(activity, R.string.hakux_state_none, Toast.LENGTH_SHORT).show()
      return
    }
    val adapter = SlotAdapter(slots, deletable = !save)
    adapter.dialog = showDialog(
      AlertDialog.Builder(activity)
        .setTitle(if (save) R.string.hakux_menu_save_state else R.string.hakux_menu_load_state)
        .setAdapter(adapter) { _, i -> if (save) save(slots[i]) else load(slots[i]) }
        .setNegativeButton(android.R.string.cancel, null)
    )
  }

  private fun busy(text: String): AlertDialog {
    val row = LinearLayout(activity).apply {
      orientation = LinearLayout.HORIZONTAL
      gravity = Gravity.CENTER_VERTICAL
      setPadding(dp(24), dp(20), dp(24), dp(20))
      addView(ProgressBar(activity))
      addView(TextView(activity).apply {
        this.text = text
        setPadding(dp(16), 0, 0, 0)
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
      })
    }
    return showDialog(AlertDialog.Builder(activity).setView(row).setCancelable(false))
  }

  private fun save(slot: Int) {
    store.prepare()
    val progress = busy(activity.getString(R.string.hakux_state_saving, slot))
    Thread {
      val err = HakuxCoreActivity.nativeSaveState(store.state(slot).absolutePath)
      activity.runOnUiThread {
        if (err != null) {
          progress.dismiss()
          showDialog(
            AlertDialog.Builder(activity)
              .setTitle(R.string.hakux_state_save_failed)
              .setMessage(err)
              .setPositiveButton(android.R.string.ok, null)
          )
          return@runOnUiThread
        }
        captureFrame { frame ->
          Thread {
            if (frame != null) {
              runCatching { store.thumb(slot).outputStream().use { frame.compress(Bitmap.CompressFormat.JPEG, 90, it) } }
            }
            activity.runOnUiThread {
              progress.dismiss()
              Toast.makeText(activity, activity.getString(R.string.hakux_state_saved, slot), Toast.LENGTH_SHORT).show()
            }
          }.start()
        }
      }
    }.start()
  }

  private fun load(slot: Int) {
    val file = store.state(slot)
    Thread {
      val err = HakuxCoreActivity.nativeStateCheck(file.absolutePath)
      activity.runOnUiThread {
        if (err != null) {
          showDialog(
            AlertDialog.Builder(activity)
              .setTitle(R.string.hakux_state_load_failed)
              .setMessage(err)
              .setPositiveButton(android.R.string.ok, null)
          )
          return@runOnUiThread
        }
        val held = HakuxRelaunchActivity.heldFrame(activity)
        runCatching { store.thumb(slot).copyTo(held, overwrite = true) }.onFailure { held.delete() }
        restartInto(file.absolutePath)
      }
    }.start()
  }

  private fun confirmDelete(slot: Int, onDeleted: () -> Unit) {
    showDialog(
      AlertDialog.Builder(activity)
        .setTitle(R.string.hakux_state_delete_title)
        .setMessage(activity.getString(R.string.hakux_state_delete_message, slot))
        .setPositiveButton(R.string.hakux_state_delete) { _, _ ->
          store.delete(slot)
          onDeleted()
        }
        .setNegativeButton(android.R.string.cancel, null)
    )
  }

  private fun dp(v: Int) =
    TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), activity.resources.displayMetrics).toInt()

  /** A slot: its frame, its number and when it was saved (or empty); Load state's with a delete button. */
  private inner class SlotAdapter(private val slots: MutableList<Int>, private val deletable: Boolean) : BaseAdapter() {
    private val format = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT)
    var dialog: AlertDialog? = null

    override fun getCount() = slots.size
    override fun getItem(i: Int) = slots[i]
    override fun getItemId(i: Int) = slots[i].toLong()

    override fun getView(i: Int, convert: View?, parent: ViewGroup?): View {
      val slot = slots[i]
      val pad = dp(8)
      val row = LinearLayout(activity).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setPadding(dp(16), pad, dp(8), pad)
      }
      row.addView(ImageView(activity).apply {
        layoutParams = LinearLayout.LayoutParams(dp(96), dp(72))
        scaleType = ImageView.ScaleType.CENTER_CROP
        setBackgroundColor(Color.DKGRAY)
        store.thumb(slot).takeIf { it.exists() }?.let { f ->
          BitmapFactory.decodeFile(f.path, BitmapFactory.Options().apply { inSampleSize = 4 })?.let(::setImageBitmap)
        }
      })
      val file = store.state(slot)
      row.addView(TextView(activity).apply {
        layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        setPadding(dp(16), 0, 0, 0)
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
        text = activity.getString(R.string.hakux_state_slot, slot) + "\n" +
          if (file.exists()) format.format(Date(file.lastModified())) else activity.getString(R.string.hakux_state_empty)
      })
      if (deletable) {
        row.addView(ImageView(activity).apply {
          setImageResource(android.R.drawable.ic_menu_delete)
          contentDescription = activity.getString(R.string.hakux_state_delete)
          setPadding(pad, pad, pad, pad)
          isFocusable = false
          setOnClickListener {
            confirmDelete(slot) {
              slots.remove(slot)
              if (slots.isEmpty()) dialog?.dismiss() else notifyDataSetChanged()
            }
          }
        })
      }
      return row
    }
  }
}

package com.izzy2lost.x1box

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import java.io.BufferedInputStream
import java.io.Closeable
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * Game saves on the HDD image's E: partition, read and written in place
 * through the FATX code in xemu_fatx_import.c.
 *
 * The dashboard's layout: E:\UDATA\<title id>\ holds a game's saves, one
 * folder each, beside TitleMeta.xbx (the game's name) and TitleImage.xbx (its
 * icon). Each save folder has SaveMeta.xbx (the save's name) and optionally
 * saveimage.xbx. E:\TDATA\<title id>\ is the game's own scratch data.
 *
 * Exports are ZIPs rooted at UDATA/ (and TDATA/ for a whole game), so an
 * export imports back as is and matches what FTP tools pull off a console.
 *
 * The emulator runs in the :xemu process, which exits when it returns to the
 * library, so nothing else has the image open while this runs.
 */
internal object XboxSaveManager {

  class SaveGame(
    val titleId: String,
    val folder: String,
    val name: String,
    val sizeBytes: Long,
    val modifiedMillis: Long,
    val icon: Bitmap?,
  ) {
    val fatxPath: String get() = "$UDATA/$titleId/$folder"
    val exportPath: String get() = "$titleId/$folder"
  }

  class SaveTitle(
    val titleId: String,
    val name: String,
    val icon: Bitmap?,
    val saves: List<SaveGame>,
    val sizeBytes: Long,
    val modifiedMillis: Long,
  ) {
    val fatxPath: String get() = "$UDATA/$titleId"
  }

  class Inventory(val titles: List<SaveTitle>, val totalBytes: Long, val freeBytes: Long)

  private const val UDATA = "/UDATA"
  private const val TDATA = "/TDATA"
  private const val TITLE_META = "TitleMeta.xbx"
  private const val TITLE_IMAGE = "TitleImage.xbx"
  private const val SAVE_META = "SaveMeta.xbx"
  private const val SAVE_IMAGE = "saveimage.xbx"
  private const val MAX_META_BYTES = 64 * 1024
  private const val MAX_IMAGE_BYTES = 256 * 1024
  /* Generous for saves, but stops a wrong ZIP from filling the device. */
  private const val MAX_IMPORT_BYTES = 2L * 1024 * 1024 * 1024
  private val TITLE_ID_PATTERN = Regex("^[0-9A-Fa-f]{8}$")

  private class Entry(val name: String, val isDirectory: Boolean, val size: Long, val modifiedMillis: Long)

  private class Volume(hddFile: File, writable: Boolean) : Closeable {
    private var handle = NativeBridge.nativeOpen(hddFile.absolutePath, writable)

    fun space(): LongArray = NativeBridge.nativeSpace(handle)

    /** Null when the folder doesn't exist. */
    fun list(path: String, directorySizes: Boolean = false): List<Entry>? {
      val lines = NativeBridge.nativeList(handle, path, directorySizes) ?: return null
      return lines.mapNotNull { line ->
        val fields = line.split('\t', limit = 4)
        if (fields.size < 4) {
          return@mapNotNull null
        }
        Entry(
          name = fields[3],
          isDirectory = fields[0] == "D",
          size = fields[1].toLongOrNull() ?: 0L,
          modifiedMillis = (fields[2].toLongOrNull() ?: 0L) * 1000L,
        )
      }
    }

    fun read(path: String): ByteArray = NativeBridge.nativeReadFile(handle, path)

    fun export(fatxPath: String, hostPath: File) =
      NativeBridge.nativeExport(handle, fatxPath, hostPath.absolutePath)

    fun import(hostDir: File, fatxDir: String) =
      NativeBridge.nativeImport(handle, hostDir.absolutePath, fatxDir)

    fun remove(path: String) = NativeBridge.nativeRemove(handle, path)

    override fun close() {
      val open = handle
      handle = 0L
      NativeBridge.nativeClose(open)
    }
  }

  /** Reads every game's saves. [knownTitles] names games whose TitleMeta.xbx is missing. */
  @Throws(IOException::class)
  fun scan(hddFile: File, knownTitles: Map<Long, XbeMetadataCache.Entry> = emptyMap()): Inventory {
    requireHdd(hddFile)
    Volume(hddFile, writable = false).use { volume ->
      val space = volume.space()
      val titles = volume.list(UDATA, directorySizes = true).orEmpty()
        .filter { it.isDirectory && TITLE_ID_PATTERN.matches(it.name) }
        .map { entry -> readTitle(volume, entry, knownTitles) }
        .sortedBy { it.name.lowercase(Locale.ROOT) }
      return Inventory(titles, space[0], space[1])
    }
  }

  private fun readTitle(
    volume: Volume,
    titleEntry: Entry,
    knownTitles: Map<Long, XbeMetadataCache.Entry>,
  ): SaveTitle {
    val titleId = titleEntry.name.uppercase(Locale.ROOT)
    val titlePath = "$UDATA/${titleEntry.name}"
    val children = volume.list(titlePath, directorySizes = true).orEmpty()
    val known = titleId.toLongOrNull(16)?.let { knownTitles[it] }

    val name = children.findFile(TITLE_META)
      ?.let { readMetaValue(volume, "$titlePath/${it.name}", "TitleName") }
      ?: known?.titleName?.takeIf { it.isNotBlank() }
      ?: titleId
    val icon = children.findFile(TITLE_IMAGE)
      ?.let { readImage(volume, "$titlePath/${it.name}") }
      ?: known?.icon?.let { file -> runCatching { BitmapFactory.decodeFile(file.absolutePath) }.getOrNull() }
    // The game's default icon for saves that don't carry their own.
    val defaultSaveIcon = children.findFile(SAVE_IMAGE)
      ?.let { readImage(volume, "$titlePath/${it.name}") }
      ?: icon

    val saves = children.filter { it.isDirectory }.map { saveEntry ->
      val savePath = "$titlePath/${saveEntry.name}"
      val files = volume.list(savePath).orEmpty()
      SaveGame(
        titleId = titleEntry.name,
        folder = saveEntry.name,
        name = files.findFile(SAVE_META)
          ?.let { readMetaValue(volume, "$savePath/${it.name}", "Name") }
          ?: saveEntry.name,
        sizeBytes = saveEntry.size,
        // A folder's own date isn't touched when the game rewrites a file in it.
        modifiedMillis = (files.map { it.modifiedMillis } + saveEntry.modifiedMillis).max(),
        icon = files.findFile(SAVE_IMAGE)?.let { readImage(volume, "$savePath/${it.name}") } ?: defaultSaveIcon,
      )
    }.sortedByDescending { it.modifiedMillis }

    return SaveTitle(
      titleId = titleEntry.name,
      name = name,
      icon = icon,
      saves = saves,
      sizeBytes = titleEntry.size,
      modifiedMillis = (saves.map { it.modifiedMillis } + titleEntry.modifiedMillis).max(),
    )
  }

  private fun List<Entry>.findFile(name: String): Entry? =
    firstOrNull { !it.isDirectory && it.name.equals(name, ignoreCase = true) }

  /*
   * The .xbx metadata files are UTF-16LE "Key=Value" lines; localized copies
   * follow under [xx] section headers, so the first match is the default.
   */
  private fun readMetaValue(volume: Volume, path: String, key: String): String? {
    val bytes = runCatching { volume.read(path) }.getOrNull() ?: return null
    if (bytes.isEmpty() || bytes.size > MAX_META_BYTES) {
      return null
    }
    val text = String(bytes, Charsets.UTF_16LE).removePrefix("\uFEFF")
    return text.lineSequence()
      .map { it.trim() }
      .firstOrNull { it.startsWith("$key=", ignoreCase = true) }
      ?.substringAfter('=')
      ?.trim()
      ?.takeIf { it.isNotEmpty() }
  }

  private fun readImage(volume: Volume, path: String): Bitmap? {
    val bytes = runCatching { volume.read(path) }.getOrNull() ?: return null
    if (bytes.size > MAX_IMAGE_BYTES) {
      return null
    }
    return runCatching { XbeMetadataReader.decodeTitleImage(bytes) }.getOrNull()
  }

  /**
   * Writes whole games ([titleIds]) and single saves ([savePaths], each
   * "<title id>/<folder>") into one ZIP. A lone save carries its game's
   * TitleMeta.xbx and TitleImage.xbx so it imports with a name.
   */
  @Throws(IOException::class)
  fun exportZip(
    hddFile: File,
    titleIds: Collection<String>,
    savePaths: Collection<String>,
    output: OutputStream,
    workDir: File,
  ) {
    requireHdd(hddFile)
    val stagingRoot = createCleanDir(workDir)
    try {
      Volume(hddFile, writable = false).use { volume ->
        for (titleId in titleIds) {
          volume.export("$UDATA/$titleId", File(stagingRoot, "UDATA/$titleId"))
          if (volume.list("$TDATA/$titleId") != null) {
            volume.export("$TDATA/$titleId", File(stagingRoot, "TDATA/$titleId"))
          }
        }
        for (savePath in savePaths) {
          val titleId = savePath.substringBefore('/')
          val folder = savePath.substringAfter('/', "")
          if (folder.isEmpty() || titleIds.any { it.equals(titleId, ignoreCase = true) }) {
            continue
          }
          val titleDir = File(stagingRoot, "UDATA/$titleId")
          volume.export("$UDATA/$titleId/$folder", File(titleDir, folder))
          val titleFiles = volume.list("$UDATA/$titleId").orEmpty()
          for (meta in listOf(TITLE_META, TITLE_IMAGE)) {
            val file = titleFiles.findFile(meta) ?: continue
            volume.export("$UDATA/$titleId/${file.name}", File(titleDir, file.name))
          }
        }
      }
      ZipOutputStream(output).use { zip -> zipTree(stagingRoot, "", zip) }
    } finally {
      stagingRoot.deleteRecursively()
    }
  }

  private fun zipTree(dir: File, prefix: String, zip: ZipOutputStream) {
    val children = dir.listFiles().orEmpty().sortedBy { it.name }
    if (children.isEmpty() && prefix.isNotEmpty()) {
      zip.putNextEntry(ZipEntry(prefix))
      zip.closeEntry()
      return
    }
    for (child in children) {
      val name = prefix + child.name
      if (child.isDirectory) {
        zipTree(child, "$name/", zip)
      } else {
        zip.putNextEntry(ZipEntry(name).apply { time = child.lastModified() })
        child.inputStream().use { it.copyTo(zip) }
        zip.closeEntry()
      }
    }
  }

  /**
   * Merges a ZIP's saves into the HDD, replacing files that already exist.
   * Accepts UDATA/<title id>/… at any depth, or bare <title id>/… folders.
   * Returns the title ids imported.
   */
  @Throws(IOException::class)
  fun importZip(hddFile: File, input: InputStream, workDir: File): List<String> {
    requireHdd(hddFile)
    val extractRoot = createCleanDir(workDir)
    try {
      extractZip(input, extractRoot)
      val udata = findDataRoot(extractRoot, "UDATA")
        ?: extractRoot.takeIf { titleDirs(it).isNotEmpty() }
        ?: extractRoot.listFiles()?.singleOrNull { it.isDirectory }
          ?.takeIf { titleDirs(it).isNotEmpty() }
      val titleDirs = udata?.let(::titleDirs).orEmpty()
      if (titleDirs.isEmpty()) {
        throw IOException(
          "No saves found. The ZIP needs UDATA/<title id>/ folders, or <title id>/ folders " +
            "named with the game's 8-character title id."
        )
      }
      val tdata = udata?.parentFile?.let { parent ->
        parent.listFiles()?.firstOrNull { it.isDirectory && it.name.equals("TDATA", ignoreCase = true) }
      }

      Volume(hddFile, writable = true).use { volume ->
        for (dir in titleDirs) {
          volume.import(dir, "$UDATA/${dir.name.lowercase(Locale.ROOT)}")
        }
        tdata?.let(::titleDirs)?.forEach { dir ->
          volume.import(dir, "$TDATA/${dir.name.lowercase(Locale.ROOT)}")
        }
      }
      return titleDirs.map { it.name.lowercase(Locale.ROOT) }
    } finally {
      extractRoot.deleteRecursively()
    }
  }

  private fun titleDirs(dir: File): List<File> =
    dir.listFiles().orEmpty().filter { it.isDirectory && TITLE_ID_PATTERN.matches(it.name) }

  private fun findDataRoot(dir: File, name: String, depth: Int = 0): File? {
    val children = dir.listFiles().orEmpty().filter { it.isDirectory }
    children.firstOrNull { it.name.equals(name, ignoreCase = true) }?.let { return it }
    if (depth >= 3) {
      return null
    }
    return children.firstNotNullOfOrNull { findDataRoot(it, name, depth + 1) }
  }

  private fun extractZip(input: InputStream, targetDir: File) {
    val rootPath = targetDir.canonicalPath + File.separator
    var totalBytes = 0L
    ZipInputStream(BufferedInputStream(input)).use { zip ->
      while (true) {
        val entry = zip.nextEntry ?: break
        val name = entry.name.replace('\\', '/')
        if (name.isBlank() || name.split('/').any { it == "__MACOSX" }) {
          continue
        }
        val outFile = File(targetDir, name).canonicalFile
        if (!outFile.path.startsWith(rootPath)) {
          throw IOException("The ZIP contains an invalid path: $name")
        }
        if (entry.isDirectory) {
          outFile.mkdirs()
          continue
        }
        outFile.parentFile?.mkdirs()
        outFile.outputStream().use { out ->
          val buffer = ByteArray(64 * 1024)
          while (true) {
            val read = zip.read(buffer)
            if (read < 0) {
              break
            }
            totalBytes += read
            if (totalBytes > MAX_IMPORT_BYTES) {
              throw IOException("The ZIP is too large to be a save backup.")
            }
            out.write(buffer, 0, read)
          }
        }
        if (entry.time > 0) {
          outFile.setLastModified(entry.time)
        }
      }
    }
  }

  /** Deletes a save. */
  @Throws(IOException::class)
  fun delete(hddFile: File, save: SaveGame) {
    requireHdd(hddFile)
    Volume(hddFile, writable = true).use { volume -> volume.remove(save.fatxPath) }
  }

  /** Deletes all of a game's saves and its TDATA scratch data. */
  @Throws(IOException::class)
  fun delete(hddFile: File, title: SaveTitle) {
    requireHdd(hddFile)
    Volume(hddFile, writable = true).use { volume ->
      volume.remove(title.fatxPath)
      if (volume.list("$TDATA/${title.titleId}") != null) {
        volume.remove("$TDATA/${title.titleId}")
      }
    }
  }

  private fun requireHdd(hddFile: File) {
    if (!hddFile.isFile) {
      throw IOException("No local HDD image is configured.")
    }
  }

  private fun createCleanDir(dir: File): File {
    dir.deleteRecursively()
    if (!dir.mkdirs()) {
      throw IOException("Failed to prepare ${dir.absolutePath}")
    }
    return dir
  }

  private object NativeBridge {
    init {
      System.loadLibrary("SDL3")
      System.loadLibrary("xemu")
    }

    external fun nativeOpen(hddPath: String, writable: Boolean): Long
    external fun nativeClose(handle: Long)
    external fun nativeSpace(handle: Long): LongArray
    external fun nativeList(handle: Long, path: String, directorySizes: Boolean): Array<String>?
    external fun nativeReadFile(handle: Long, path: String): ByteArray
    external fun nativeExport(handle: Long, fatxPath: String, hostPath: String)
    external fun nativeImport(handle: Long, hostDir: String, fatxDir: String)
    external fun nativeRemove(handle: Long, path: String)
  }
}

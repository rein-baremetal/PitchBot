package com.pitchbot.app

import android.content.Context
import android.net.Uri
import android.os.Build
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.InputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * One-zip backup of everything PitchBot has learned.
 *
 * Contents: manifest.json, matches.json, logs/, versions.json, models/, tracks/, prefs.json.
 * Import is staged: the zip is unpacked into a temp folder, checked, and only then copied over the app's files,
 * so a bad or half-downloaded zip never leaves the app half-restored.
 *
 * "Learning only" skips the phone-specific setup (button mapping, radar box, kit colours, measured delay), which
 * is wrong on a phone with a different screen. Everything else (play style, learned weights, match history,
 * tracker numbers) comes across.
 */
object Backup {

    const val FORMAT = 2
    private const val MAX_TOTAL_BYTES = 600L * 1024 * 1024
    private const val SKIP_KEY = "lastBackupMs"

    data class Info(
        val format: Int,
        val created: Long,
        val device: String,
        val matches: Int,
        val learnedMatches: Int,
        val versions: Int,
        val activeVersion: Int,
        val recordings: Int
    ) {
        fun summary(): String =
            "$matches match${if (matches == 1) "" else "es"} ($learnedMatches learned from), " +
            "model v$activeVersion of $versions, $recordings recording${if (recordings == 1) "" else "s"}"
    }

    private val deviceExact = setOf(
        "mapped", "joyX", "joyY", "joyR", "thrX", "thrY", "passX", "passY", "shootX", "shootY", "dashX", "dashY",
        "radL", "radT", "radR", "radB", "roiPl", "roiPt", "roiPr", "roiPb", "roiCalibW", "roiCalibH",
        "olL", "olT", "olR", "olB", "radarResizePending", "latMeasured", "kitHome", "kitAway", "kitBall", "kitBn"
    )

    private fun isDeviceKey(k: String) =
        k in deviceExact || k.startsWith("hud") || (k.startsWith("kitB") && k.length > 4 && k[4].isDigit())

    private fun allowedName(n: String): Boolean {
        if (n.contains("..") || n.startsWith("/") || n.contains('\\')) return false
        if (n == "matches.json" || n == "versions.json" || n == "prefs.json" || n == "manifest.json") return true
        return n.startsWith("logs/") || n.startsWith("models/") || n.startsWith("tracks/")
    }

    private fun countInfo(matches: JSONArray?, versions: JSONObject?, recordings: Int, created: Long, device: String, fmt: Int): Info {
        var learned = 0
        if (matches != null) for (i in 0 until matches.length()) if (matches.getJSONObject(i).optBoolean("learned")) learned++
        return Info(fmt, created, device, matches?.length() ?: 0, learned,
            versions?.optJSONArray("versions")?.length() ?: 1, versions?.optInt("active", 0) ?: 0, recordings)
    }

    fun export(ctx: Context, uri: Uri): Info {
        val files = ctx.filesDir
        val prefs = Prefs(ctx)
        val mf = File(files, "matches.json")
        val vf = File(files, "versions.json")
        val matches = try { if (mf.exists()) JSONArray(mf.readText()) else null } catch (_: Exception) { null }
        val versions = try { if (vf.exists()) JSONObject(vf.readText()) else null } catch (_: Exception) { null }
        val tracks = File(files, "tracks").listFiles()?.filter { it.isFile } ?: emptyList()
        val now = System.currentTimeMillis()
        val info = countInfo(matches, versions, tracks.size, now, "${Build.MANUFACTURER} ${Build.MODEL}", FORMAT)

        val os = ctx.contentResolver.openOutputStream(uri) ?: throw IllegalStateException("Cannot open the chosen file")
        os.use {
            ZipOutputStream(it.buffered()).use { z ->
                fun addBytes(name: String, bytes: ByteArray) {
                    z.putNextEntry(ZipEntry(name)); z.write(bytes); z.closeEntry()
                }
                fun addFile(name: String, f: File) {
                    z.putNextEntry(ZipEntry(name))
                    f.inputStream().use { i -> i.copyTo(z) }
                    z.closeEntry()
                }
                addBytes("manifest.json", JSONObject()
                    .put("format", FORMAT).put("created", now).put("device", info.device)
                    .put("matches", info.matches).put("learned", info.learnedMatches)
                    .put("versions", info.versions).put("active", info.activeVersion)
                    .put("recordings", info.recordings).toString().toByteArray())
                if (mf.exists()) addFile("matches.json", mf)
                if (vf.exists()) addFile("versions.json", vf)
                for (d in listOf("logs", "models")) {
                    File(files, d).listFiles()?.filter { f -> f.isFile }?.forEach { f -> addFile("$d/${f.name}", f) }
                }
                tracks.forEach { f -> addFile("tracks/${f.name}", f) }
                val o = JSONObject()
                for ((k, v) in prefs.sp.all) {
                    if (k == SKIP_KEY) continue
                    val e = JSONObject()
                    when (v) {
                        is Boolean -> e.put("t", "b").put("v", v)
                        is Int -> e.put("t", "i").put("v", v)
                        is Float -> e.put("t", "f").put("v", v.toDouble())
                        is String -> e.put("t", "s").put("v", v)
                        is Long -> e.put("t", "l").put("v", v)
                        else -> continue
                    }
                    o.put(k, e)
                }
                addBytes("prefs.json", o.toString().toByteArray())
            }
        }
        prefs.lastBackupMs = now
        return info
    }

    /** Reads only the small files of a backup so the app can show what is inside before replacing anything. */
    fun inspect(ctx: Context, uri: Uri): Info {
        var manifest: JSONObject? = null
        var matches: JSONArray? = null
        var versions: JSONObject? = null
        var rec = 0
        var seen = false
        val ins = ctx.contentResolver.openInputStream(uri) ?: throw IllegalStateException("Cannot open the chosen file")
        ins.use {
            ZipInputStream(it.buffered()).use { z ->
                var e = z.nextEntry
                while (e != null) {
                    val n = e.name
                    if (!e.isDirectory && allowedName(n)) {
                        seen = true
                        when {
                            n == "manifest.json" -> manifest = JSONObject(String(readLimited(z, 1_000_000)))
                            n == "matches.json" -> matches = JSONArray(String(readLimited(z, 50_000_000)))
                            n == "versions.json" -> versions = JSONObject(String(readLimited(z, 5_000_000)))
                            n.startsWith("tracks/") -> rec++
                        }
                    }
                    e = z.nextEntry
                }
            }
        }
        if (!seen) throw IllegalStateException("This is not a PitchBot backup")
        val m = manifest
        return countInfo(matches, versions, rec, m?.optLong("created", 0L) ?: 0L, m?.optString("device", "") ?: "", m?.optInt("format", 1) ?: 1)
    }

    fun import(ctx: Context, uri: Uri, learningOnly: Boolean): Info {
        val files = ctx.filesDir
        val tmp = File(ctx.cacheDir, "restore_${System.currentTimeMillis()}")
        tmp.mkdirs()
        try {
            var total = 0L
            val ins = ctx.contentResolver.openInputStream(uri) ?: throw IllegalStateException("Cannot open the chosen file")
            ins.use {
                ZipInputStream(it.buffered()).use { z ->
                    var e = z.nextEntry
                    while (e != null) {
                        val name = e.name
                        if (!e.isDirectory && allowedName(name)) {
                            val target = File(tmp, name)
                            if (!target.canonicalPath.startsWith(tmp.canonicalPath + File.separator)) throw IllegalStateException("Unsafe path in backup")
                            target.parentFile?.mkdirs()
                            target.outputStream().use { out ->
                                val buf = ByteArray(64 * 1024)
                                while (true) {
                                    val n = z.read(buf)
                                    if (n < 0) break
                                    total += n
                                    if (total > MAX_TOTAL_BYTES) throw IllegalStateException("Backup is too large")
                                    out.write(buf, 0, n)
                                }
                            }
                        }
                        e = z.nextEntry
                    }
                }
            }

            val tm = File(tmp, "matches.json")
            val tv = File(tmp, "versions.json")
            val tp = File(tmp, "prefs.json")
            if (!tm.exists() && !tv.exists() && !tp.exists()) throw IllegalStateException("This is not a PitchBot backup")

            // Check everything before touching the app's own files.
            val matches = if (tm.exists()) JSONArray(tm.readText()).also { a ->
                for (i in 0 until a.length()) MatchRec.fromJson(a.getJSONObject(i))
            } else null
            val versions = if (tv.exists()) JSONObject(tv.readText()) else null
            if (versions != null) {
                val arr = versions.getJSONArray("versions")
                for (i in 0 until arr.length()) {
                    val id = arr.getJSONObject(i).getInt("id")
                    if (id != 0 && !File(tmp, "models/v$id.txt").exists()) throw IllegalStateException("Backup is missing model v$id")
                }
            }
            val prefsJson = if (tp.exists()) JSONObject(tp.readText()) else null

            // Apply.
            if (versions != null) {
                File(files, "models").also { it.mkdirs() }.listFiles()?.forEach { it.delete() }
                File(tmp, "models").listFiles()?.forEach { it.copyTo(File(files, "models/${it.name}"), true) }
                tv.copyTo(File(files, "versions.json"), true)
            }
            if (matches != null) {
                File(files, "logs").also { it.mkdirs() }.listFiles()?.forEach { it.delete() }
                File(tmp, "logs").listFiles()?.forEach { it.copyTo(File(files, "logs/${it.name}"), true) }
                tm.copyTo(File(files, "matches.json"), true)
            }
            File(tmp, "tracks").listFiles()?.let { list ->
                File(files, "tracks").mkdirs()
                list.forEach { it.copyTo(File(files, "tracks/${it.name}"), true) }
            }
            if (prefsJson != null) {
                val ed = Prefs(ctx).sp.edit()
                for (k in prefsJson.keys()) {
                    if (k == SKIP_KEY) continue
                    if (learningOnly && isDeviceKey(k)) continue
                    val x = prefsJson.getJSONObject(k)
                    when (x.getString("t")) {
                        "b" -> ed.putBoolean(k, x.getBoolean("v"))
                        "i" -> ed.putInt(k, x.getInt("v"))
                        "f" -> ed.putFloat(k, x.getDouble("v").toFloat())
                        "s" -> ed.putString(k, x.getString("v"))
                        "l" -> ed.putLong(k, x.getLong("v"))
                    }
                }
                ed.apply()
            }
            return countInfo(matches, versions, File(files, "tracks").listFiles()?.size ?: 0, 0L, "", FORMAT)
        } finally {
            tmp.deleteRecursively()
        }
    }

    private fun readLimited(ins: InputStream, max: Int): ByteArray {
        val bo = java.io.ByteArrayOutputStream()
        val buf = ByteArray(16 * 1024)
        while (true) {
            val n = ins.read(buf)
            if (n < 0) break
            if (bo.size() + n > max) throw IllegalStateException("Backup file is malformed")
            bo.write(buf, 0, n)
        }
        return bo.toByteArray()
    }
}

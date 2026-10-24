package com.pitchbot.app

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

data class MatchRec(
    val id: String,
    val ts: Long,
    val poss: Float,
    val passAtt: Int,
    val passOk: Int,
    val shots: Int,
    val losses: Int,
    var gf: Int = 0,
    var ga: Int = 0,
    var scored: Boolean = false,
    var learned: Boolean = false,
    var version: Int = 0,
    var liveSec: Int = 0
) {
    fun passAcc(): Float = if (passAtt > 0) passOk.toFloat() / passAtt else 0f

    // Everything here is measured during the game: goals (read from the radar), pass keeping, possession and ball security.
    fun quality(): Float {
        val gd = ((gf - ga) / 3f).coerceIn(-1f, 1f) * 0.5f + 0.5f
        val minutes = liveSec / 60f
        val keep = if (liveSec >= 60) (1f - losses / minutes / 8f).coerceIn(0f, 1f) else 0.5f
        return 0.40f * gd + 0.25f * passAcc() + 0.15f * (poss / 100f) + 0.20f * keep
    }

    fun toJson() = JSONObject()
        .put("id", id).put("ts", ts).put("poss", poss.toDouble()).put("passAtt", passAtt).put("passOk", passOk)
        .put("shots", shots).put("losses", losses).put("gf", gf).put("ga", ga)
        .put("scored", scored).put("learned", learned).put("version", version).put("liveSec", liveSec)

    companion object {
        fun fromJson(o: JSONObject) = MatchRec(
            o.getString("id"), o.getLong("ts"), o.getDouble("poss").toFloat(), o.getInt("passAtt"),
            o.getInt("passOk"), o.getInt("shots"), o.getInt("losses"), o.optInt("gf"), o.optInt("ga"),
            o.optBoolean("scored"), o.optBoolean("learned"), o.optInt("version"), o.optInt("liveSec")
        )
    }
}

class MatchStore(ctx: Context) {
    private val dir = ctx.applicationContext.filesDir
    private val logs = File(dir, "logs").also { it.mkdirs() }
    private val index = File(dir, "matches.json")
    private var items: MutableList<MatchRec> = load()

    private fun load(): MutableList<MatchRec> {
        val out = ArrayList<MatchRec>()
        try {
            if (index.exists()) {
                val a = JSONArray(index.readText())
                for (i in 0 until a.length()) out.add(MatchRec.fromJson(a.getJSONObject(i)))
            }
        } catch (_: Exception) {
        }
        return out
    }

    private fun save() {
        val a = JSONArray()
        items.forEach { a.put(it.toJson()) }
        index.writeText(a.toString())
    }

    fun reload() { items = load() }

    fun list(): List<MatchRec> = items.sortedBy { it.ts }

    fun add(rec: MatchRec, log: String) {
        File(logs, rec.id + ".txt").writeText(log)
        items.add(rec)
        save()
    }

    fun update(rec: MatchRec) {
        val i = items.indexOfFirst { it.id == rec.id }
        if (i >= 0) items[i] = rec
        save()
    }

    fun log(id: String): String = try { File(logs, "$id.txt").readText() } catch (_: Exception) { "" }
}

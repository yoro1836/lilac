package com.hexadecinull.vineos.helper

import java.util.Base64

data class HelperReply(val ok: Boolean, val payload: String)

data class HelperEvent(val name: String, val fields: List<String>)

sealed interface HelperMessage {
    data class Reply(val id: String, val reply: HelperReply) : HelperMessage

    data class Event(val event: HelperEvent) : HelperMessage
}

data class GuestApp(val packageName: String, val label: String)

data class SnapshotInfo(val name: String, val sizeBytes: Long, val usedBytes: Long, val createdAtSec: Long)

data class GuestStatus(val state: String, val initPid: Int, val bootSeen: Boolean, val canPause: Boolean, val error: String)

// One tab-separated line per message, see helper/helper_server.h in the native sources
object HelperProtocol {
    private const val MAX_ICON_BYTES = 512 * 1024
    private val packagePattern = Regex("^[A-Za-z0-9_.]+$")
    private val pngMagic = byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47)

    fun escape(text: String): String {
        val out = StringBuilder(text.length)
        for (c in text) {
            when (c) {
                '\n' -> out.append("\\n")
                '\r' -> out.append("\\r")
                '\t' -> out.append("\\t")
                '\\' -> out.append("\\\\")
                else -> out.append(c)
            }
        }
        return out.toString()
    }

    fun unescape(text: String): String {
        val out = StringBuilder(text.length)
        var i = 0
        while (i < text.length) {
            val c = text[i]
            if (c == '\\' && i + 1 < text.length) {
                i++
                out.append(
                    when (val n = text[i]) {
                        'n' -> '\n'
                        'r' -> '\r'
                        't' -> '\t'
                        else -> n
                    },
                )
            } else {
                out.append(c)
            }
            i++
        }
        return out.toString()
    }

    fun encodeRequest(id: String, command: String, args: List<String>): String =
        (listOf(id, command) + args.map(::escape)).joinToString("\t")

    fun parseLine(line: String): HelperMessage? {
        val fields = line.split('\t')
        if (fields.size < 2) return null
        if (fields[0] == "*") return HelperMessage.Event(HelperEvent(fields[1], fields.drop(2).map(::unescape)))
        if (fields.size < 3) return null
        return HelperMessage.Reply(fields[0], HelperReply(fields[1] == "ok", unescape(fields[2])))
    }

    fun parseKeyValues(payload: String): Map<String, String> {
        val map = LinkedHashMap<String, String>()
        for (line in payload.lineSequence()) {
            val eq = line.indexOf('=')
            if (eq > 0) map[line.substring(0, eq)] = line.substring(eq + 1)
        }
        return map
    }

    fun parseStatus(payload: String): GuestStatus {
        val kv = parseKeyValues(payload)
        return GuestStatus(
            state = kv["state"] ?: "stopped",
            initPid = kv["init_pid"]?.toIntOrNull() ?: -1,
            bootSeen = kv["boot"] == "1",
            canPause = kv["can_pause"] == "1",
            error = unescape(kv["error"].orEmpty()),
        )
    }

    fun isValidPackageName(name: String): Boolean = name.length in 1..255 && packagePattern.matches(name)

    // Lines of "package TAB label", sorted by label. Anything with a malformed package name is dropped.
    fun parseApps(payload: String): List<GuestApp> = payload.lineSequence()
        .map { it.trimEnd('\r') }
        .filter { it.isNotBlank() }
        .mapNotNull { line ->
            val parts = line.split('\t', limit = 2)
            val pkg = parts[0].trim()
            if (!isValidPackageName(pkg)) return@mapNotNull null
            GuestApp(pkg, parts.getOrNull(1)?.trim().orEmpty().ifEmpty { pkg })
        }
        .distinctBy { it.packageName }
        .sortedWith(compareBy({ it.label.lowercase() }, { it.packageName }))
        .toList()

    // Lines of "name TAB size TAB used TAB mtime"
    fun parseSnapshots(payload: String): List<SnapshotInfo> = payload.lineSequence()
        .mapNotNull { line ->
            val p = line.split('\t')
            if (p.size < 4 || p[0].isEmpty()) return@mapNotNull null
            SnapshotInfo(p[0], p[1].toLongOrNull() ?: 0L, p[2].toLongOrNull() ?: 0L, p[3].toLongOrNull() ?: 0L)
        }
        .sortedByDescending { it.createdAtSec }
        .toList()

    // The agent answers `icon <package>` with one base64 line holding a PNG. Returns null when it is not one.
    fun decodeIcon(payload: String): ByteArray? {
        val text = payload.filterNot { it.isWhitespace() }
        if (text.isEmpty() || text.length > MAX_ICON_BYTES * 2) return null
        val bytes = runCatching { Base64.getDecoder().decode(text) }.getOrNull() ?: return null
        if (bytes.size < pngMagic.size || bytes.size > MAX_ICON_BYTES) return null
        for (i in pngMagic.indices) if (bytes[i] != pngMagic[i]) return null
        return bytes
    }
}

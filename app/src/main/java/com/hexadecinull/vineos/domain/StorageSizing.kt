package com.hexadecinull.vineos.domain

import java.util.Locale

// Sizes for the guest's /data disk, a sparse file that costs nothing until the guest fills it
object StorageSizing {
    const val MIN_MB = 2048
    const val MB_PER_GB = 1024
    private const val RESERVE_MB = 2048
    private val PRESETS_GB = intArrayOf(2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048)

    // Largest size that still leaves the phone some room, never below the minimum and rounded down to whole GB
    fun maxMb(freeBytes: Long): Int {
        val freeMb = (freeBytes / (1024L * 1024L)).coerceAtMost(Int.MAX_VALUE.toLong()).toInt()
        val usable = (freeMb - RESERVE_MB).coerceAtLeast(MIN_MB)
        return usable / MB_PER_GB * MB_PER_GB
    }

    fun clamp(mb: Int, maxMb: Int): Int = mb.coerceIn(MIN_MB, maxMb.coerceAtLeast(MIN_MB))

    // Slider stops: the usual sizes up to the limit, and the limit itself
    fun presetsMb(maxMb: Int): List<Int> {
        val limit = maxMb.coerceAtLeast(MIN_MB)
        val stops = PRESETS_GB.map { it * MB_PER_GB }.filter { it <= limit }.toMutableList()
        if (stops.lastOrNull() != limit) stops.add(limit)
        return stops
    }

    fun nearestPresetIndex(mb: Int, presets: List<Int>): Int {
        var best = 0
        for (i in presets.indices) if (kotlin.math.abs(presets[i] - mb) < kotlin.math.abs(presets[best] - mb)) best = i
        return best
    }

    fun format(mb: Int): String = when {
        mb < MB_PER_GB -> "$mb MB"
        mb % MB_PER_GB == 0 -> "${mb / MB_PER_GB} GB"
        else -> String.format(Locale.ROOT, "%.1f GB", mb / MB_PER_GB.toDouble())
    }

    // "32", "32.5", "32 GB", "32gb" and "32,5" all read as GB. Returns null for anything else.
    fun parseGb(text: String): Int? {
        val cleaned = text.trim().lowercase(Locale.ROOT).removeSuffix("gb").removeSuffix("g").trim().replace(',', '.')
        if (cleaned.isEmpty()) return null
        val gb = cleaned.toDoubleOrNull() ?: return null
        if (gb.isNaN() || gb <= 0.0 || gb > 4096.0) return null
        return Math.round(gb * MB_PER_GB).toInt()
    }

    fun formatGbInput(mb: Int): String = if (mb % MB_PER_GB ==
        0
    ) {
        (mb / MB_PER_GB).toString()
    } else {
        String.format(Locale.ROOT, "%.1f", mb / MB_PER_GB.toDouble())
    }
}

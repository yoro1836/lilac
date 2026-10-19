package com.hexadecinull.vineos.domain

import com.hexadecinull.vineos.data.models.GuestResolution
import com.hexadecinull.vineos.data.models.ScreenFit

data class DisplayPlan(val width: Int, val height: Int, val dpi: Int)

// Picks the size and density of the guest's screen from the phone's real screen
object DisplayPlanner {
    private const val TARGET_DP_WIDTH = 392
    private const val ALIGN = 8
    private const val MIN_SIDE = 240

    fun plan(screenWidthPx: Int, screenHeightPx: Int, fit: ScreenFit, quality: GuestResolution): DisplayPlan {
        var shortSide = minOf(screenWidthPx, screenHeightPx).coerceAtLeast(MIN_SIDE)
        var longSide = maxOf(screenWidthPx, screenHeightPx).coerceAtLeast(MIN_SIDE * 16 / 9)
        if (fit == ScreenFit.KEEP_ASPECT) {
            // The biggest 9:16 rectangle that fits on the screen
            shortSide = minOf(shortSide, longSide * 9 / 16)
            longSide = shortSide * 16 / 9
        }

        if (longSide > quality.longSide) {
            val scale = quality.longSide.toDouble() / longSide
            longSide = quality.longSide
            shortSide = (shortSide * scale).toInt()
        }
        val w = (shortSide / ALIGN * ALIGN).coerceAtLeast(MIN_SIDE)
        val h = (longSide / ALIGN * ALIGN).coerceAtLeast(MIN_SIDE)
        val dpi = (w * 160.0 / TARGET_DP_WIDTH).toInt().coerceIn(120, 640)
        return DisplayPlan(w, h, dpi)
    }
}

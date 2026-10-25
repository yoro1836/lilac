package com.hexadecinull.vineos.domain

import com.google.common.truth.Truth.assertThat
import com.hexadecinull.vineos.data.models.GuestResolution
import com.hexadecinull.vineos.data.models.ScreenFit
import org.junit.Test

class DisplayPlannerTest {
    private fun ratio(p: DisplayPlan) = p.width.toDouble() / p.height

    @Test
    fun `fill mode follows the phone's shape`() {
        val plan = DisplayPlanner.plan(1080, 2400, ScreenFit.FILL, GuestResolution.BALANCED)
        assertThat(plan.height).isEqualTo(1280)
        assertThat(ratio(plan)).isWithin(0.01).of(1080.0 / 2400.0)
    }

    @Test
    fun `landscape screen sizes still give a portrait guest`() {
        val plan = DisplayPlanner.plan(2400, 1080, ScreenFit.FILL, GuestResolution.HIGH)
        assertThat(plan.width).isLessThan(plan.height)
    }

    @Test
    fun `keep aspect mode makes a 9 by 16 guest`() {
        val plan = DisplayPlanner.plan(1080, 2400, ScreenFit.KEEP_ASPECT, GuestResolution.HIGH)
        assertThat(ratio(plan)).isWithin(0.01).of(9.0 / 16.0)
    }

    @Test
    fun `small screens are not scaled up`() {
        val plan = DisplayPlanner.plan(480, 800, ScreenFit.FILL, GuestResolution.HIGH)
        assertThat(plan.width).isEqualTo(480)
        assertThat(plan.height).isEqualTo(800)
    }

    @Test
    fun `native keeps the phone's resolution`() {
        val plan = DisplayPlanner.plan(1080, 2340, ScreenFit.FILL, GuestResolution.NATIVE)
        assertThat(plan.width).isEqualTo(1080)
        assertThat(plan.height).isEqualTo(2336)
    }

    @Test
    fun `sides are multiples of 8`() {
        for (quality in GuestResolution.entries) {
            val plan = DisplayPlanner.plan(1079, 2399, ScreenFit.FILL, quality)
            assertThat(plan.width % 8).isEqualTo(0)
            assertThat(plan.height % 8).isEqualTo(0)
        }
    }

    @Test
    fun `lower quality gives fewer pixels`() {
        val high = DisplayPlanner.plan(1440, 3200, ScreenFit.FILL, GuestResolution.HIGH)
        val low = DisplayPlanner.plan(1440, 3200, ScreenFit.FILL, GuestResolution.LOW)
        assertThat(low.width * low.height).isLessThan(high.width * high.height)
    }

    @Test
    fun `density scales with width`() {
        val small = DisplayPlanner.plan(540, 960, ScreenFit.FILL, GuestResolution.HIGH)
        val big = DisplayPlanner.plan(1080, 1920, ScreenFit.FILL, GuestResolution.HIGH)
        assertThat(big.dpi).isGreaterThan(small.dpi)
        assertThat(big.dpi).isAtLeast(380)
        assertThat(big.dpi).isAtMost(480)
    }

    @Test
    fun `degenerate input is kept sane`() {
        val plan = DisplayPlanner.plan(0, 0, ScreenFit.FILL, GuestResolution.BALANCED)
        assertThat(plan.width).isAtLeast(240)
        assertThat(plan.height).isAtLeast(240)
    }
}

package com.hexadecinull.vineos.domain

import com.google.common.truth.Truth.assertThat
import org.junit.Test

class StorageSizingTest {
    private val gb = 1024L * 1024L * 1024L

    @Test
    fun `limit is the free space minus a reserve, in whole GB`() {
        assertThat(StorageSizing.maxMb(100 * gb)).isEqualTo(98 * 1024)
    }

    @Test
    fun `limit goes well past the old 16 GB cap`() {
        assertThat(StorageSizing.maxMb(200 * gb)).isGreaterThan(16384)
    }

    @Test
    fun `limit never drops below the minimum`() {
        assertThat(StorageSizing.maxMb(0)).isEqualTo(StorageSizing.MIN_MB)
        assertThat(StorageSizing.maxMb(1 * gb)).isEqualTo(StorageSizing.MIN_MB)
    }

    @Test
    fun `huge free space does not overflow`() {
        assertThat(StorageSizing.maxMb(Long.MAX_VALUE)).isGreaterThan(0)
    }

    @Test
    fun `clamp keeps a size inside the range`() {
        assertThat(StorageSizing.clamp(100, 8192)).isEqualTo(StorageSizing.MIN_MB)
        assertThat(StorageSizing.clamp(99999, 8192)).isEqualTo(8192)
        assertThat(StorageSizing.clamp(4096, 8192)).isEqualTo(4096)
    }

    @Test
    fun `presets end at the limit and are ascending`() {
        val presets = StorageSizing.presetsMb(100 * 1024)
        assertThat(presets.first()).isEqualTo(2048)
        assertThat(presets.last()).isEqualTo(100 * 1024)
        assertThat(presets).isInOrder()
        assertThat(presets).contains(32 * 1024)
    }

    @Test
    fun `presets for a tiny limit still hold the limit`() {
        assertThat(StorageSizing.presetsMb(100)).containsExactly(2048)
    }

    @Test
    fun `nearest preset index`() {
        val presets = listOf(2048, 4096, 8192)
        assertThat(StorageSizing.nearestPresetIndex(5000, presets)).isEqualTo(1)
        assertThat(StorageSizing.nearestPresetIndex(99999, presets)).isEqualTo(2)
    }

    @Test
    fun `format uses MB below one GB and GB above`() {
        assertThat(StorageSizing.format(512)).isEqualTo("512 MB")
        assertThat(StorageSizing.format(16384)).isEqualTo("16 GB")
        assertThat(StorageSizing.format(1536)).isEqualTo("1.5 GB")
        assertThat(StorageSizing.format(65536)).isEqualTo("64 GB")
    }

    @Test
    fun `parse accepts plain numbers, units and decimal commas`() {
        assertThat(StorageSizing.parseGb("32")).isEqualTo(32768)
        assertThat(StorageSizing.parseGb(" 32 GB ")).isEqualTo(32768)
        assertThat(StorageSizing.parseGb("32gb")).isEqualTo(32768)
        assertThat(StorageSizing.parseGb("1,5")).isEqualTo(1536)
        assertThat(StorageSizing.parseGb("0.5")).isEqualTo(512)
    }

    @Test
    fun `parse rejects nonsense`() {
        assertThat(StorageSizing.parseGb("")).isNull()
        assertThat(StorageSizing.parseGb("abc")).isNull()
        assertThat(StorageSizing.parseGb("-4")).isNull()
        assertThat(StorageSizing.parseGb("0")).isNull()
        assertThat(StorageSizing.parseGb("999999")).isNull()
        assertThat(StorageSizing.parseGb("NaN")).isNull()
    }

    @Test
    fun `input text round trips`() {
        assertThat(StorageSizing.formatGbInput(32768)).isEqualTo("32")
        assertThat(StorageSizing.formatGbInput(1536)).isEqualTo("1.5")
        assertThat(StorageSizing.parseGb(StorageSizing.formatGbInput(1536))).isEqualTo(1536)
    }
}

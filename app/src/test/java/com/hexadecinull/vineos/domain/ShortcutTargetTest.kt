package com.hexadecinull.vineos.domain

import com.google.common.truth.Truth.assertThat
import com.hexadecinull.vineos.domain.shortcuts.ShortcutTarget
import org.junit.Test

class ShortcutTargetTest {
    private val uuid = "3f1c2d4e-5a6b-4c7d-8e9f-0a1b2c3d4e5f"

    @Test
    fun `valid extras give a target`() {
        val target = ShortcutTarget.fromExtras(uuid, "com.example.app")
        assertThat(target).isEqualTo(ShortcutTarget(uuid, "com.example.app"))
    }

    @Test
    fun `missing extras give nothing`() {
        assertThat(ShortcutTarget.fromExtras(null, "com.example.app")).isNull()
        assertThat(ShortcutTarget.fromExtras(uuid, null)).isNull()
    }

    @Test
    fun `instance ids that could escape the instances folder are rejected`() {
        assertThat(ShortcutTarget.fromExtras("../etc", "com.example.app")).isNull()
        assertThat(ShortcutTarget.fromExtras("a/b", "com.example.app")).isNull()
        assertThat(ShortcutTarget.fromExtras("..", "com.example.app")).isNull()
        assertThat(ShortcutTarget.fromExtras("", "com.example.app")).isNull()
        assertThat(ShortcutTarget.fromExtras("a".repeat(65), "com.example.app")).isNull()
    }

    @Test
    fun `package names with shell characters are rejected`() {
        assertThat(ShortcutTarget.fromExtras(uuid, "com.example; reboot")).isNull()
        assertThat(ShortcutTarget.fromExtras(uuid, "com.example.app\nx")).isNull()
        assertThat(ShortcutTarget.fromExtras(uuid, "")).isNull()
    }

    @Test
    fun `shortcut id carries the instance and package`() {
        val target = ShortcutTarget(uuid, "com.example.app")
        assertThat(target.shortcutId).isEqualTo("app:$uuid:com.example.app")
        assertThat(ShortcutTarget.instanceOfShortcutId(target.shortcutId)).isEqualTo(uuid)
    }

    @Test
    fun `foreign shortcut ids are not ours`() {
        assertThat(ShortcutTarget.instanceOfShortcutId("other:thing")).isNull()
        assertThat(ShortcutTarget.instanceOfShortcutId("app:")).isNull()
    }
}

package com.hexadecinull.vineos.helper

import com.google.common.truth.Truth.assertThat
import org.junit.Test

class GuestProtocolTest {
    @Test
    fun `single touch line`() {
        val line = GuestProtocol.touchLine(GuestProtocol.ACTION_DOWN, 0, listOf(TouchPoint(0, 0.5f, 0.25f)))
        assertThat(line).isEqualTo("touch 0 0 1 0 0.5000 0.2500")
    }

    @Test
    fun `multi touch line carries every pointer`() {
        val line = GuestProtocol.touchLine(
            GuestProtocol.ACTION_POINTER_DOWN,
            1,
            listOf(TouchPoint(0, 0.1f, 0.2f), TouchPoint(3, 0.9f, 0.8f)),
        )
        assertThat(line).isEqualTo("touch 5 1 2 0 0.1000 0.2000 3 0.9000 0.8000")
    }

    @Test
    fun `coordinates outside the screen are clamped`() {
        val line = GuestProtocol.touchLine(GuestProtocol.ACTION_MOVE, 0, listOf(TouchPoint(0, -0.3f, 1.7f)))
        assertThat(line).isEqualTo("touch 2 0 1 0 0.0000 1.0000")
    }

    @Test
    fun `key and text lines`() {
        assertThat(GuestProtocol.keyLine(4, true)).isEqualTo("key 4 1")
        assertThat(GuestProtocol.keyLine(4, false)).isEqualTo("key 4 0")
        assertThat(GuestProtocol.textLine("a\nb")).isEqualTo("text a\\nb")
    }
}

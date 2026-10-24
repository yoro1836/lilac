package io.github.yoro1836.lilac.ui

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class HostAbiTest {
    @Test
    fun `64-bit primary ABIs are supported`() {
        assertTrue(isHostAbiSupported(arrayOf("arm64-v8a", "armeabi-v7a", "armeabi")))
        assertTrue(isHostAbiSupported(arrayOf("x86_64", "x86")))
    }

    @Test
    fun `only the primary ABI counts`() {
        assertFalse(isHostAbiSupported(arrayOf("armeabi-v7a", "arm64-v8a")))
        assertFalse(isHostAbiSupported(arrayOf("x86")))
        assertFalse(isHostAbiSupported(emptyArray()))
    }
}

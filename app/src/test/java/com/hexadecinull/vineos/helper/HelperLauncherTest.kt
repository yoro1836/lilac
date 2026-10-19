package com.hexadecinull.vineos.helper

import com.google.common.truth.Truth.assertThat
import org.junit.Test

class HelperLauncherTest {
    @Test
    fun `command runs the helper through su with its arguments`() {
        val cmd = HelperLauncher.command("/data/app/lib/arm64", "/data/user/0/app/files/instances", 10234, null)
        assertThat(cmd[0]).isEqualTo("su")
        assertThat(cmd[1]).isEqualTo("-c")
        assertThat(cmd[2]).isEqualTo(
            "exec '/data/app/lib/arm64/libvine_helper.so' --base '/data/user/0/app/files/instances' --uid 10234",
        )
    }

    @Test
    fun `qemu path is appended when present`() {
        val cmd = HelperLauncher.command("/lib", "/base", 1, "/lib/libqemu_arm.so")
        assertThat(cmd[2]).endsWith("--qemu '/lib/libqemu_arm.so'")
    }

    @Test
    fun `single quotes in paths cannot break out of the shell string`() {
        assertThat(HelperLauncher.shellQuote("a'b")).isEqualTo("'a'\\''b'")
        assertThat(HelperLauncher.shellQuote("x; rm -rf /")).isEqualTo("'x; rm -rf /'")
    }
}

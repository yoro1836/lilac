package com.hexadecinull.vineos.helper

import com.google.common.truth.Truth.assertThat
import java.util.Base64
import org.junit.Test

class HelperProtocolTest {
    @Test
    fun `escape and unescape round trip special characters`() {
        val text = "a\tb\nc\r\\d \\n literal"
        assertThat(HelperProtocol.unescape(HelperProtocol.escape(text))).isEqualTo(text)
    }

    @Test
    fun `escaped text never contains a tab or newline`() {
        val escaped = HelperProtocol.escape("one\ttwo\nthree")
        assertThat(escaped).doesNotContain("\t")
        assertThat(escaped).doesNotContain("\n")
    }

    @Test
    fun `request line has id, command and escaped args`() {
        val line = HelperProtocol.encodeRequest("7", "exec", listOf("it-1", "a\tb"))
        assertThat(line).isEqualTo("7\texec\tit-1\ta\\tb")
    }

    @Test
    fun `reply line is parsed with its payload unescaped`() {
        val msg = HelperProtocol.parseLine("12\tok\tstate=running\\ninit_pid=42")
        assertThat(msg).isEqualTo(HelperMessage.Reply("12", HelperReply(true, "state=running\ninit_pid=42")))
    }

    @Test
    fun `error reply with empty payload is parsed`() {
        val msg = HelperProtocol.parseLine("3\terr\t")
        assertThat(msg).isEqualTo(HelperMessage.Reply("3", HelperReply(false, "")))
    }

    @Test
    fun `event line is parsed`() {
        val msg = HelperProtocol.parseLine("*\tstate\tinst-1\tbooting\t")
        assertThat(msg).isEqualTo(HelperMessage.Event(HelperEvent("state", listOf("inst-1", "booting", ""))))
    }

    @Test
    fun `garbage lines are ignored`() {
        assertThat(HelperProtocol.parseLine("")).isNull()
        assertThat(HelperProtocol.parseLine("no tabs here")).isNull()
        assertThat(HelperProtocol.parseLine("5\tok")).isNull()
    }

    @Test
    fun `status payload is parsed`() {
        val status = HelperProtocol.parseStatus("state=booting\ninit_pid=1234\nboot=1\ncan_pause=0\nerror=bad\\nthing")
        assertThat(status.state).isEqualTo("booting")
        assertThat(status.initPid).isEqualTo(1234)
        assertThat(status.bootSeen).isTrue()
        assertThat(status.canPause).isFalse()
        assertThat(status.error).isEqualTo("bad\nthing")
    }

    @Test
    fun `status of a missing instance falls back to stopped`() {
        val status = HelperProtocol.parseStatus("")
        assertThat(status.state).isEqualTo("stopped")
        assertThat(status.initPid).isEqualTo(-1)
    }

    @Test
    fun `apps are parsed, deduplicated and sorted by label`() {
        val apps = HelperProtocol.parseApps(
            "org.b.app\tbeta\ncom.a.app\tAlpha\norg.b.app\tduplicate\nbad name\tx\ncom.c.nolabel\n\n",
        )
        assertThat(apps.map { it.packageName }).containsExactly("com.a.app", "org.b.app", "com.c.nolabel").inOrder()
        assertThat(apps[0].label).isEqualTo("Alpha")
        assertThat(apps[2].label).isEqualTo("com.c.nolabel")
    }

    @Test
    fun `package names with shell characters are rejected`() {
        assertThat(HelperProtocol.isValidPackageName("com.example.app_1")).isTrue()
        assertThat(HelperProtocol.isValidPackageName("com.example;rm")).isFalse()
        assertThat(HelperProtocol.isValidPackageName("")).isFalse()
        assertThat(HelperProtocol.isValidPackageName("a b")).isFalse()
    }

    @Test
    fun `snapshots are parsed newest first`() {
        val list = HelperProtocol.parseSnapshots("old\t1000\t500\t100\nnew\t2000\t900\t200\nbroken\n")
        assertThat(list.map { it.name }).containsExactly("new", "old").inOrder()
        assertThat(list[0].usedBytes).isEqualTo(900L)
    }

    @Test
    fun `png icon payload is decoded`() {
        val png = byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3)
        val payload = Base64.getEncoder().encodeToString(png) + "\n"
        assertThat(HelperProtocol.decodeIcon(payload)).isEqualTo(png)
    }

    @Test
    fun `non png and invalid icon payloads are rejected`() {
        assertThat(HelperProtocol.decodeIcon("")).isNull()
        assertThat(HelperProtocol.decodeIcon("not base64 !!")).isNull()
        assertThat(HelperProtocol.decodeIcon(Base64.getEncoder().encodeToString(byteArrayOf(1, 2, 3, 4, 5)))).isNull()
    }
}

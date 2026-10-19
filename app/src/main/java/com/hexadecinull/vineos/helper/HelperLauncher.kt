package com.hexadecinull.vineos.helper

// Builds the `su` command line that starts libvine_helper.so as root
object HelperLauncher {
    const val HELPER_LIB = "libvine_helper.so"
    const val QEMU_LIB = "libqemu_arm.so"

    fun shellQuote(text: String): String = "'" + text.replace("'", "'\\''") + "'"

    fun command(nativeLibDir: String, baseDir: String, uid: Int, qemuPath: String?): List<String> {
        val sb = StringBuilder("exec ")
        sb.append(shellQuote("$nativeLibDir/$HELPER_LIB"))
        sb.append(" --base ").append(shellQuote(baseDir))
        sb.append(" --uid ").append(uid)
        if (!qemuPath.isNullOrEmpty()) sb.append(" --qemu ").append(shellQuote(qemuPath))
        return listOf("su", "-c", sb.toString())
    }
}

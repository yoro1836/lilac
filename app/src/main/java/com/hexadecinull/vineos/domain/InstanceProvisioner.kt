package com.hexadecinull.vineos.domain

import android.content.Context
import android.os.StatFs
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.util.zip.ZipFile
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

data class ProvisionProgress(val fraction: Float, val step: String)

data class VromManifest(val supportedAbis: List<String>, val sha256: Map<String, String>) {
    // True when the ROM only has 32-bit ARM userspace
    val isArm32Only: Boolean get() = supportedAbis.isNotEmpty() &&
        supportedAbis.none { it == "arm64-v8a" } &&
        supportedAbis.any { it.startsWith("armeabi") }

    companion object {
        fun parse(text: String): VromManifest = runCatching {
            val root = Json.parseToJsonElement(text).jsonObject
            VromManifest(
                supportedAbis = root["supportedAbis"]?.jsonArray?.map { it.jsonPrimitive.content }.orEmpty(),
                sha256 = root["sha256"]?.jsonObject?.mapValues { it.value.jsonPrimitive.content }.orEmpty(),
            )
        }.getOrDefault(VromManifest(emptyList(), emptyMap()))
    }
}

// Unpacks a .vrom into a new instance directory. The guest's own kernel entry is skipped, guests run on the phone's kernel.
@Singleton
class InstanceProvisioner @Inject constructor(@ApplicationContext private val context: Context) {
    val instancesDir: File = File(context.filesDir, "instances").also { it.mkdirs() }

    fun instanceDir(instanceId: String): File = File(instancesDir, instanceId)

    fun freeBytes(): Long = runCatching { StatFs(instancesDir.path).availableBytes }.getOrDefault(0L)

    suspend fun create(vrom: File, instanceId: String, onProgress: (ProvisionProgress) -> Unit): Result<File> =
        withContext(Dispatchers.IO) {
            val dir = instanceDir(instanceId)
            try {
                ZipFile(vrom).use { zip ->
                    val manifestEntry = zip.getEntry(MANIFEST) ?: throw IOException("This ROM file has no manifest.json")
                    val manifestText = zip.getInputStream(manifestEntry).bufferedReader().use { it.readText() }
                    val manifest = VromManifest.parse(manifestText)

                    val wanted = PAYLOAD.mapNotNull { name -> zip.getEntry(name)?.let { name to it } }
                    if (wanted.none { it.first == "system.img" }) throw IOException("This ROM file has no system.img")
                    val total = wanted.sumOf { it.second.size.coerceAtLeast(0L) }
                    val free = StatFs(instancesDir.path).availableBytes
                    if (free < total + SAFETY_BYTES) {
                        throw IOException("Not enough free storage. The ROM needs ${total / MB} MB and only ${free / MB} MB are free.")
                    }
                    if (dir.exists()) throw IOException("The instance folder already exists")
                    if (!dir.mkdirs()) throw IOException("Could not create the instance folder")

                    File(dir, MANIFEST).writeText(manifestText)
                    var done = 0L
                    var lastReport = 0L
                    for ((name, entry) in wanted) {
                        val target = File(dir, name)
                        val tmp = File(dir, "$name.part")
                        val digest = MessageDigest.getInstance("SHA-256")
                        zip.getInputStream(entry).use { input ->
                            tmp.outputStream().buffered(BUFFER).use { out ->
                                val buf = ByteArray(BUFFER)
                                while (true) {
                                    val n = input.read(buf)
                                    if (n < 0) break
                                    digest.update(buf, 0, n)
                                    out.write(buf, 0, n)
                                    done += n
                                    if (done - lastReport >= REPORT_STEP || done == total) {
                                        lastReport = done
                                        onProgress(ProvisionProgress(if (total > 0) done.toFloat() / total else 1f, "Unpacking $name"))
                                        ensureActive()
                                    }
                                }
                            }
                        }
                        val expected = manifest.sha256[name]
                        val actual = digest.digest().joinToString("") { "%02x".format(it) }
                        if (expected != null && !actual.equals(expected, ignoreCase = true)) {
                            throw IOException("$name is damaged (checksum mismatch). Download the ROM again.")
                        }
                        if (!tmp.renameTo(target)) throw IOException("Could not finish writing $name")
                    }
                }
                Result.success(dir)
            } catch (e: Exception) {
                dir.deleteRecursively()
                if (e is kotlinx.coroutines.CancellationException) throw e
                Result.failure(e)
            }
        }

    fun readManifest(instanceDir: File): VromManifest {
        val f = File(instanceDir, MANIFEST)
        return if (f.isFile) VromManifest.parse(f.readText()) else VromManifest(emptyList(), emptyMap())
    }

    companion object {
        const val MANIFEST = "manifest.json"
        private val PAYLOAD = listOf("ramdisk.img", "vendor.img", "system.img")
        private const val BUFFER = 256 * 1024
        private const val REPORT_STEP = 4L * 1024 * 1024
        private const val MB = 1024L * 1024L
        private const val SAFETY_BYTES = 256L * MB
    }
}

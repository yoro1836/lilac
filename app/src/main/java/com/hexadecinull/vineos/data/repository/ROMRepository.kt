package com.hexadecinull.vineos.data.repository

import android.content.Context
import android.net.Uri
import android.os.StatFs
import com.hexadecinull.vineos.data.models.DownloadProgress
import com.hexadecinull.vineos.data.models.ROMDownloadState
import com.hexadecinull.vineos.data.models.ROMImage
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.zip.ZipFile
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import okhttp3.OkHttpClient
import okhttp3.Request

@Singleton
class ROMRepository @Inject constructor(@ApplicationContext private val context: Context, private val httpClient: OkHttpClient) {
    private val romsDir = File(context.filesDir, "roms").also { it.mkdirs() }
    private val localRomsFile = File(romsDir, "local_roms.json")
    private val json = Json { ignoreUnknownKeys = true }
    private val verifyCache = ConcurrentHashMap<String, Boolean>()

    private val _manifestRoms = MutableStateFlow<List<ROMImage>>(emptyList())
    private val _localRoms = MutableStateFlow(loadLocalRoms())
    val roms: Flow<List<ROMImage>> = combine(_manifestRoms, _localRoms) { manifest, local -> manifest + local }

    private val _downloadProgress = MutableStateFlow<Map<String, DownloadProgress>>(emptyMap())
    val downloadProgress: Flow<Map<String, DownloadProgress>> = _downloadProgress.asStateFlow()

    suspend fun fetchManifest(): Result<List<ROMImage>> = withContext(Dispatchers.IO) {
        runCatching {
            val request = Request.Builder().url(MANIFEST_URL).build()
            val body = httpClient.newCall(request).execute().use { response ->
                if (!response.isSuccessful) error("HTTP ${response.code}")
                response.body.string()
            }
            val manifest = json.decodeFromString<ROMManifest>(body)
            val enriched = manifest.roms.map { rom ->
                val localFile = File(romsDir, "${rom.id}.vrom")
                val present = localFile.exists()
                val valid = present && verifyFile(localFile, rom.sha256)
                rom.copy(
                    localPath = if (present) localFile.absolutePath else null,
                    isDownloaded = valid,
                    downloadState = when {
                        valid -> ROMDownloadState.READY
                        present -> ROMDownloadState.CORRUPTED
                        else -> ROMDownloadState.NOT_DOWNLOADED
                    },
                )
            }
            _manifestRoms.value = enriched
            enriched
        }
    }

    suspend fun download(rom: ROMImage, onProgress: (DownloadProgress) -> Unit): Result<File> = withContext(Dispatchers.IO) {
        val dest = File(romsDir, "${rom.id}.vrom")
        val part = File(romsDir, "${rom.id}.vrom.part")
        runCatching {
            val free = StatFs(romsDir.path).availableBytes
            if (rom.sizeBytes > 0 && free < rom.sizeBytes + MIN_FREE_BYTES) {
                error("Not enough free storage. This ROM needs ${rom.sizeBytes / MB} MB and ${free / MB} MB are free.")
            }
            updateProgress(rom.id, 0L, rom.sizeBytes, ROMDownloadState.DOWNLOADING)

            val digest = MessageDigest.getInstance("SHA-256")
            val request = Request.Builder().url(rom.downloadUrl).build()
            httpClient.newCall(request).execute().use { response ->
                if (!response.isSuccessful) error("HTTP ${response.code}")
                val body = response.body
                val totalBytes = body.contentLength().takeIf { it > 0 } ?: rom.sizeBytes

                part.outputStream().buffered(BUFFER_SIZE).use { out ->
                    body.byteStream().buffered(BUFFER_SIZE).use { input ->
                        var downloaded = 0L
                        var lastReportMs = 0L
                        val buf = ByteArray(BUFFER_SIZE)
                        var read: Int
                        while (input.read(buf).also { read = it } != -1) {
                            out.write(buf, 0, read)
                            digest.update(buf, 0, read)
                            downloaded += read
                            val now = System.currentTimeMillis()
                            if (now - lastReportMs >= PROGRESS_INTERVAL_MS) {
                                lastReportMs = now
                                updateProgress(rom.id, downloaded, totalBytes, ROMDownloadState.DOWNLOADING)
                                onProgress(DownloadProgress(rom.id, downloaded, totalBytes, ROMDownloadState.DOWNLOADING))
                            }
                        }
                    }
                }
            }

            updateProgress(rom.id, rom.sizeBytes, rom.sizeBytes, ROMDownloadState.VERIFYING)
            val actual = digest.digest().joinToString("") { "%02x".format(it) }
            if (!actual.equals(rom.sha256, ignoreCase = true)) {
                part.delete()
                updateProgress(rom.id, 0L, rom.sizeBytes, ROMDownloadState.CORRUPTED)
                error("SHA-256 verification failed for ${rom.id}")
            }
            dest.delete()
            if (!part.renameTo(dest)) error("Could not save the downloaded ROM")

            updateProgress(rom.id, rom.sizeBytes, rom.sizeBytes, ROMDownloadState.READY)
            refreshROM(rom.id, dest)
            dest
        }.onFailure {
            part.delete()
            if (_downloadProgress.value[rom.id]?.state != ROMDownloadState.CORRUPTED) {
                updateProgress(rom.id, 0L, rom.sizeBytes, ROMDownloadState.NOT_DOWNLOADED)
            }
        }
    }

    suspend fun delete(rom: ROMImage) = withContext(Dispatchers.IO) {
        if (rom.isLocal) {
            rom.localPath?.let { File(it).delete() }
            _localRoms.value = _localRoms.value.filterNot { it.id == rom.id }
            saveLocalRoms()
        } else {
            File(romsDir, "${rom.id}.vrom").delete()
            refreshROM(rom.id, null)
        }
    }

    // Copies a picked .vrom into app storage, reads its embedded manifest.json for metadata, and registers it alongside the manifest-fetched ROMs
    suspend fun importLocalRom(sourceUri: Uri): Result<ROMImage> = withContext(Dispatchers.IO) {
        runCatching {
            val id = "local-${UUID.randomUUID()}"
            val dest = File(romsDir, "$id.vrom")

            val digest = MessageDigest.getInstance("SHA-256")
            context.contentResolver.openInputStream(sourceUri)?.use { input ->
                dest.outputStream().buffered(BUFFER_SIZE).use { output ->
                    val buf = ByteArray(BUFFER_SIZE)
                    var read: Int
                    while (input.read(buf).also { read = it } != -1) {
                        output.write(buf, 0, read)
                        digest.update(buf, 0, read)
                    }
                }
            } ?: error("Could not open the selected file")

            val entry = runCatching { readVromManifest(dest) }.getOrNull()
                ?: run {
                    dest.delete()
                    error("Not a valid .vrom file (missing or unreadable manifest.json)")
                }

            val rom = ROMImage(
                id = id,
                displayName = entry.displayName,
                androidVersion = entry.androidVersion,
                apiLevel = entry.apiLevel,
                description = "Imported from device storage",
                downloadUrl = "",
                sha256 = digest.digest().joinToString("") { "%02x".format(it) },
                sizeBytes = dest.length(),
                supportedAbis = entry.supportedAbis,
                has32BitSupport = entry.has32BitSupport,
                releaseDate = entry.releaseDate,
                isLocal = true,
                localPath = dest.absolutePath,
                isDownloaded = true,
                downloadState = ROMDownloadState.READY,
            )

            _localRoms.value = _localRoms.value + rom
            saveLocalRoms()
            rom
        }
    }

    private fun readVromManifest(vromFile: File): VromManifestEntry {
        ZipFile(vromFile).use { zip ->
            val entry = zip.getEntry("manifest.json") ?: error("manifest.json not found in archive")
            val text = zip.getInputStream(entry).bufferedReader().use { it.readText() }
            return json.decodeFromString(text)
        }
    }

    private fun loadLocalRoms(): List<ROMImage> {
        if (!localRomsFile.exists()) return emptyList()
        return runCatching { json.decodeFromString<List<ROMImage>>(localRomsFile.readText()) }.getOrDefault(emptyList())
    }

    private fun saveLocalRoms() {
        runCatching { localRomsFile.writeText(json.encodeToString(_localRoms.value)) }
    }

    fun getROMFile(romId: String): File? {
        val f = File(romsDir, "$romId.vrom")
        return if (f.exists()) f else null
    }

    fun getRom(romId: String): ROMImage? = (_manifestRoms.value + _localRoms.value).find { it.id == romId }

    private fun hashFile(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().buffered(BUFFER_SIZE).use { stream ->
            val buf = ByteArray(BUFFER_SIZE)
            var read: Int
            while (stream.read(buf).also { read = it } != -1) digest.update(buf, 0, read)
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    // Hashing a multi-GB ROM takes seconds, so the answer is remembered until the file or the expected hash changes
    private fun verifyFile(file: File, expectedSha256: String): Boolean {
        if (!file.exists()) return false
        val key = "${file.absolutePath}|${file.length()}|${file.lastModified()}|$expectedSha256"
        return verifyCache.getOrPut(key) { hashFile(file).equals(expectedSha256, ignoreCase = true) }
    }

    private fun updateProgress(romId: String, downloaded: Long, total: Long, state: ROMDownloadState) {
        _downloadProgress.value = _downloadProgress.value + (romId to DownloadProgress(romId, downloaded, total, state))
    }

    private fun refreshROM(romId: String, localFile: File?) {
        _manifestRoms.value = _manifestRoms.value.map { rom ->
            if (rom.id == romId) {
                rom.copy(
                    localPath = localFile?.absolutePath,
                    isDownloaded = localFile != null,
                    downloadState = if (localFile != null) ROMDownloadState.READY else ROMDownloadState.NOT_DOWNLOADED,
                )
            } else {
                rom
            }
        }
    }

    @Serializable
    private data class VromManifestEntry(
        val displayName: String,
        val androidVersion: String,
        val apiLevel: Int,
        val supportedAbis: List<String>,
        val has32BitSupport: Boolean = false,
        val releaseDate: String = "",
    )

    companion object {
        private const val MANIFEST_URL = "https://vineos.hexadecinull.dpdns.org/roms/manifest.json"
        private const val BUFFER_SIZE = 64 * 1024
        private const val PROGRESS_INTERVAL_MS = 250L
        private const val MB = 1024L * 1024L
        private const val MIN_FREE_BYTES = 256L * MB
    }
}

package com.hexadecinull.vineos.native

import android.view.Surface

// The part of the engine that lives in the app process: probing the device and drawing the guest's frames
object VineRuntime {
    val isLoaded: Boolean = try {
        System.loadLibrary("vine_runtime")
        true
    } catch (_: UnsatisfiedLinkError) {
        false
    }

    external fun getRuntimeVersion(): String

    external fun hostSupportsAArch32(): Boolean

    external fun probeHost(): String

    external fun verifyQemu(path: String): Boolean

    // Opens <instance>/shared/vinefb, returns 0 while the guest has not created it yet
    external fun openDisplay(vinefbPath: String): Long

    external fun closeDisplay(handle: Long)

    external fun attachSurface(handle: Long, surface: Surface?)

    external fun detachSurface(handle: Long)

    external fun startRendering(handle: Long): Boolean

    external fun stopRendering(handle: Long)

    // {open, rendering, width, height, guest frames, frames drawn}
    external fun displayInfo(handle: Long): LongArray
}

package com.hexadecinull.vineos.ui

import android.view.KeyEvent

// While a guest screen shows, the keys it takes (volume) go to the guest, not the phone
object GuestKeys {
    @Volatile var handler: ((KeyEvent) -> Boolean)? = null
}

package com.hexadecinull.vineos.data.models

enum class NavButtonsMode(val label: String, val description: String) {
    HIDDEN("Hidden", "No buttons. The back gesture goes back in the guest. Tap with three fingers to show the controls."),
    AUTO_HIDE("Tap to show", "A small handle at the top. Tap it to show the buttons for a few seconds."),
    ALWAYS("Always visible", "The three buttons stay on top of the guest screen."),
    ;

    companion object {
        fun fromName(name: String?): NavButtonsMode = entries.firstOrNull { it.name == name } ?: HIDDEN
    }
}

enum class ScreenFit(val label: String, val description: String) {
    FILL("Fill the whole screen", "The guest gets your phone's screen shape, so it fills the display edge to edge."),
    KEEP_ASPECT("Keep 9:16 shape", "The guest keeps a classic 9:16 shape, with black bars when your phone is taller."),
    ;

    companion object {
        fun fromName(name: String?): ScreenFit = entries.firstOrNull { it.name == name } ?: FILL
    }
}

enum class GuestResolution(val label: String, val longSide: Int, val description: String) {
    HIGH("High", 1920, "Sharpest picture, uses the most CPU"),
    BALANCED("Balanced", 1280, "Good picture, smoother on most phones"),
    LOW("Low", 960, "Fastest, softer picture"),
    ;

    companion object {
        fun fromName(name: String?): GuestResolution = entries.firstOrNull { it.name == name } ?: BALANCED
    }
}

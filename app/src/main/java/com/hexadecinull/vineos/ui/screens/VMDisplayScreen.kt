package com.hexadecinull.vineos.ui.screens

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ActivityInfo
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.PowerSettingsNew
import androidx.compose.material.icons.filled.Square
import androidx.compose.material3.Button
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.hexadecinull.vineos.data.models.NavButtonsMode
import com.hexadecinull.vineos.data.models.ScreenFit
import com.hexadecinull.vineos.data.models.VMStatus
import com.hexadecinull.vineos.helper.GuestProtocol
import com.hexadecinull.vineos.helper.TouchPoint
import com.hexadecinull.vineos.ui.GuestKeys
import com.hexadecinull.vineos.ui.viewmodel.VMDisplayViewModel
import kotlinx.coroutines.delay

private const val CONTROLS_TIMEOUT_MS = 4000L

@Composable
fun VMDisplayScreen(instanceId: String, onBack: () -> Unit, modifier: Modifier = Modifier, vm: VMDisplayViewModel = hiltViewModel()) {
    val navButtons by vm.navButtons.collectAsStateWithLifecycle()
    val screenFit by vm.screenFit.collectAsStateWithLifecycle()
    val avoidCutout by vm.avoidCutout.collectAsStateWithLifecycle()

    ImmersiveEffect(lockPortrait = screenFit == ScreenFit.FILL)

    DisposableEffect(instanceId) {
        vm.attach(instanceId)
        onDispose { vm.detach() }
    }
    DisposableEffect(vm) {
        val handler = { event: KeyEvent -> vm.onHardwareKey(event) }
        GuestKeys.handler = handler
        onDispose { if (GuestKeys.handler === handler) GuestKeys.handler = null }
    }

    var controlsVisible by remember { mutableStateOf(false) }
    LaunchedEffect(navButtons) { controlsVisible = navButtons == NavButtonsMode.ALWAYS }
    LaunchedEffect(controlsVisible, navButtons) {
        if (controlsVisible && navButtons != NavButtonsMode.ALWAYS) {
            delay(CONTROLS_TIMEOUT_MS)
            controlsVisible = false
        }
    }

    // The system back gesture goes to the guest while it runs, and leaves this screen otherwise
    BackHandler(enabled = vm.isActive) { vm.pressBack() }

    val guestAspect = if (vm.guestWidth > 0 && vm.guestHeight > 0) vm.guestWidth.toFloat() / vm.guestHeight else 9f / 16f

    Box(modifier = modifier.fillMaxSize().background(Color.Black)) {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .then(if (avoidCutout) Modifier.windowInsetsPadding(WindowInsets.displayCutout) else Modifier),
            contentAlignment = Alignment.Center,
        ) {
            GuestSurface(
                onSurfaceReady = vm::onSurfaceReady,
                onSurfaceDestroyed = vm::onSurfaceDestroyed,
                onTouch = vm::onTouch,
                onThreeFingerTouch = { controlsVisible = true },
                modifier = if (screenFit == ScreenFit.KEEP_ASPECT) Modifier.aspectRatio(guestAspect) else Modifier.fillMaxSize(),
            )
        }

        if (navButtons == NavButtonsMode.AUTO_HIDE && !controlsVisible) {
            Box(
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .padding(top = 6.dp)
                    .clickable { controlsVisible = true }
                    .padding(horizontal = 24.dp, vertical = 8.dp),
            ) {
                Box(
                    modifier = Modifier
                        .width(44.dp)
                        .height(5.dp)
                        .background(Color.White.copy(alpha = 0.4f), RoundedCornerShape(50)),
                )
            }
        }

        AnimatedVisibility(
            visible = controlsVisible && vm.isActive,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier.align(Alignment.BottomCenter),
        ) {
            GuestControls(
                onBack = vm::pressBack,
                onHome = vm::pressHome,
                onRecents = vm::pressRecents,
                onExit = onBack,
                onStop = {
                    vm.stopInstance()
                    onBack()
                },
                modifier = Modifier.padding(bottom = 24.dp),
            )
        }

        StatusOverlay(
            visible = !vm.hasFrame || !vm.isActive,
            name = vm.instanceName,
            status = vm.status,
            error = vm.errorText,
            showHint = navButtons == NavButtonsMode.HIDDEN,
            onClose = onBack,
        )
    }
}

@Composable
private fun StatusOverlay(visible: Boolean, name: String, status: VMStatus, error: String?, showHint: Boolean, onClose: () -> Unit) {
    if (!visible) return
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.9f)),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.padding(32.dp)) {
            val failed = status == VMStatus.ERROR
            val stopped = status == VMStatus.STOPPED
            Text(
                text = when {
                    failed -> "Could not start ${name.ifBlank { "the instance" }}"
                    stopped -> "${name.ifBlank { "The instance" }} is not running"
                    else -> "Starting ${name.ifBlank { "instance" }}…"
                },
                style = MaterialTheme.typography.titleMedium,
                color = Color.White,
                textAlign = TextAlign.Center,
            )
            if (failed && !error.isNullOrBlank()) {
                Spacer(Modifier.height(12.dp))
                Text(
                    error,
                    style = MaterialTheme.typography.bodyMedium,
                    color = Color.White.copy(alpha = 0.75f),
                    textAlign = TextAlign.Center
                )
            }
            if (failed || stopped) {
                Spacer(Modifier.height(20.dp))
                Button(onClick = onClose) { Text("Back to instances") }
            } else if (showHint) {
                Spacer(Modifier.height(16.dp))
                Text(
                    "Touch the screen with three fingers to show the controls.",
                    style = MaterialTheme.typography.bodySmall,
                    color = Color.White.copy(alpha = 0.6f),
                    textAlign = TextAlign.Center,
                )
            }
        }
    }
}

// Hides the status and navigation bars, and keeps the phone upright when the guest fills the screen
@Composable
private fun ImmersiveEffect(lockPortrait: Boolean) {
    val view = LocalView.current
    DisposableEffect(lockPortrait) {
        val activity = view.context.findActivity()
        val controller = activity?.window?.let { WindowCompat.getInsetsController(it, view) }
        val previousOrientation = activity?.requestedOrientation
        controller?.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        controller?.hide(WindowInsetsCompat.Type.systemBars())
        if (lockPortrait) activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
        onDispose {
            controller?.show(WindowInsetsCompat.Type.systemBars())
            if (activity != null && previousOrientation != null) activity.requestedOrientation = previousOrientation
        }
    }
}

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}

@Composable
private fun GuestSurface(
    onSurfaceReady: (android.view.Surface) -> Unit,
    onSurfaceDestroyed: () -> Unit,
    onTouch: (action: Int, actionIndex: Int, points: List<TouchPoint>) -> Unit,
    onThreeFingerTouch: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val currentReady by rememberUpdatedState(onSurfaceReady)
    val currentDestroyed by rememberUpdatedState(onSurfaceDestroyed)
    val currentTouch by rememberUpdatedState(onTouch)
    val currentThree by rememberUpdatedState(onThreeFingerTouch)

    AndroidView(
        factory = { context ->
            SurfaceView(context).apply {
                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(holder: SurfaceHolder) = currentReady(holder.surface)

                    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

                    override fun surfaceDestroyed(holder: SurfaceHolder) = currentDestroyed()
                })
                var swallowed = false
                setOnTouchListener { v, event ->
                    val action = event.actionMasked
                    if (action == MotionEvent.ACTION_DOWN) swallowed = false
                    if (!swallowed && action == MotionEvent.ACTION_POINTER_DOWN && event.pointerCount >= 3) {
                        swallowed = true
                        currentTouch(GuestProtocol.ACTION_CANCEL, 0, event.toPoints(v.width, v.height))
                        currentThree()
                    } else if (!swallowed && v.width > 0 && v.height > 0) {
                        currentTouch(action, event.actionIndex, event.toPoints(v.width, v.height))
                    }
                    true
                }
            }
        },
        modifier = modifier,
    )
}

private fun MotionEvent.toPoints(width: Int, height: Int): List<TouchPoint> {
    val w = width.coerceAtLeast(1).toFloat()
    val h = height.coerceAtLeast(1).toFloat()
    return List(pointerCount) { i -> TouchPoint(getPointerId(i), getX(i) / w, getY(i) / h) }
}

@Composable
private fun GuestControls(
    onBack: () -> Unit,
    onHome: () -> Unit,
    onRecents: () -> Unit,
    onExit: () -> Unit,
    onStop: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier
            .background(Color.Black.copy(alpha = 0.55f), CircleShape)
            .padding(horizontal = 16.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        ControlButton(onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") }
        Spacer(Modifier.width(12.dp))
        ControlButton(onHome) { Icon(Icons.Filled.Home, contentDescription = "Home") }
        Spacer(Modifier.width(12.dp))
        ControlButton(onRecents) { Icon(Icons.Filled.Square, contentDescription = "Recents") }
        Spacer(Modifier.width(12.dp))
        ControlButton(onExit) { Icon(Icons.Filled.Close, contentDescription = "Leave, keep running") }
        Spacer(Modifier.width(12.dp))
        ControlButton(onStop) { Icon(Icons.Filled.PowerSettingsNew, contentDescription = "Stop instance") }
    }
}

@Composable
private fun ControlButton(onClick: () -> Unit, content: @Composable () -> Unit) {
    FloatingActionButton(
        onClick = onClick,
        modifier = Modifier.size(44.dp),
        containerColor = Color.White.copy(alpha = 0.15f),
        contentColor = Color.White,
        content = content,
    )
}

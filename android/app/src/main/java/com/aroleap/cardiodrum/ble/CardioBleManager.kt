package com.aroleap.cardiodrum.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.ArrayDeque

/**
 * Owns the whole BLE lifecycle for the CardioDrum: scan → connect → discover →
 * enable notifications → stream frames, plus command writes, a keep-alive, a
 * frame watchdog and auto-reconnect.
 *
 * Design notes carried over from the firmware/dashboard hardening:
 *  - Android GATT allows exactly one outstanding operation at a time. Writes and
 *    the CCCD (notification-enable) write are therefore funnelled through a small
 *    serialized queue; overlapping them silently drops operations.
 *  - The firmware RELEASES THE BRAKE on every disconnect, so the commanded
 *    resistance is re-asserted by the caller whenever we re-enter [Connected].
 *  - A silent PING every 3 s keeps the OS stack's link warm and stops the
 *    firmware's 30 s peer dead-man from releasing the brake mid-session.
 *  - Commands are newline-terminated (the firmware splits its RX buffer on '\n').
 *
 * All calls assume the relevant runtime BLE permissions are already granted; the
 * UI gates [connect] behind a permission request.
 */
@SuppressLint("MissingPermission")
class CardioBleManager(private val appContext: Context) {

    private val tag = "CardioBle"
    private val handler = Handler(Looper.getMainLooper())

    private val btManager =
        appContext.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val adapter: BluetoothAdapter? = btManager?.adapter

    private val _state = MutableStateFlow<ConnectionState>(ConnectionState.Idle)
    val state: StateFlow<ConnectionState> = _state.asStateFlow()

    /** Set by the ViewModel to receive decoded data frames. */
    var onFrame: ((Frame) -> Unit)? = null
    /** Set by the ViewModel to receive [STATUS]/[INFO] text lines. */
    var onText: ((String) -> Unit)? = null
    /** Fired once each time the link (re-)enters Connected, after notifications are on. */
    var onReady: (() -> Unit)? = null

    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var txChar: BluetoothGattCharacteristic? = null
    private var device: BluetoothDevice? = null

    // Intent flag: true between a user connect() and a user disconnect(). While
    // true, an unexpected drop triggers auto-reconnect; a user disconnect clears it.
    @Volatile private var shouldBeConnected = false
    private var reconnectAttempt = 0
    private var downtimeStartMs = 0L

    private var lastFrameMs = 0L
    private var pingCount = 0

    // ---- GATT operation queue ----------------------------------------------
    private val opQueue = ArrayDeque<() -> Unit>()
    private var opInFlight = false
    private val opTimeout = Runnable {
        Log.w(tag, "GATT op timed out; advancing queue")
        completeOp()
    }

    private fun enqueueOp(op: () -> Unit) {
        synchronized(opQueue) { opQueue.add(op) }
        handler.post { runNextOp() }
    }

    private fun runNextOp() {
        val op: (() -> Unit)?
        synchronized(opQueue) {
            if (opInFlight) return
            op = opQueue.poll()
            if (op != null) opInFlight = true
        }
        if (op != null) {
            handler.removeCallbacks(opTimeout)
            handler.postDelayed(opTimeout, 2_000)
            try {
                op.invoke()
            } catch (e: Exception) {
                Log.e(tag, "GATT op threw", e)
                completeOp()
            }
        }
    }

    private fun completeOp() {
        handler.removeCallbacks(opTimeout)
        synchronized(opQueue) { opInFlight = false }
        handler.post { runNextOp() }
    }

    private fun clearOps() {
        handler.removeCallbacks(opTimeout)
        synchronized(opQueue) { opQueue.clear(); opInFlight = false }
    }

    // ---- Public API ---------------------------------------------------------

    fun isBluetoothOn(): Boolean = adapter?.isEnabled == true

    fun connect() {
        if (adapter == null) {
            _state.value = ConnectionState.Error("No Bluetooth adapter")
            return
        }
        if (!adapter.isEnabled) {
            _state.value = ConnectionState.Error("Bluetooth is off")
            return
        }
        if (_state.value == ConnectionState.Connected ||
            _state.value == ConnectionState.Scanning ||
            _state.value == ConnectionState.Connecting
        ) return

        shouldBeConnected = true
        startScan()
    }

    /** Deliberate teardown: releases the brake, stops the GATT link, no reconnect. */
    fun disconnect() {
        shouldBeConnected = false
        stopKeepAlive()
        stopFrameWatchdog()
        cancelReconnect()
        stopScan()
        // Best-effort brake release before we drop the link.
        rawWrite(BleConstants.CMD_STOP)
        closeGatt()
        _state.value = ConnectionState.Idle
    }

    /** Queue a newline-terminated command. Safe to call anytime; ignored if down. */
    fun writeCommand(cmd: String) {
        val char = rxChar ?: return
        val g = gatt ?: return
        val bytes = (cmd + "\n").toByteArray(Charsets.UTF_8)
        enqueueOp { performWrite(g, char, bytes) }
    }

    fun setResistance(pct: Int) = writeCommand(BleConstants.cmdResistance(pct))

    // ---- Scanning -----------------------------------------------------------

    private val scanTimeout = Runnable {
        stopScan()
        if (shouldBeConnected && gatt == null) {
            _state.value = ConnectionState.Error("CardioDrum not found — is it powered on?")
            shouldBeConnected = false
        }
    }

    private fun startScan() {
        val scanner = adapter?.bluetoothLeScanner ?: run {
            _state.value = ConnectionState.Error("BLE scanner unavailable")
            return
        }
        _state.value = ConnectionState.Scanning
        // OR of two filters: match the device name OR the NUS service UUID (the
        // firmware puts both in the scan response). Either is enough to find it.
        val filters = listOf(
            ScanFilter.Builder().setDeviceName(BleConstants.DEVICE_NAME).build(),
            ScanFilter.Builder()
                .setServiceUuid(ParcelUuid(BleConstants.SERVICE_UUID)).build(),
        )
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        try {
            scanner.startScan(filters, settings, scanCallback)
        } catch (e: Exception) {
            _state.value = ConnectionState.Error("Scan failed: ${e.message}")
            return
        }
        handler.postDelayed(scanTimeout, BleConstants.SCAN_TIMEOUT_MS)
    }

    private fun stopScan() {
        handler.removeCallbacks(scanTimeout)
        try {
            adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        } catch (_: Exception) { /* adapter may be off */ }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val dev = result.device ?: return
            val name = dev.name ?: result.scanRecord?.deviceName
            val matches = name == BleConstants.DEVICE_NAME ||
                result.scanRecord?.serviceUuids
                    ?.any { it.uuid == BleConstants.SERVICE_UUID } == true
            if (!matches) return
            stopScan()
            device = dev
            openGatt(dev)
        }

        override fun onScanFailed(errorCode: Int) {
            _state.value = ConnectionState.Error("Scan failed (code $errorCode)")
        }
    }

    // ---- Connection ---------------------------------------------------------

    private fun openGatt(dev: BluetoothDevice) {
        _state.value = ConnectionState.Connecting
        clearOps()
        gatt = dev.connectGatt(appContext, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    private fun closeGatt() {
        clearOps()
        rxChar = null
        txChar = null
        val g = gatt
        gatt = null
        try { g?.disconnect() } catch (_: Exception) {}
        try { g?.close() } catch (_: Exception) {}
    }

    private val gattCallback = object : android.bluetooth.BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothGatt.STATE_CONNECTED) {
                reconnectAttempt = 0
                downtimeStartMs = 0L
                handler.post { g.discoverServices() }
            } else if (newState == BluetoothGatt.STATE_DISCONNECTED) {
                handleDrop()
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val service = g.getService(BleConstants.SERVICE_UUID)
            rxChar = service?.getCharacteristic(BleConstants.RX_CHAR_UUID)
            txChar = service?.getCharacteristic(BleConstants.TX_CHAR_UUID)
            if (service == null || rxChar == null || txChar == null) {
                _state.value = ConnectionState.Error("Service not found on device")
                closeGatt()
                return
            }
            enableNotifications(g, txChar!!)
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int
        ) {
            // CCCD write complete → link is fully up and streaming.
            completeOp()
            onLinkReady()
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int
        ) {
            completeOp()
        }

        // API 33+ delivers the value explicitly.
        override fun onCharacteristicChanged(
            g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray
        ) {
            handleNotify(value)
        }

        // Pre-33 path.
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(
            g: BluetoothGatt, characteristic: BluetoothGattCharacteristic
        ) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
                handleNotify(characteristic.value ?: return)
            }
        }
    }

    private fun enableNotifications(g: BluetoothGatt, char: BluetoothGattCharacteristic) {
        g.setCharacteristicNotification(char, true)
        val cccd = char.getDescriptor(BleConstants.CCCD_UUID) ?: run {
            _state.value = ConnectionState.Error("Notify descriptor missing")
            return
        }
        enqueueOp {
            val enable = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeDescriptor(cccd, enable)
            } else {
                @Suppress("DEPRECATION")
                run {
                    cccd.value = enable
                    g.writeDescriptor(cccd)
                }
            }
        }
    }

    private fun onLinkReady() {
        _state.value = ConnectionState.Connected
        lastFrameMs = System.currentTimeMillis()
        pingCount = 0
        startKeepAlive()
        startFrameWatchdog()
        // Let the ViewModel re-assert the commanded resistance and ask for status.
        handler.post { onReady?.invoke() }
    }

    private fun handleNotify(bytes: ByteArray) {
        lastFrameMs = System.currentTimeMillis()
        val frame = Frame.parse(bytes)
        if (frame != null) {
            onFrame?.invoke(frame)
        } else {
            val text = bytes.toString(Charsets.UTF_8).trim()
            if (text.isNotEmpty()) onText?.invoke(text)
        }
    }

    // ---- Drop handling / reconnect -----------------------------------------

    private fun handleDrop() {
        stopKeepAlive()
        stopFrameWatchdog()
        rxChar = null
        txChar = null
        clearOps()
        try { gatt?.close() } catch (_: Exception) {}
        gatt = null

        if (!shouldBeConnected) {
            _state.value = ConnectionState.Idle
            return
        }
        // Unexpected drop — the firmware will re-advertise almost immediately.
        if (downtimeStartMs == 0L) downtimeStartMs = System.currentTimeMillis()
        scheduleReconnect()
    }

    private fun scheduleReconnect() {
        if (!shouldBeConnected) return
        val downtime = System.currentTimeMillis() - downtimeStartMs
        if (downtime > BleConstants.RECONNECT_GIVE_UP_MS) {
            shouldBeConnected = false
            _state.value = ConnectionState.Error("Lost connection — tap Connect to retry")
            return
        }
        reconnectAttempt++
        _state.value = ConnectionState.Reconnecting(reconnectAttempt)
        // Exponential backoff, capped.
        val backoff = (BleConstants.RECONNECT_MIN_BACKOFF_MS shl (reconnectAttempt - 1))
            .coerceAtMost(BleConstants.RECONNECT_MAX_BACKOFF_MS)
        handler.postDelayed(reconnectRunnable, backoff)
    }

    private val reconnectRunnable = Runnable {
        if (!shouldBeConnected) return@Runnable
        val dev = device
        if (dev != null) openGatt(dev) else startScan()
    }

    private fun cancelReconnect() {
        handler.removeCallbacks(reconnectRunnable)
        reconnectAttempt = 0
        downtimeStartMs = 0L
    }

    // ---- Keep-alive ---------------------------------------------------------

    private val keepAliveRunnable = object : Runnable {
        override fun run() {
            if (_state.value != ConnectionState.Connected) return
            pingCount++
            // A full status request every Nth tick, silent PING otherwise.
            if (pingCount % BleConstants.STATUS_EVERY_N_PINGS == 0) {
                rawWrite(BleConstants.CMD_STATUS)
            } else {
                rawWrite(BleConstants.CMD_PING)
            }
            handler.postDelayed(this, BleConstants.KEEPALIVE_PERIOD_MS)
        }
    }

    private fun startKeepAlive() {
        handler.removeCallbacks(keepAliveRunnable)
        handler.postDelayed(keepAliveRunnable, BleConstants.KEEPALIVE_PERIOD_MS)
    }

    private fun stopKeepAlive() = handler.removeCallbacks(keepAliveRunnable)

    // ---- Frame watchdog -----------------------------------------------------

    private val frameWatchdogRunnable = object : Runnable {
        override fun run() {
            if (_state.value != ConnectionState.Connected) return
            val silence = System.currentTimeMillis() - lastFrameMs
            if (silence > BleConstants.FRAME_WATCHDOG_MS) {
                Log.w(tag, "Frame watchdog: $silence ms silent — forcing reconnect")
                // Force a clean teardown; onConnectionStateChange may be late/never.
                if (downtimeStartMs == 0L) downtimeStartMs = System.currentTimeMillis()
                stopKeepAlive()
                try { gatt?.disconnect() } catch (_: Exception) {}
                // handleDrop() runs from the disconnect callback; if it never comes,
                // force it after a grace period.
                handler.postDelayed({
                    if (_state.value == ConnectionState.Connected) handleDrop()
                }, 1_500)
                return
            }
            handler.postDelayed(this, 1_000)
        }
    }

    private fun startFrameWatchdog() {
        handler.removeCallbacks(frameWatchdogRunnable)
        handler.postDelayed(frameWatchdogRunnable, 1_000)
    }

    private fun stopFrameWatchdog() = handler.removeCallbacks(frameWatchdogRunnable)

    // ---- Low-level write (used by keep-alive and STOP) ----------------------
    // Goes through the same op queue so it never collides with a command write.
    private fun rawWrite(cmd: String) {
        val char = rxChar ?: return
        val g = gatt ?: return
        val bytes = (cmd + "\n").toByteArray(Charsets.UTF_8)
        enqueueOp { performWrite(g, char, bytes) }
    }

    private fun performWrite(
        g: BluetoothGatt, char: BluetoothGattCharacteristic, bytes: ByteArray
    ) {
        val writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        val ok: Boolean = try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeCharacteristic(char, bytes, writeType) ==
                    BluetoothGatt.GATT_SUCCESS
            } else {
                @Suppress("DEPRECATION")
                run {
                    char.writeType = writeType
                    char.value = bytes
                    g.writeCharacteristic(char)
                }
            }
        } catch (e: Exception) {
            Log.e(tag, "write failed", e)
            false
        }
        // For write-no-response Android still calls onCharacteristicWrite, which
        // advances the queue. If the initiate call itself failed, advance now so
        // the queue can't wedge.
        if (!ok) completeOp()
    }
}

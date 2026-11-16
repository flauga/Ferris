package com.aroleap.cardiodrum.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.aroleap.cardiodrum.ble.BleConstants
import com.aroleap.cardiodrum.ble.CardioBleManager
import com.aroleap.cardiodrum.ble.ConnectionState
import com.aroleap.cardiodrum.ble.Frame
import com.aroleap.cardiodrum.data.Physics
import com.aroleap.cardiodrum.data.SessionEntity
import com.aroleap.cardiodrum.data.SessionRepository
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlin.math.abs
import kotlin.math.max

/**
 * Bridges the BLE stream to the UI: decodes each frame through [Physics], keeps
 * the live dashboard state, integrates cumulative work during a session, and
 * persists finished sessions so their work output can be compared.
 */
class CardioViewModel(app: Application) : AndroidViewModel(app) {

    private val ble = CardioBleManager(app.applicationContext)
    private val repo = SessionRepository(app.applicationContext)

    val connectionState: StateFlow<ConnectionState> = ble.state

    private val _live = MutableStateFlow(LiveState())
    val live: StateFlow<LiveState> = _live.asStateFlow()

    /** Slider-backed commanded resistance (0–100). Re-asserted on every connect. */
    private val _commandedResistance = MutableStateFlow(0)
    val commandedResistance: StateFlow<Int> = _commandedResistance.asStateFlow()

    private val _lastSummary = MutableStateFlow<SessionSummary?>(null)
    val lastSummary: StateFlow<SessionSummary?> = _lastSummary.asStateFlow()

    private val _lastMessage = MutableStateFlow<String?>(null)
    val lastMessage: StateFlow<String?> = _lastMessage.asStateFlow()

    val sessions = repo.sessions.stateIn(
        viewModelScope, SharingStarted.WhileSubscribed(5_000), emptyList()
    )

    // ---- Session accumulators ----------------------------------------------
    private var sessionStartedAt = 0L
    private var lastDeviceMs: Long? = null
    private var energyJ = 0.0
    private var distanceM = 0.0
    private var durationSec = 0.0
    private var sumPower = 0.0
    private var sumVel = 0.0
    private var sumRes = 0.0
    private var nSamp = 0
    private var peakPower = 0.0
    private var peakRpm = 0.0

    init {
        ble.onFrame = ::onFrame
        ble.onText = { line ->
            _lastMessage.value = line
            // The firmware's dead-man releases the brake and reports it; reflect 0.
            if (line.contains("DEADMAN", ignoreCase = true) &&
                line.contains("released", ignoreCase = true)
            ) {
                _commandedResistance.value = 0
            }
        }
        ble.onReady = {
            // Firmware releases the brake on every (re)connect — re-assert the load.
            ble.setResistance(_commandedResistance.value)
            ble.writeCommand(BleConstants.CMD_STATUS)
        }

        // Debounced resistance writes: a rapid drag collapses to the settled value.
        viewModelScope.launch {
            _commandedResistance.collectLatest { pct ->
                kotlinx.coroutines.delay(30)
                if (connectionState.value.isConnected) ble.setResistance(pct)
            }
        }
    }

    // ---- Connection ---------------------------------------------------------
    fun connect() = ble.connect()

    fun disconnect() {
        if (_live.value.sessionActive) stopSession()
        ble.disconnect()
    }

    fun isBluetoothOn() = ble.isBluetoothOn()

    // ---- Resistance ---------------------------------------------------------
    fun setResistance(pct: Int) {
        _commandedResistance.value = pct.coerceIn(0, 100)
    }

    // ---- Session control ----------------------------------------------------
    fun startSession() {
        resetAccumulators()
        sessionStartedAt = System.currentTimeMillis()
        _lastSummary.value = null
        _live.value = _live.value.copy(
            sessionActive = true,
            energyJ = 0.0, durationSec = 0.0, avgPowerW = 0.0,
            distanceM = 0.0, peakPowerW = 0.0, peakRpm = 0.0,
        )
    }

    fun stopSession() {
        if (!_live.value.sessionActive) return
        val avgPower = if (nSamp > 0) sumPower / nSamp else 0.0
        val avgVel = if (nSamp > 0) sumVel / nSamp else 0.0
        val avgRes = if (nSamp > 0) sumRes / nSamp else 0.0
        val workJ = energyJ
        val dist = distanceM
        val dur = durationSec
        val samples = nSamp
        val pPower = peakPower
        val pRpm = peakRpm
        val startedAt = sessionStartedAt

        _live.value = _live.value.copy(sessionActive = false)

        viewModelScope.launch {
            val previousBest = repo.maxWork()
            val entity = SessionEntity(
                startedAt = startedAt,
                durationSec = dur,
                workJ = workJ,
                avgPowerW = avgPower,
                peakPowerW = pPower,
                avgVelocityMs = avgVel,
                peakRpm = pRpm,
                avgResistancePct = avgRes,
                distanceM = dist,
                sampleCount = samples,
            )
            repo.save(entity)
            _lastSummary.value = SessionSummary(
                startedAt = startedAt,
                durationSec = dur,
                workJ = workJ,
                avgPowerW = avgPower,
                peakPowerW = pPower,
                avgVelocityMs = avgVel,
                peakRpm = pRpm,
                avgResistancePct = avgRes,
                distanceM = dist,
                sampleCount = samples,
                previousBestJ = previousBest,
            )
        }
    }

    fun dismissSummary() { _lastSummary.value = null }

    fun deleteSession(session: SessionEntity) {
        viewModelScope.launch { repo.delete(session) }
    }

    // ---- Frame handling -----------------------------------------------------
    private fun onFrame(frame: Frame) {
        val rpm = frame.rpm
        val resistance = frame.resistancePct
        val velocity = Physics.velocity(rpm)
        val power = Physics.power(resistance, rpm)

        val active = _live.value.sessionActive
        if (active) {
            val last = lastDeviceMs
            if (last != null) {
                val dt = (frame.ms - last) / 1000.0
                // Guard against reorder / the 49.7-day device-ms wrap.
                if (dt in 0.0..1.0) {
                    energyJ += power * dt
                    distanceM += velocity * dt
                    durationSec += dt
                }
            }
            lastDeviceMs = frame.ms
            sumPower += power
            sumVel += velocity
            sumRes += resistance
            nSamp++
            peakPower = max(peakPower, power)
            peakRpm = max(peakRpm, abs(rpm))
        }

        _live.value = _live.value.copy(
            rpm = rpm,
            power = power,
            velocity = velocity,
            resistance = resistance,
            positionCounts = frame.positionCounts,
            energyJ = energyJ,
            durationSec = durationSec,
            avgPowerW = if (nSamp > 0) sumPower / nSamp else 0.0,
            distanceM = distanceM,
            peakPowerW = peakPower,
            peakRpm = peakRpm,
        )
    }

    private fun resetAccumulators() {
        lastDeviceMs = null
        energyJ = 0.0; distanceM = 0.0; durationSec = 0.0
        sumPower = 0.0; sumVel = 0.0; sumRes = 0.0; nSamp = 0
        peakPower = 0.0; peakRpm = 0.0
    }

    override fun onCleared() {
        super.onCleared()
        ble.disconnect()
    }
}

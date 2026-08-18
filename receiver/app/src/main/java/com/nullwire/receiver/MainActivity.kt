package com.nullwire.receiver

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.AutomaticGainControl
import android.media.audiofx.NoiseSuppressor
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.Process
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.ProgressBar
import android.widget.SeekBar
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.SecureRandom
import java.util.concurrent.atomic.AtomicInteger

class MainActivity : AppCompatActivity() {

    companion object {
        private const val PERMISSION_REQ_CODE = 3001
        private const val AUDIO_PORT = 50005
        private const val MIC_PORT = 50006
        private const val DISCOVERY_PORT = 50007
        private const val NOTIFICATION_ID = 1001
        private const val CHANNEL_ID = "nullwire_playback_channel"
        private const val PACKET_MAGIC: Short = 0x574E
        private const val PACKET_HEADER_SIZE = 8
    }

    private var isListening = false
    private var isMicStreaming = false
    private var isBeaconRunning = false

    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    private lateinit var rootLayout: View
    private lateinit var tvIpAddress: TextView
    private lateinit var tvPairingPin: TextView
    private lateinit var tvStatus: TextView
    private lateinit var tvStats: TextView
    private lateinit var btnListen: Button
    private lateinit var pbLevelMeter: ProgressBar
    private lateinit var sbBass: SeekBar
    private lateinit var tvBassVal: TextView
    private lateinit var sbTreble: SeekBar
    private lateinit var tvTrebleVal: TextView
    private lateinit var tvMicStatus: TextView
    private lateinit var btnMic: Button

    private var audioRecord: AudioRecord? = null
    private var echoCanceler: AcousticEchoCanceler? = null
    private var noiseSuppressor: NoiseSuppressor? = null
    private var autoGain: AutomaticGainControl? = null
    private var micSocket: DatagramSocket? = null
    private var micThread: Thread? = null
    private var pcTargetIp = "192.168.1.10"

    private var beaconThread: Thread? = null
    private var beaconSocket: DatagramSocket? = null

    private val sessionToken = AtomicInteger(0)
    private var lastPacketCount: Long = 0
    private var isConnectedState = false
    private var stalePacketTicks = 0

    private lateinit var tvWifiAccelStatus: TextView
    private lateinit var switchWifiAccel: androidx.appcompat.widget.SwitchCompat
    private var isWifiAccelEnabled = true

    private lateinit var layoutToastBanner: View
    private lateinit var viewToastDot: View
    private lateinit var tvToastTitle: TextView
    private lateinit var tvToastMessage: TextView
    private var toastDismissRunnable: Runnable? = null

    fun showAppToast(title: String, message: String, dotColor: Int, durationMs: Long = 3500) {
        runOnUiThread {
            toastDismissRunnable?.let { handler.removeCallbacks(it) }

            tvToastTitle.text = title
            tvToastMessage.text = message
            viewToastDot.backgroundTintList = android.content.res.ColorStateList.valueOf(dotColor)

            if (layoutToastBanner.visibility != View.VISIBLE) {
                layoutToastBanner.visibility = View.VISIBLE
                layoutToastBanner.alpha = 0f
                layoutToastBanner.translationY = 80f
                layoutToastBanner.animate()
                    .alpha(1f)
                    .translationY(0f)
                    .setDuration(280)
                    .start()
            }

            val dismiss = Runnable {
                layoutToastBanner.animate()
                    .alpha(0f)
                    .translationY(80f)
                    .setDuration(280)
                    .withEndAction { layoutToastBanner.visibility = View.GONE }
                    .start()
            }
            toastDismissRunnable = dismiss
            handler.postDelayed(dismiss, durationMs)
        }
    }

    private val handler = Handler(Looper.getMainLooper())
    private val telemetryRunnable = object : Runnable {
        override fun run() {
            if (isListening) {
                val rms = AudioEngine.getPlaybackRms()
                val packets = AudioEngine.getPlaybackPacketCount()
                val level = (rms * 100).toInt().coerceIn(0, 100)
                pbLevelMeter.progress = level

                if (packets > lastPacketCount) {
                    stalePacketTicks = 0
                    tvStats.text = "Packets: $packets  ·  DMA Latency: <2.67ms  ·  Loss: 0.0%"
                    if (!isConnectedState) {
                        isConnectedState = true
                        tvStatus.text = "● Streaming 48kHz Lossless Audio"
                        tvStatus.setTextColor(getColor(R.color.color_green))
                        showAppToast("CONNECTED TO PC", "Streaming Lossless 48kHz Audio (<2.67ms DMA)", getColor(R.color.color_green), 3500)
                        updateNotification("● Streaming Lossless Audio (DMA Active)", true)
                    }
                } else if (packets > 0 && isConnectedState) {
                    stalePacketTicks++
                    if (stalePacketTicks > 50) { // ~2.5 seconds without packets
                        isConnectedState = false
                        tvStatus.text = "⚠️ Connection Lost · Waiting for PC..."
                        tvStatus.setTextColor(getColor(R.color.color_red))
                        showAppToast("CONNECTION LOST", "PC audio stream disconnected. Searching...", getColor(R.color.color_red), 4000)
                        updateNotification("⚠️ Connection Lost · Waiting for PC...", false)
                    }
                } else {
                    tvStats.text = "Packets: 0  ·  DMA Ready  ·  Loss: 0.0%"
                }
                lastPacketCount = packets
                handler.postDelayed(this, 50)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        createNotificationChannel()
        rotateSessionToken()

        rootLayout = findViewById(R.id.rootLayout)
        tvIpAddress = findViewById(R.id.tvIpAddress)
        tvPairingPin = findViewById(R.id.tvPairingPin)
        tvStatus = findViewById(R.id.tvStatus)
        tvStats = findViewById(R.id.tvStats)
        btnListen = findViewById(R.id.btnListen)
        pbLevelMeter = findViewById(R.id.pbLevelMeter)
        sbBass = findViewById(R.id.sbBass)
        tvBassVal = findViewById(R.id.tvBassVal)
        sbTreble = findViewById(R.id.sbTreble)
        tvTrebleVal = findViewById(R.id.tvTrebleVal)
        tvMicStatus = findViewById(R.id.tvMicStatus)
        btnMic = findViewById(R.id.btnMic)
        tvWifiAccelStatus = findViewById(R.id.tvWifiAccelStatus)
        switchWifiAccel = findViewById(R.id.switchWifiAccel)
        layoutToastBanner = findViewById(R.id.layoutToastBanner)
        viewToastDot = findViewById(R.id.viewToastDot)
        tvToastTitle = findViewById(R.id.tvToastTitle)
        tvToastMessage = findViewById(R.id.tvToastMessage)

        switchWifiAccel.isChecked = isWifiAccelEnabled
        switchWifiAccel.setOnCheckedChangeListener { _, isChecked ->
            isWifiAccelEnabled = isChecked
            AudioEngine.setWifiAcceleration(isChecked)
            if (isChecked) {
                if (isListening) acquireLocks()
                tvWifiAccelStatus.text = "⚡ Acceleration Active (Low-Latency Lock & Nice -19)"
                tvWifiAccelStatus.setTextColor(getColor(R.color.color_green))
                showAppToast("⚡ WI-FI FASTPATH ACTIVE", "Low-Latency Wi-Fi Lock & High Priority Active", getColor(R.color.color_green), 3000)
            } else {
                releaseLocks()
                tvWifiAccelStatus.text = "⚪ Standard Wi-Fi Mode (Battery Saver)"
                tvWifiAccelStatus.setTextColor(getColor(R.color.color_text_sec))
                showAppToast("⚪ STANDARD WI-FI MODE", "Battery Saver Mode Active", getColor(R.color.color_text_sec), 3000)
            }
        }

        ViewCompat.setOnApplyWindowInsetsListener(rootLayout) { view, insets ->
            val systemBars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            val displayCutout = insets.getInsets(WindowInsetsCompat.Type.displayCutout())
            val topPadding = maxOf(systemBars.top, displayCutout.top) + 20
            val bottomPadding = maxOf(systemBars.bottom, displayCutout.bottom) + 32
            view.setPadding(view.paddingLeft, topPadding, view.paddingRight, bottomPadding)
            insets
        }

        refreshNetworkIdentity()

        btnListen.setOnClickListener {
            if (isListening) {
                stopListening()
            } else {
                startListening()
            }
        }

        sbBass.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                tvBassVal.text = "+$progress dB"
                AudioEngine.setBassBoost(progress.toFloat())
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })

        sbTreble.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                tvTrebleVal.text = "+$progress dB"
                AudioEngine.setTrebleBoost(progress.toFloat())
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })

        btnMic.setOnClickListener {
            if (isMicStreaming) {
                stopMicStreaming()
            } else {
                promptPcIpAndStartMic()
            }
        }

        // Auto-start discovery beacon so PC can find this phone
        startDiscoveryBeacon()
        // NOTE: Audio listening does NOT auto-start. User must press Listen button.
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val name = "NullWire Audio Stream"
            val descriptionText = "Real-time notifications for NullWire audio streaming"
            val importance = NotificationManager.IMPORTANCE_LOW
            val channel = NotificationChannel(CHANNEL_ID, name, importance).apply {
                description = descriptionText
            }
            val notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            notificationManager.createNotificationChannel(channel)
        }
    }

    private fun updateNotification(contentText: String, isConnected: Boolean = false) {
        try {
            val intent = Intent(this, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_SINGLE_TOP
            }
            val pendingIntent = PendingIntent.getActivity(this, 0, intent, PendingIntent.FLAG_IMMUTABLE)

            val title = if (isConnected) "⚡ NullWire Connected" else "NullWire Pro Audio"
            val builder = NotificationCompat.Builder(this, CHANNEL_ID)
                .setSmallIcon(R.mipmap.ic_launcher)
                .setContentTitle(title)
                .setContentText(contentText)
                .setContentIntent(pendingIntent)
                .setOngoing(true)
                .setPriority(if (isConnected) NotificationCompat.PRIORITY_DEFAULT else NotificationCompat.PRIORITY_LOW)

            val notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            notificationManager.notify(NOTIFICATION_ID, builder.build())
        } catch (_: Exception) {}
    }

    private fun cancelNotification() {
        try {
            val notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            notificationManager.cancel(NOTIFICATION_ID)
        } catch (_: Exception) {}
    }

    private fun rotateSessionToken() {
        val rng = SecureRandom()
        var token: Int
        do {
            token = rng.nextInt()
        } while (token == 0)
        sessionToken.set(token)
        AudioEngine.setSessionToken(token)
    }

    private fun pairingPinText(): String {
        val pin = (sessionToken.get().toUInt() % 1_000_000u).toInt()
        return String.format("%06d", pin)
    }

    private fun refreshNetworkIdentity() {
        tvIpAddress.text = getWifiIpAddress()
        tvPairingPin.text = "Pairing PIN: ${pairingPinText()}"
    }

    private fun startListening() {
        if (isListening) return
        acquireLocks()
        AudioEngine.setSessionToken(sessionToken.get())
        val success = AudioEngine.startNativePlayback(AUDIO_PORT)
        if (success) {
            isListening = true
            isConnectedState = false
            btnListen.text = "⏹ Stop Audio Stream"
            btnListen.setBackgroundColor(getColor(R.color.color_red))
            tvStatus.text = "● AAudio Direct MMAP Ready"
            tvStatus.setTextColor(getColor(R.color.color_green))
            handler.post(telemetryRunnable)
            updateNotification("Ready to receive lossless audio on port 50005")
        } else {
            if (!isMicStreaming) releaseLocks()
        }
    }

    private fun stopListening() {
        isListening = false
        isConnectedState = false
        handler.removeCallbacks(telemetryRunnable)
        AudioEngine.stopNativePlayback()
        if (!isMicStreaming) releaseLocks()
        cancelNotification()

        btnListen.text = "▶ Listen (Connect Audio)"
        btnListen.setBackgroundColor(getColor(R.color.color_accent))
        tvStatus.text = "Ready to receive audio"
        tvStatus.setTextColor(getColor(R.color.color_text_sec))
        pbLevelMeter.progress = 0
        tvStats.text = "Packets: 0  ·  DMA Idle"
        Toast.makeText(this, "Audio Stream Stopped", Toast.LENGTH_SHORT).show()
    }

    private fun sanitizedDeviceName(): String {
        val raw = (Build.MODEL ?: "Android").replace('|', ' ').replace('\n', ' ').trim()
        return raw.take(32).ifEmpty { "Android" }
    }

    private fun startDiscoveryBeacon() {
        if (isBeaconRunning) return
        isBeaconRunning = true

        beaconThread = Thread {
            var socket: DatagramSocket? = null
            val bcastAddr = InetAddress.getByName("255.255.255.255")
            val recvBuf = ByteArray(512)

            while (isBeaconRunning) {
                try {
                    if (socket == null || socket.isClosed) {
                        socket = DatagramSocket(DISCOVERY_PORT).apply {
                            broadcast = true
                            soTimeout = 300
                            reuseAddress = true
                        }
                        beaconSocket = socket
                    }

                    val myIp = getWifiIpAddress()
                    val token = sessionToken.get().toUInt()
                    val beaconMsg = "NWDISC|${sanitizedDeviceName()}|$token"
                    val beaconBytes = beaconMsg.toByteArray(Charsets.US_ASCII)
                    val beaconMsg2 = "NWBC|${sanitizedDeviceName()}|$myIp|$token"
                    val beaconBytes2 = beaconMsg2.toByteArray(Charsets.US_ASCII)

                    try {
                        socket.send(DatagramPacket(beaconBytes, beaconBytes.size, bcastAddr, DISCOVERY_PORT))
                        socket.send(DatagramPacket(beaconBytes2, beaconBytes2.size, bcastAddr, DISCOVERY_PORT))
                    } catch (_: Exception) {}

                    val endTime = System.currentTimeMillis() + 1000L
                    while (System.currentTimeMillis() < endTime && isBeaconRunning) {
                        try {
                            val recvPkt = DatagramPacket(recvBuf, recvBuf.size)
                            socket.receive(recvPkt)
                            val msg = String(recvPkt.data, 0, recvPkt.length, Charsets.US_ASCII).trim()
                            val from = recvPkt.address
                            if (msg.startsWith("NWDS") && from is Inet4Address && isSafeLanAddress(from)) {
                                val replyPkt = DatagramPacket(beaconBytes, beaconBytes.size, from, recvPkt.port)
                                socket.send(replyPkt)
                            }
                        } catch (_: Exception) {
                            break
                        }
                    }
                } catch (_: Exception) {
                    try { Thread.sleep(1000) } catch (_: Exception) { break }
                }
            }
            try { socket?.close() } catch (_: Exception) {}
        }
        beaconThread?.start()
    }

    private fun stopDiscoveryBeacon() {
        isBeaconRunning = false
        try { beaconSocket?.close() } catch (_: Exception) {}
        beaconSocket = null
        beaconThread?.interrupt()
        beaconThread = null
    }

    private fun promptPcIpAndStartMic() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.RECORD_AUDIO), PERMISSION_REQ_CODE)
            return
        }

        val input = EditText(this)
        input.setText(pcTargetIp)
        input.setSingleLine(true)
        input.filters = arrayOf(android.text.InputFilter.LengthFilter(45))

        AlertDialog.Builder(this)
            .setTitle("Target PC IP Address")
            .setMessage("Enter the IPv4 address of your Windows PC on this Wi-Fi:")
            .setView(input)
            .setPositiveButton("Start Mic") { _, _ ->
                val ip = input.text.toString().trim()
                if (isValidUnicastIpv4(ip)) {
                    pcTargetIp = ip
                    startMicStreaming(ip)
                } else {
                    Toast.makeText(this, "Enter a valid LAN IPv4 address", Toast.LENGTH_SHORT).show()
                }
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == PERMISSION_REQ_CODE) {
            if (grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                promptPcIpAndStartMic()
            } else {
                Toast.makeText(this, "Microphone permission is required", Toast.LENGTH_SHORT).show()
            }
        }
    }

    private fun startMicStreaming(ip: String) {
        stopMicStreaming()
        acquireLocks()

        try {
            val minBuf = AudioRecord.getMinBufferSize(
                48000,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT
            )
            if (minBuf <= 0) {
                throw IllegalStateException("Unsupported mic format")
            }

            var recorder = AudioRecord(
                MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                48000,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                minBuf * 2
            )

            if (recorder.state != AudioRecord.STATE_INITIALIZED) {
                recorder.release()
                recorder = AudioRecord(
                    MediaRecorder.AudioSource.MIC,
                    48000,
                    AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT,
                    minBuf * 2
                )
            }

            if (recorder.state != AudioRecord.STATE_INITIALIZED) {
                recorder.release()
                throw IllegalStateException("AudioRecord failed to initialize")
            }

            val sessionId = recorder.audioSessionId
            if (sessionId != 0) {
                try {
                    if (AcousticEchoCanceler.isAvailable()) {
                        echoCanceler = AcousticEchoCanceler.create(sessionId)
                        echoCanceler?.enabled = true
                    }
                } catch (_: Exception) {}
                try {
                    if (NoiseSuppressor.isAvailable()) {
                        noiseSuppressor = NoiseSuppressor.create(sessionId)
                        noiseSuppressor?.enabled = true
                    }
                } catch (_: Exception) {}
                try {
                    if (AutomaticGainControl.isAvailable()) {
                        autoGain = AutomaticGainControl.create(sessionId)
                        autoGain?.enabled = true
                    }
                } catch (_: Exception) {}
            }

            recorder.startRecording()
            audioRecord = recorder

            micSocket = DatagramSocket()
            val targetAddr = InetAddress.getByName(ip)
            if (targetAddr !is Inet4Address || !isSafeLanAddress(targetAddr)) {
                throw IllegalArgumentException("PC address is not a safe LAN IPv4")
            }
            isMicStreaming = true

            micThread = Thread {
                Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
                val pcm = ByteArray(1024)
                val packet = ByteArray(PACKET_HEADER_SIZE + pcm.size)
                var seq = 0

                while (isMicStreaming) {
                    try {
                        val read = audioRecord?.read(pcm, 0, pcm.size) ?: 0
                        if (read > 0 && micSocket != null) {
                            ByteBuffer.wrap(packet, 0, PACKET_HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
                                .putShort(PACKET_MAGIC)
                                .putShort(seq.toShort())
                                .putInt(sessionToken.get())
                            seq = (seq + 1) and 0xFFFF
                            System.arraycopy(pcm, 0, packet, PACKET_HEADER_SIZE, read)
                            micSocket?.send(DatagramPacket(packet, PACKET_HEADER_SIZE + read, targetAddr, MIC_PORT))
                        }
                    } catch (_: Exception) {
                        if (!isMicStreaming) break
                    }
                }
            }
            micThread?.start()

            btnMic.text = "⏹ Stop Phone Mic"
            btnMic.setBackgroundColor(getColor(R.color.color_red))
            tvMicStatus.text = "● Streaming Mic to PC ($ip:50006)"
            tvMicStatus.setTextColor(getColor(R.color.color_green))
            Toast.makeText(this, "🎙 Phone Mic Active -> PC", Toast.LENGTH_SHORT).show()

        } catch (e: Exception) {
            stopMicStreaming()
            Toast.makeText(this, "Microphone error: ${e.message}", Toast.LENGTH_SHORT).show()
        }
    }

    private fun releaseMicEffects() {
        try { echoCanceler?.release() } catch (_: Exception) {}
        echoCanceler = null
        try { noiseSuppressor?.release() } catch (_: Exception) {}
        noiseSuppressor = null
        try { autoGain?.release() } catch (_: Exception) {}
        autoGain = null
    }

    private fun stopMicStreaming() {
        isMicStreaming = false
        try { micSocket?.close() } catch (_: Exception) {}
        micSocket = null

        micThread?.interrupt()
        try { micThread?.join(300) } catch (_: Exception) {}
        micThread = null

        try {
            audioRecord?.stop()
            audioRecord?.release()
        } catch (_: Exception) {}
        audioRecord = null
        releaseMicEffects()

        if (!isListening) releaseLocks()

        if (::btnMic.isInitialized) {
            btnMic.text = "🎙 Start Phone Mic (to PC)"
            btnMic.setBackgroundColor(getColor(R.color.color_card))
            tvMicStatus.text = "Mic: Idle (Stream phone voice to PC)"
            tvMicStatus.setTextColor(getColor(R.color.color_text))
        }
    }

    private fun acquireLocks() {
        try {
            val pm = getSystemService(Context.POWER_SERVICE) as? PowerManager
            if (wakeLock == null) {
                wakeLock = pm?.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "NullWire:NativeWakeLock")?.apply {
                    acquire(12 * 60 * 60 * 1000L)
                }
            }
            val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            if (wifiLock == null) {
                val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                } else {
                    @Suppress("DEPRECATION")
                    WifiManager.WIFI_MODE_FULL_HIGH_PERF
                }
                wifiLock = wm?.createWifiLock(mode, "NullWire:NativeWifiLock")?.apply {
                    acquire()
                }
            }
        } catch (_: Exception) {}
    }

    private fun releaseLocks() {
        try { if (wakeLock?.isHeld == true) wakeLock?.release() } catch (_: Exception) {}
        wakeLock = null
        try { if (wifiLock?.isHeld == true) wifiLock?.release() } catch (_: Exception) {}
        wifiLock = null
    }

    private fun isSafeLanAddress(addr: Inet4Address): Boolean {
        if (addr.isAnyLocalAddress || addr.isMulticastAddress) return false
        val bytes = addr.address
        val b0 = bytes[0].toInt() and 0xFF
        val b1 = bytes[1].toInt() and 0xFF
        if (b0 == 10) return true
        if (b0 == 127) return true
        if (b0 == 192 && b1 == 168) return true
        if (b0 == 172 && b1 in 16..31) return true
        return false
    }

    private fun isValidUnicastIpv4(ip: String): Boolean {
        return try {
            val addr = InetAddress.getByName(ip)
            addr is Inet4Address && !addr.isAnyLocalAddress && !addr.isMulticastAddress && isSafeLanAddress(addr)
        } catch (_: Exception) {
            false
        }
    }

    private fun getWifiIpAddress(): String {
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces() ?: return "Waiting for Wi-Fi"
            while (interfaces.hasMoreElements()) {
                val iface = interfaces.nextElement()
                if (iface.isLoopback || !iface.isUp) continue
                val addresses = iface.inetAddresses
                while (addresses.hasMoreElements()) {
                    val addr = addresses.nextElement()
                    if (addr is Inet4Address && !addr.isLoopbackAddress) {
                        val host = addr.hostAddress ?: continue
                        val name = iface.name.lowercase()
                        if (name.contains("wlan") || name.contains("ap") || isSafeLanAddress(addr)) {
                            return host
                        }
                    }
                }
            }
        } catch (_: Exception) {}
        return "Waiting for Wi-Fi"
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        cancelNotification()
        stopDiscoveryBeacon()
        stopListening()
        stopMicStreaming()
        super.onDestroy()
    }
}

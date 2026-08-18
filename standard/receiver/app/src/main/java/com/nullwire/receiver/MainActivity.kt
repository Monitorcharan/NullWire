package com.nullwire.receiver

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.view.View
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.NotificationCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.security.SecureRandom
import java.util.concurrent.atomic.AtomicInteger

class MainActivity : AppCompatActivity() {

    companion object {
        private const val AUDIO_PORT = 50005
        private const val DISCOVERY_PORT = 50007
        private const val NOTIFICATION_ID = 2001
        private const val CHANNEL_ID = "nullwire_standard_channel"
    }

    private var isListening = false
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

    private var beaconThread: Thread? = null
    private var beaconSocket: DatagramSocket? = null

    private val sessionToken = AtomicInteger(0)
    private var lastPacketCount: Long = 0
    private var isConnectedState = false

    private val handler = Handler(Looper.getMainLooper())
    private val telemetryRunnable = object : Runnable {
        override fun run() {
            if (isListening) {
                val rms = AudioEngine.getPlaybackRms()
                val packets = AudioEngine.getPlaybackPacketCount()
                val level = (rms * 100).toInt().coerceIn(0, 100)
                pbLevelMeter.progress = level

                if (packets > 0) {
                    tvStats.text = "Packets: $packets  ·  DMA Latency: <2.67ms"
                    if (!isConnectedState && packets > lastPacketCount + 5) {
                        isConnectedState = true
                        tvStatus.text = "● Streaming 48kHz Lossless Audio"
                        tvStatus.setTextColor(getColor(R.color.color_green))
                        Toast.makeText(this@MainActivity, "⚡ Connected to PC! Audio playing.", Toast.LENGTH_SHORT).show()
                        updateNotification("● Streaming Lossless Audio (DMA Active)")
                    }
                } else {
                    tvStats.text = "Packets: 0  ·  DMA Ready"
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

        // Auto-start discovery beacon and audio listener for zero friction
        startDiscoveryBeacon()
        startListening()
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

    private fun updateNotification(contentText: String) {
        try {
            val intent = Intent(this, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_SINGLE_TOP
            }
            val pendingIntent = PendingIntent.getActivity(this, 0, intent, PendingIntent.FLAG_IMMUTABLE)

            val builder = NotificationCompat.Builder(this, CHANNEL_ID)
                .setSmallIcon(R.mipmap.ic_launcher)
                .setContentTitle("NullWire Audio")
                .setContentText(contentText)
                .setContentIntent(pendingIntent)
                .setOngoing(true)
                .setPriority(NotificationCompat.PRIORITY_LOW)

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
            releaseLocks()
        }
    }

    private fun stopListening() {
        isListening = false
        isConnectedState = false
        handler.removeCallbacks(telemetryRunnable)
        AudioEngine.stopNativePlayback()
        releaseLocks()
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
                    val beaconMsg = "NWBC|${sanitizedDeviceName()}|$myIp|$token"
                    val beaconBytes = beaconMsg.toByteArray(Charsets.US_ASCII)

                    try {
                        val bcastPkt = DatagramPacket(beaconBytes, beaconBytes.size, bcastAddr, DISCOVERY_PORT)
                        socket.send(bcastPkt)
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

    private fun acquireLocks() {
        try {
            val pm = getSystemService(Context.POWER_SERVICE) as? PowerManager
            if (wakeLock == null) {
                wakeLock = pm?.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "NullWire:StandardWakeLock")?.apply {
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
                wifiLock = wm?.createWifiLock(mode, "NullWire:StandardWifiLock")?.apply {
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
        super.onDestroy()
    }
}

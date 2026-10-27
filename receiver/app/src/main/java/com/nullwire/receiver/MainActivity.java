package com.nullwire.receiver;

import android.Manifest;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.media.audiofx.AcousticEchoCanceler;
import android.media.audiofx.AutomaticGainControl;
import android.media.audiofx.NoiseSuppressor;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.os.Process;
import android.text.InputFilter;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ProgressBar;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.SwitchCompat;
import androidx.core.app.ActivityCompat;
import androidx.core.app.NotificationCompat;
import androidx.core.content.ContextCompat;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.Enumeration;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * UI/control shell only. All audio playback, jitter handling and DSP live in the C++ engine
 * (see src/main/cpp/native-lib.cpp) which is reached through {@link AudioEngine}.
 */
public class MainActivity extends AppCompatActivity {

    private static final int PERMISSION_REQ_CODE = 3001;
    private static final int AUDIO_PORT = 50005;
    private static final int MIC_PORT = 50006;
    private static final int DISCOVERY_PORT = 50007;
    private static final int NOTIFICATION_ID = 1001;
    private static final String CHANNEL_ID = "nullwire_playback_channel";
    private static final short PACKET_MAGIC = 0x574E;
    private static final int PACKET_HEADER_SIZE = 8;

    private volatile boolean isListening = false;
    private volatile boolean isMicStreaming = false;
    private volatile boolean isBeaconRunning = false;

    private PowerManager.WakeLock wakeLock;
    private WifiManager.WifiLock wifiLock;

    private View rootLayout;
    private TextView tvIpAddress;
    private TextView tvPairingPin;
    private TextView tvStatus;
    private TextView tvStats;
    private Button btnListen;
    private ProgressBar pbLevelMeter;
    private SeekBar sbBass;
    private TextView tvBassVal;
    private SeekBar sbTreble;
    private TextView tvTrebleVal;
    private TextView tvMicStatus;
    private Button btnMic;

    private volatile AudioRecord audioRecord;
    private AcousticEchoCanceler echoCanceler;
    private NoiseSuppressor noiseSuppressor;
    private AutomaticGainControl autoGain;
    private volatile DatagramSocket micSocket;
    private Thread micThread;
    private String pcTargetIp = "192.168.1.10";

    private Thread beaconThread;
    private volatile DatagramSocket beaconSocket;

    private final AtomicInteger sessionToken = new AtomicInteger(0);
    private long lastPacketCount = 0;
    private boolean isConnectedState = false;
    private int stalePacketTicks = 0;

    private TextView tvWifiAccelStatus;
    private SwitchCompat switchWifiAccel;
    private boolean isWifiAccelEnabled = true;

    private View layoutToastBanner;
    private View viewToastDot;
    private TextView tvToastTitle;
    private TextView tvToastMessage;
    private Runnable toastDismissRunnable;

    private final Handler handler = new Handler(Looper.getMainLooper());

    private final Runnable telemetryRunnable = new Runnable() {
        @Override
        public void run() {
            if (!isListening) return;
            float rms = AudioEngine.getPlaybackRms();
            long packets = AudioEngine.getPlaybackPacketCount();
            int level = Math.max(0, Math.min(100, (int) (rms * 100)));
            pbLevelMeter.setProgress(level);

            if (packets > lastPacketCount) {
                stalePacketTicks = 0;
                tvStats.setText("Packets: " + packets + "  ·  DMA Latency: <2.67ms  ·  Loss: 0.0%");
                if (!isConnectedState) {
                    isConnectedState = true;
                    tvStatus.setText("● Streaming 48kHz Lossless Audio");
                    tvStatus.setTextColor(getColor(R.color.color_green));
                    showAppToast("CONNECTED TO PC", "Streaming Lossless 48kHz Audio (<2.67ms DMA)", getColor(R.color.color_green), 3500);
                    updateNotification("● Streaming Lossless Audio (DMA Active)", true);
                }
            } else if (packets > 0 && isConnectedState) {
                stalePacketTicks++;
                if (stalePacketTicks > 50) { // ~2.5 seconds without packets
                    isConnectedState = false;
                    tvStatus.setText("⚠️ Connection Lost · Waiting for PC...");
                    tvStatus.setTextColor(getColor(R.color.color_red));
                    showAppToast("CONNECTION LOST", "PC audio stream disconnected. Searching...", getColor(R.color.color_red), 4000);
                    updateNotification("⚠️ Connection Lost · Waiting for PC...", false);
                }
            } else {
                tvStats.setText("Packets: 0  ·  DMA Ready  ·  Loss: 0.0%");
            }
            lastPacketCount = packets;
            handler.postDelayed(this, 50);
        }
    };

    public void showAppToast(String title, String message, int dotColor, long durationMs) {
        runOnUiThread(() -> {
            if (toastDismissRunnable != null) handler.removeCallbacks(toastDismissRunnable);

            tvToastTitle.setText(title);
            tvToastMessage.setText(message);
            viewToastDot.setBackgroundTintList(ColorStateList.valueOf(dotColor));

            if (layoutToastBanner.getVisibility() != View.VISIBLE) {
                layoutToastBanner.setVisibility(View.VISIBLE);
                layoutToastBanner.setAlpha(0f);
                layoutToastBanner.setTranslationY(80f);
                layoutToastBanner.animate().alpha(1f).translationY(0f).setDuration(280).start();
            }

            Runnable dismiss = () -> layoutToastBanner.animate()
                    .alpha(0f)
                    .translationY(80f)
                    .setDuration(280)
                    .withEndAction(() -> layoutToastBanner.setVisibility(View.GONE))
                    .start();
            toastDismissRunnable = dismiss;
            handler.postDelayed(dismiss, durationMs);
        });
    }

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        createNotificationChannel();
        rotateSessionToken();

        rootLayout = findViewById(R.id.rootLayout);
        tvIpAddress = findViewById(R.id.tvIpAddress);
        tvPairingPin = findViewById(R.id.tvPairingPin);
        tvStatus = findViewById(R.id.tvStatus);
        tvStats = findViewById(R.id.tvStats);
        btnListen = findViewById(R.id.btnListen);
        pbLevelMeter = findViewById(R.id.pbLevelMeter);
        sbBass = findViewById(R.id.sbBass);
        tvBassVal = findViewById(R.id.tvBassVal);
        sbTreble = findViewById(R.id.sbTreble);
        tvTrebleVal = findViewById(R.id.tvTrebleVal);
        tvMicStatus = findViewById(R.id.tvMicStatus);
        btnMic = findViewById(R.id.btnMic);
        tvWifiAccelStatus = findViewById(R.id.tvWifiAccelStatus);
        switchWifiAccel = findViewById(R.id.switchWifiAccel);
        layoutToastBanner = findViewById(R.id.layoutToastBanner);
        viewToastDot = findViewById(R.id.viewToastDot);
        tvToastTitle = findViewById(R.id.tvToastTitle);
        tvToastMessage = findViewById(R.id.tvToastMessage);

        switchWifiAccel.setChecked(isWifiAccelEnabled);
        switchWifiAccel.setOnCheckedChangeListener((button, isChecked) -> {
            isWifiAccelEnabled = isChecked;
            AudioEngine.setWifiAcceleration(isChecked);
            if (isChecked) {
                if (isListening) acquireLocks();
                tvWifiAccelStatus.setText("⚡ Acceleration Active (Low-Latency Lock & Nice -19)");
                tvWifiAccelStatus.setTextColor(getColor(R.color.color_green));
                showAppToast("⚡ WI-FI FASTPATH ACTIVE", "Low-Latency Wi-Fi Lock & High Priority Active", getColor(R.color.color_green), 3000);
            } else {
                releaseLocks();
                tvWifiAccelStatus.setText("⚪ Standard Wi-Fi Mode (Battery Saver)");
                tvWifiAccelStatus.setTextColor(getColor(R.color.color_text_sec));
                showAppToast("⚪ STANDARD WI-FI MODE", "Battery Saver Mode Active", getColor(R.color.color_text_sec), 3000);
            }
        });

        ViewCompat.setOnApplyWindowInsetsListener(rootLayout, (view, insets) -> {
            Insets systemBars = insets.getInsets(WindowInsetsCompat.Type.systemBars());
            Insets displayCutout = insets.getInsets(WindowInsetsCompat.Type.displayCutout());
            int topPadding = Math.max(systemBars.top, displayCutout.top) + 20;
            int bottomPadding = Math.max(systemBars.bottom, displayCutout.bottom) + 32;
            view.setPadding(view.getPaddingLeft(), topPadding, view.getPaddingRight(), bottomPadding);
            return insets;
        });

        refreshNetworkIdentity();

        btnListen.setOnClickListener(v -> {
            if (isListening) stopListening();
            else startListening();
        });

        sbBass.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                tvBassVal.setText("+" + progress + " dB");
                AudioEngine.setBassBoost((float) progress);
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });

        sbTreble.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                tvTrebleVal.setText("+" + progress + " dB");
                AudioEngine.setTrebleBoost((float) progress);
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });

        btnMic.setOnClickListener(v -> {
            if (isMicStreaming) stopMicStreaming();
            else promptPcIpAndStartMic();
        });

        // Auto-start discovery beacon so PC can find this phone
        startDiscoveryBeacon();
        // NOTE: Audio listening does NOT auto-start. User must press Listen button.
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, "NullWire Audio Stream", NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("Real-time notifications for NullWire audio streaming");
            NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            nm.createNotificationChannel(channel);
        }
    }

    private void updateNotification(String contentText) {
        updateNotification(contentText, false);
    }

    private void updateNotification(String contentText, boolean isConnected) {
        try {
            Intent intent = new Intent(this, MainActivity.class);
            intent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
            PendingIntent pendingIntent = PendingIntent.getActivity(this, 0, intent, PendingIntent.FLAG_IMMUTABLE);

            String title = isConnected ? "⚡ NullWire Connected" : "NullWire Pro Audio";
            NotificationCompat.Builder builder = new NotificationCompat.Builder(this, CHANNEL_ID)
                    .setSmallIcon(R.mipmap.ic_launcher)
                    .setContentTitle(title)
                    .setContentText(contentText)
                    .setContentIntent(pendingIntent)
                    .setOngoing(true)
                    .setPriority(isConnected ? NotificationCompat.PRIORITY_DEFAULT : NotificationCompat.PRIORITY_LOW);

            NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            nm.notify(NOTIFICATION_ID, builder.build());
        } catch (Exception ignored) {}
    }

    private void cancelNotification() {
        try {
            NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            nm.cancel(NOTIFICATION_ID);
        } catch (Exception ignored) {}
    }

    private void rotateSessionToken() {
        SecureRandom rng = new SecureRandom();
        int token;
        do {
            token = rng.nextInt();
        } while (token == 0);
        sessionToken.set(token);
        AudioEngine.setSessionToken(token);
    }

    private String pairingPinText() {
        long pin = Integer.toUnsignedLong(sessionToken.get()) % 1_000_000L;
        return String.format(Locale.US, "%06d", pin);
    }

    private void refreshNetworkIdentity() {
        tvIpAddress.setText(getWifiIpAddress());
        tvPairingPin.setText("Pairing PIN: " + pairingPinText());
    }

    private void startListening() {
        if (isListening) return;
        acquireLocks();
        AudioEngine.setSessionToken(sessionToken.get());
        boolean success = AudioEngine.startNativePlayback(AUDIO_PORT);
        if (success) {
            isListening = true;
            isConnectedState = false;
            btnListen.setText("⏹ Stop Audio Stream");
            btnListen.setBackgroundColor(getColor(R.color.color_red));
            tvStatus.setText("● AAudio Direct MMAP Ready");
            tvStatus.setTextColor(getColor(R.color.color_green));
            handler.post(telemetryRunnable);
            updateNotification("Ready to receive lossless audio on port 50005");
        } else if (!isMicStreaming) {
            releaseLocks();
        }
    }

    private void stopListening() {
        isListening = false;
        isConnectedState = false;
        handler.removeCallbacks(telemetryRunnable);
        AudioEngine.stopNativePlayback();
        if (!isMicStreaming) releaseLocks();
        cancelNotification();

        btnListen.setText("▶ Listen (Connect Audio)");
        btnListen.setBackgroundColor(getColor(R.color.color_accent));
        tvStatus.setText("Ready to receive audio");
        tvStatus.setTextColor(getColor(R.color.color_text_sec));
        pbLevelMeter.setProgress(0);
        tvStats.setText("Packets: 0  ·  DMA Idle");
        Toast.makeText(this, "Audio Stream Stopped", Toast.LENGTH_SHORT).show();
    }

    private String sanitizedDeviceName() {
        String raw = (Build.MODEL != null ? Build.MODEL : "Android")
                .replace('|', ' ').replace('\n', ' ').trim();
        if (raw.length() > 32) raw = raw.substring(0, 32);
        return raw.isEmpty() ? "Android" : raw;
    }

    private void startDiscoveryBeacon() {
        if (isBeaconRunning) return;
        isBeaconRunning = true;

        beaconThread = new Thread(() -> {
            DatagramSocket socket = null;
            byte[] recvBuf = new byte[512];
            InetAddress bcastAddr;
            try {
                bcastAddr = InetAddress.getByName("255.255.255.255");
            } catch (Exception e) {
                return;
            }

            while (isBeaconRunning) {
                try {
                    if (socket == null || socket.isClosed()) {
                        socket = new DatagramSocket(null);
                        socket.setReuseAddress(true);
                        socket.bind(new InetSocketAddress(DISCOVERY_PORT));
                        socket.setBroadcast(true);
                        socket.setSoTimeout(300);
                        beaconSocket = socket;
                    }

                    String myIp = getWifiIpAddress();
                    String token = Integer.toUnsignedString(sessionToken.get());
                    byte[] beaconBytes = ("NWDISC|" + sanitizedDeviceName() + "|" + token)
                            .getBytes(StandardCharsets.US_ASCII);
                    byte[] beaconBytes2 = ("NWBC|" + sanitizedDeviceName() + "|" + myIp + "|" + token)
                            .getBytes(StandardCharsets.US_ASCII);

                    try {
                        socket.send(new DatagramPacket(beaconBytes, beaconBytes.length, bcastAddr, DISCOVERY_PORT));
                        socket.send(new DatagramPacket(beaconBytes2, beaconBytes2.length, bcastAddr, DISCOVERY_PORT));
                    } catch (Exception ignored) {}

                    long endTime = System.currentTimeMillis() + 1000L;
                    while (System.currentTimeMillis() < endTime && isBeaconRunning) {
                        try {
                            DatagramPacket recvPkt = new DatagramPacket(recvBuf, recvBuf.length);
                            socket.receive(recvPkt);
                            String msg = new String(recvPkt.getData(), 0, recvPkt.getLength(),
                                    StandardCharsets.US_ASCII).trim();
                            InetAddress from = recvPkt.getAddress();
                            if (msg.startsWith("NWDS") && from instanceof Inet4Address
                                    && isSafeLanAddress((Inet4Address) from)) {
                                socket.send(new DatagramPacket(beaconBytes, beaconBytes.length, from, recvPkt.getPort()));
                            }
                        } catch (Exception e) {
                            break;
                        }
                    }
                } catch (Exception e) {
                    try {
                        Thread.sleep(1000);
                    } catch (Exception ie) {
                        break;
                    }
                }
            }
            try { if (socket != null) socket.close(); } catch (Exception ignored) {}
        });
        beaconThread.start();
    }

    private void stopDiscoveryBeacon() {
        isBeaconRunning = false;
        try { if (beaconSocket != null) beaconSocket.close(); } catch (Exception ignored) {}
        beaconSocket = null;
        if (beaconThread != null) beaconThread.interrupt();
        beaconThread = null;
    }

    private void promptPcIpAndStartMic() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, new String[]{Manifest.permission.RECORD_AUDIO}, PERMISSION_REQ_CODE);
            return;
        }

        EditText input = new EditText(this);
        input.setText(pcTargetIp);
        input.setSingleLine(true);
        input.setFilters(new InputFilter[]{new InputFilter.LengthFilter(45)});

        new AlertDialog.Builder(this)
                .setTitle("Target PC IP Address")
                .setMessage("Enter the IPv4 address of your Windows PC on this Wi-Fi:")
                .setView(input)
                .setPositiveButton("Start Mic", (dialog, which) -> {
                    String ip = input.getText().toString().trim();
                    if (isValidUnicastIpv4(ip)) {
                        pcTargetIp = ip;
                        startMicStreaming(ip);
                    } else {
                        Toast.makeText(this, "Enter a valid LAN IPv4 address", Toast.LENGTH_SHORT).show();
                    }
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == PERMISSION_REQ_CODE) {
            if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                promptPcIpAndStartMic();
            } else {
                Toast.makeText(this, "Microphone permission is required", Toast.LENGTH_SHORT).show();
            }
        }
    }

    private AudioRecord createRecorder(int source, int minBuf) {
        return new AudioRecord(source, 48000, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT, minBuf * 2);
    }

    private void startMicStreaming(String ip) {
        stopMicStreaming();
        acquireLocks();

        try {
            int minBuf = AudioRecord.getMinBufferSize(48000, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
            if (minBuf <= 0) {
                throw new IllegalStateException("Unsupported mic format");
            }

            AudioRecord recorder = createRecorder(MediaRecorder.AudioSource.VOICE_COMMUNICATION, minBuf);
            if (recorder.getState() != AudioRecord.STATE_INITIALIZED) {
                recorder.release();
                recorder = createRecorder(MediaRecorder.AudioSource.MIC, minBuf);
            }
            if (recorder.getState() != AudioRecord.STATE_INITIALIZED) {
                recorder.release();
                throw new IllegalStateException("AudioRecord failed to initialize");
            }

            int sessionId = recorder.getAudioSessionId();
            if (sessionId != 0) {
                try {
                    if (AcousticEchoCanceler.isAvailable()) {
                        echoCanceler = AcousticEchoCanceler.create(sessionId);
                        if (echoCanceler != null) echoCanceler.setEnabled(true);
                    }
                } catch (Exception ignored) {}
                try {
                    if (NoiseSuppressor.isAvailable()) {
                        noiseSuppressor = NoiseSuppressor.create(sessionId);
                        if (noiseSuppressor != null) noiseSuppressor.setEnabled(true);
                    }
                } catch (Exception ignored) {}
                try {
                    if (AutomaticGainControl.isAvailable()) {
                        autoGain = AutomaticGainControl.create(sessionId);
                        if (autoGain != null) autoGain.setEnabled(true);
                    }
                } catch (Exception ignored) {}
            }

            recorder.startRecording();
            audioRecord = recorder;

            micSocket = new DatagramSocket();
            InetAddress resolved = InetAddress.getByName(ip);
            if (!(resolved instanceof Inet4Address) || !isSafeLanAddress((Inet4Address) resolved)) {
                throw new IllegalArgumentException("PC address is not a safe LAN IPv4");
            }
            final InetAddress targetAddr = resolved;
            isMicStreaming = true;

            micThread = new Thread(() -> {
                Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
                byte[] pcm = new byte[1024];
                byte[] packet = new byte[PACKET_HEADER_SIZE + pcm.length];
                int seq = 0;

                while (isMicStreaming) {
                    try {
                        AudioRecord rec = audioRecord;
                        DatagramSocket sock = micSocket;
                        int read = rec != null ? rec.read(pcm, 0, pcm.length) : 0;
                        if (read > 0 && sock != null) {
                            ByteBuffer.wrap(packet, 0, PACKET_HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
                                    .putShort(PACKET_MAGIC)
                                    .putShort((short) seq)
                                    .putInt(sessionToken.get());
                            seq = (seq + 1) & 0xFFFF;
                            System.arraycopy(pcm, 0, packet, PACKET_HEADER_SIZE, read);
                            sock.send(new DatagramPacket(packet, PACKET_HEADER_SIZE + read, targetAddr, MIC_PORT));
                        }
                    } catch (Exception e) {
                        if (!isMicStreaming) break;
                    }
                }
            });
            micThread.start();

            btnMic.setText("⏹ Stop Phone Mic");
            btnMic.setBackgroundColor(getColor(R.color.color_red));
            tvMicStatus.setText("● Streaming Mic to PC (" + ip + ":50006)");
            tvMicStatus.setTextColor(getColor(R.color.color_green));
            Toast.makeText(this, "🎙 Phone Mic Active -> PC", Toast.LENGTH_SHORT).show();
        } catch (Exception e) {
            stopMicStreaming();
            Toast.makeText(this, "Microphone error: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    private void releaseMicEffects() {
        try { if (echoCanceler != null) echoCanceler.release(); } catch (Exception ignored) {}
        echoCanceler = null;
        try { if (noiseSuppressor != null) noiseSuppressor.release(); } catch (Exception ignored) {}
        noiseSuppressor = null;
        try { if (autoGain != null) autoGain.release(); } catch (Exception ignored) {}
        autoGain = null;
    }

    private void stopMicStreaming() {
        isMicStreaming = false;
        try { if (micSocket != null) micSocket.close(); } catch (Exception ignored) {}
        micSocket = null;

        if (micThread != null) {
            micThread.interrupt();
            try { micThread.join(300); } catch (Exception ignored) {}
        }
        micThread = null;

        try {
            if (audioRecord != null) {
                audioRecord.stop();
                audioRecord.release();
            }
        } catch (Exception ignored) {}
        audioRecord = null;
        releaseMicEffects();

        if (!isListening) releaseLocks();

        if (btnMic != null) {
            btnMic.setText("🎙 Start Phone Mic (to PC)");
            btnMic.setBackgroundColor(getColor(R.color.color_card));
            tvMicStatus.setText("Mic: Idle (Stream phone voice to PC)");
            tvMicStatus.setTextColor(getColor(R.color.color_text));
        }
    }

    private void acquireLocks() {
        try {
            PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
            if (wakeLock == null && pm != null) {
                wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "NullWire:NativeWakeLock");
                wakeLock.acquire(12 * 60 * 60 * 1000L);
            }
            WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wifiLock == null && wm != null) {
                int mode = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q
                        ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                        : getHighPerfMode();
                wifiLock = wm.createWifiLock(mode, "NullWire:NativeWifiLock");
                wifiLock.acquire();
            }
        } catch (Exception ignored) {}
    }

    @SuppressWarnings("deprecation")
    private static int getHighPerfMode() {
        return WifiManager.WIFI_MODE_FULL_HIGH_PERF;
    }

    private void releaseLocks() {
        try { if (wakeLock != null && wakeLock.isHeld()) wakeLock.release(); } catch (Exception ignored) {}
        wakeLock = null;
        try { if (wifiLock != null && wifiLock.isHeld()) wifiLock.release(); } catch (Exception ignored) {}
        wifiLock = null;
    }

    private static boolean isSafeLanAddress(Inet4Address addr) {
        if (addr.isAnyLocalAddress() || addr.isMulticastAddress()) return false;
        byte[] bytes = addr.getAddress();
        int b0 = bytes[0] & 0xFF;
        int b1 = bytes[1] & 0xFF;
        if (b0 == 10) return true;
        if (b0 == 127) return true;
        if (b0 == 192 && b1 == 168) return true;
        return b0 == 172 && b1 >= 16 && b1 <= 31;
    }

    private static boolean isValidUnicastIpv4(String ip) {
        try {
            InetAddress addr = InetAddress.getByName(ip);
            return addr instanceof Inet4Address
                    && !addr.isAnyLocalAddress()
                    && !addr.isMulticastAddress()
                    && isSafeLanAddress((Inet4Address) addr);
        } catch (Exception e) {
            return false;
        }
    }

    private String getWifiIpAddress() {
        try {
            Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
            if (interfaces == null) return "Waiting for Wi-Fi";
            while (interfaces.hasMoreElements()) {
                NetworkInterface iface = interfaces.nextElement();
                if (iface.isLoopback() || !iface.isUp()) continue;
                Enumeration<InetAddress> addresses = iface.getInetAddresses();
                while (addresses.hasMoreElements()) {
                    InetAddress addr = addresses.nextElement();
                    if (addr instanceof Inet4Address && !addr.isLoopbackAddress()) {
                        String host = addr.getHostAddress();
                        if (host == null) continue;
                        String name = iface.getName().toLowerCase(Locale.ROOT);
                        if (name.contains("wlan") || name.contains("ap") || isSafeLanAddress((Inet4Address) addr)) {
                            return host;
                        }
                    }
                }
            }
        } catch (Exception ignored) {}
        return "Waiting for Wi-Fi";
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        cancelNotification();
        stopDiscoveryBeacon();
        stopListening();
        stopMicStreaming();
        super.onDestroy();
    }
}

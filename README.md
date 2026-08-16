# ⚡ NullWire Pro — Production-Grade Network Audio Suite

**NullWire Pro** is a high-performance, **100% Native C++ (Zero Python, Zero Flutter)** bidirectional audio bridge designed for lossless audio transmission between a Windows PC and an Android device over 5GHz Wi-Fi.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           END-TO-END NATIVE PIPELINE                        │
├─────────────────────────────────────────────────────────────────────────────┤
│  PC (C++20 WASAPI + MMCSS Priority 31) ──► 5GHz Wi-Fi (DSCP 46 Voice QoS)  │
│  ──► Android (C++ NDK AAudio Direct MMAP DMA ──► Hardware Headphone DAC)    │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## ✨ Features

- **⚡ Sub-5ms Glass-to-Glass Latency:** Direct hardware MMAP DMA on Android and real-time MMCSS Priority 31 scheduling on Windows.
- **📊 Real-Time Network Latency & Jitter Tracker Graph:** Live oscilloscope waveform tracking packet delivery intervals in milliseconds (Current, Min, Max, Jitter).
- **🖥️ 100% Flicker-Free & Resizable Native UI:** Double-buffered hardware GDI memory rendering with support for minimize, maximize, and responsive drag resizing.
- **📱 Notch & Camera Cutout Protection:** Android app uses dynamic WindowInsets to prevent content from overlapping camera punch-holes or notches.
- **🎙️ Studio-Grade Reverse Microphone Streaming:** Streams phone headset or built-in mic to PC (UDP `50006`) with hardware Acoustic Echo Cancellation (AEC), Automatic Gain Control (AGC), and Noise Suppression (NS).
- **🔊 4 Dedicated Acoustic Scenario Profiles:**
  - 🎮 **Gaming Mode:** $2.67\,\text{ms}$ ultra-fast packet intervals + $2.8\,\text{kHz}$ spatial footstep clarity boost.
  - 🎵 **Hi-Fi Music:** Harman Target Audiophile curve + deep sub-bass extension ($80\,\text{Hz}$) + high-res air shimmer ($12\,\text{kHz}$).
  - 🎬 **Cinema Movie:** Dialogue intelligibility ($1.5\text{–}2.5\,\text{kHz}$) + explosive cinematic sub-bass rumble.
  - 🎯 **Pure Bit-Perfect Direct:** Exact bit-for-bit unadulterated pass-through (0 DSP) for pure studio reference.
- **📦 Windows Setup Wizard Installer:** [`NullWire_Setup.exe`](./NullWire_Setup.exe) installs NullWire Pro permanently with Desktop & Start Menu shortcuts, uninstaller, and Windows Add/Remove Programs integration.

---

## 📦 Production Binaries

| Application | Binary | Size | Purpose |
| :--- | :--- | :--- | :--- |
| **Windows Installer** | [`NullWire_Setup.exe`](./NullWire_Setup.exe) | **3.1 MB** | 1-Click Windows Setup Wizard (Permanent PC Install) |
| **Windows Standalone App** | [`NullWireSender.exe`](./NullWireSender.exe) | **886 KB** | Portable C++20 WASAPI MMCSS Real-Time Streamer |
| **Android Receiver** | [`NullWireReceiver.apk`](./NullWireReceiver.apk) | **13.0 MB** | 100% Native C++ NDK AAudio Direct MMAP DMA App |

---

## 🚀 Quick Start Guide

### 1. Windows PC Installation
* Run [`NullWire_Setup.exe`](./NullWire_Setup.exe) to install NullWire Pro to your PC with Desktop & Start Menu shortcuts.
* *(Or simply run [`NullWireSender.exe`](./NullWireSender.exe) portably without installation).*

### 2. Android Phone Setup
1. Transfer and install [`NullWireReceiver.apk`](./NullWireReceiver.apk) on your Android phone.
2. Open **NullWire Pro** and tap **Listen (Connect Audio)**.
3. Note your phone's displayed **Wi-Fi IP address** (e.g., `192.168.1.15`).

### 3. Connect & Enjoy
1. In the Windows app, select your **PC Audio Source Device** and **Acoustic Scenario Profile**.
2. Enter your phone's IP address and click **Start Audio Stream**.
3. Watch the **Real-Time Latency Tracker Graph** report sub-5ms packet delivery!
4. *(Optional)* Click **Start Phone Mic** on your phone to use your phone/earphones as a wireless PC microphone.

---

## 📄 License

NullWire is licensed under the **[GNU General Public License v3.0 (GPLv3)](./LICENSE)**.  
Free and open-source for personal use, forever.


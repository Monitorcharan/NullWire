# 🚀 How to Deploy NullWire to Render (render.com)

NullWire is fully configured for deployment on [Render](https://render.com).

---

## ⚡ Method 1: Automatic 1-Click Deployment (Recommended)

Render automatically detects the [`render.yaml`](../render.yaml) file in this repository.

1. **Push your code to GitHub:**
   ```bash
   git add .
   git commit -m "Add NullWire website and downloads for Render"
   git push origin main
   ```
2. **Go to [dashboard.render.com](https://dashboard.render.com)**.
3. Click **New +** ➔ **Blueprint**.
4. Connect your GitHub repository.
5. Render will automatically read `render.yaml`, configure the `nullwire` web service, and deploy it!

---

## 🛠️ Method 2: Manual Web Service on Render

If you prefer setting it up manually in the Render dashboard:

1. Go to [dashboard.render.com](https://dashboard.render.com) ➔ Click **New +** ➔ **Web Service**.
2. Connect your GitHub repository.
3. Configure the following settings:
   * **Name:** `nullwire`
   * **Region:** Any (e.g. Oregon, Frankfurt, Singapore)
   * **Branch:** `main`
   * **Root Directory:** `website`
   * **Runtime:** `Node`
   * **Build Command:** `npm install`
   * **Start Command:** `npm start`
   * **Instance Type:** `Free`
4. Click **Create Web Service**.

---

## 📦 What Happens on Render

* **Live Website:** Render will host the landing page with SSL (`https://nullwire-xxxx.onrender.com`).
* **Direct Binary Downloads:** Users clicking the download buttons will download the `.exe` and `.apk` files directly from your Render URL with binary streaming headers (`Content-Disposition: attachment`).
* **All Files Included in `/downloads`:**
  * `NullWire_Setup.exe` (Pro Studio Setup)
  * `NullWireReceiver.apk` (Pro Studio APK)
  * `NullWireSender.exe` (Pro Portable)
  * `NullWire_Standard_Setup.exe` (Standard Setup)
  * `NullWire_Standard.apk` (Standard APK)
  * `NullWire_Standard.exe` (Standard Portable)

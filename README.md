# Light strip, Android app (v0.1)

Controls the ESP32-C3 light strip lamp over your home WiFi. Designed by Ro-Han G.

## What's in the app

- **Light**: lamp illustration that shows the real colour and brightness, power switch and pull cord,
  volume-style brightness slider, and every mode: Colour, Warm, Cold, White, Rainbow, Blink and Music,
  with colour presets, custom hue and saturation, warm and cold tone, rainbow speed and blink timing.
- **Timers**: On delay and Off delay with presets, custom minutes, a live countdown and Cancel.
- **Note**: edit the OLED note, pick one of the 7 display fonts, alignment and how long before it
  appears, with a preview of the 128x64 screen.
- **WiFi**: current network and signal, saved networks (connect, forget), add a network, forget all.
- **Lamps**: add the lamp by IP address (or esp32led.local), name it, edit it, remove it, and switch
  between several lamps. The app checks the address before saving.

No changes to the lamp firmware are needed. The app calls the same `/api/...` endpoints the lamp's
own web page uses.

## Build the APK

### Option A: GitHub (no software to install)
1. Create a new GitHub repository and upload everything in this folder, including the hidden
   `.github` folder.
2. Open the **Actions** tab. The "Build APK" workflow runs on every push (or press **Run workflow**).
3. When it finishes (about 3 to 5 minutes), open the run and download **LightStrip-v0.1-apk**.
   Unzip it to get `LightStrip-v0.1.apk`.

### Option B: Android Studio
1. File > Open, and pick this folder. Let Gradle sync finish.
2. Build > Build App Bundle(s) / APK(s) > Build APK(s).
3. The APK is in `app/build/outputs/apk/debug/`.

Command line with the Android SDK installed: `./gradlew assembleRelease`.

## Install on your phone
Copy the APK to the phone and open it. Android asks you to allow installs from that app
(Files, Chrome, etc.) the first time. The APK is signed with a development key, which is fine
for your own phones.

## First use
1. Connect the phone to the same WiFi as the lamp.
2. Read the IP address from the lamp's display home screen (or Menu > WiFi details).
3. In the app, enter it under **Lamps** and tap **Connect and save**.

If the lamp joins a different network or your router gives it a new address, edit the lamp under
**Lamps** and enter the new IP address. Reserving the lamp's address in your router's DHCP
settings keeps it the same.

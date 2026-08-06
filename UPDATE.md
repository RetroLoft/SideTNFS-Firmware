# How to Update the SideTNFS Firmware

This guide walks you through updating the firmware on your SideTNFS cartridge, step by step. No prior experience with the Raspberry Pi Pico is needed.

**Before you start, a few things worth knowing:**

- SideTNFS runs on a **Raspberry Pi Pico W**, a small computer built into the cartridge.
- Firmware updates are installed using a **`.uf2` file** — a single file you simply copy onto the Pico.
- You do **not** need any special flashing software. Your computer's normal file manager is enough.
- The SideTNFS cartridge must be **completely disconnected from the Atari** while you update it.
- Always **switch the Atari off** before plugging in or removing the cartridge.

---

## 1. Download the Latest Release

Go to the official releases page:

**https://github.com/RetroLoft/SideTNFS-Firmware/releases**

- The newest version is at the top of the page and marked **Latest**.
- If the files aren't visible right away, click **Assets** to expand the list for that release.
- Download only the normal Production firmware file for the Raspberry Pi Pico W:

  **`sidetnfs_production.uf2`**

**Do not download:**
- "Source code (zip)" or "Source code (tar.gz)" — these are for developers, not for flashing.
- Any file with **debug**, **diagnostic**, **mindiag**, **test**, or a similar word in its name. These are development builds and are not meant for everyday use.

---

## 2. Disconnect SideTNFS from the Atari

1. Switch the Atari **off** completely.
2. Remove SideTNFS from the cartridge port.
3. From here on, keep the cartridge away from the Atari — you'll flash it on its own.

> ⚠️ **Never insert or remove SideTNFS while the Atari is switched on.**

---

## 3. Put the Pico into BOOTSEL Mode

BOOTSEL mode makes the Pico appear as a plain USB drive so you can copy the new firmware onto it.

1. Unplug the USB cable from the Pico, if it's connected.
2. Find the small white **BOOTSEL** button on the Raspberry Pi Pico W module itself. Press and hold it.

   > This is the **BOOTSEL** button built into the Pico module — it is **not** the same as SideTNFS's own **SELECT** button.

3. While still holding BOOTSEL, connect the Pico to your computer with a USB data cable.
4. Release BOOTSEL.
5. A removable USB drive named **RPI-RP2** should appear on your computer, just like a USB stick.

---

## 4. Install the New Firmware

1. Copy or drag the downloaded `sidetnfs_production.uf2` file onto the **RPI-RP2** drive.
2. The drive will usually disappear on its own once the file has been written — this is normal and means the Pico has restarted with the new firmware.
3. Wait a few seconds to be sure.
4. Unplug the USB cable.
5. With the Atari still switched **off**, plug SideTNFS back into the cartridge port.
6. Switch the Atari **on**.
7. Check the version number shown on the Atari's boot screen when SideTNFS starts, and compare it with the version you just downloaded (see the release notes on the releases page).

> If **RPI-RP2** disappears while you're still in the middle of copying the file, that's expected — it almost always just means the Pico has already restarted into the new firmware.

---

## 5. Your Settings

Your Wi-Fi and drive settings are stored in a separate, protected area of the Pico's memory that a normal firmware update never writes to, so they are preserved when you install a new `.uf2` file.

As with any update, it's still good practice to jot down your Wi-Fi and drive settings beforehand, just in case. Occasionally, a release's notes may mention that a factory reset or reconfiguration is needed for that specific version — check the release notes if you're unsure.

---

## Troubleshooting

- **RPI-RP2 doesn't appear:** Try again, but hold BOOTSEL down *before* plugging in the USB cable, and keep holding it until the drive shows up.
- **Still nothing:** Make sure your USB cable supports data transfer — some cables are charge-only. Try a different USB port, and avoid using a USB hub.
- **RPI-RP2 appears, but copying the file fails or doesn't finish:** Re-download the `.uf2` file (it may not have downloaded correctly) and try again.
- **SideTNFS doesn't start after updating:** Repeat the BOOTSEL steps above and reinstall the latest stable Production release.
- Don't use a debug firmware build to troubleshoot a problem unless a developer specifically asks you to — it's a diagnostic tool, not a fix.

---

Need more help? See the [Community](README.md#community) section in the main README.

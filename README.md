# DS4 to Xbox 360 Bridge (For Knockoff DS4 Controllers)

<div align="center">
  <a href="https://github.com/engineer948/ds4-bridge-linux/releases/download/0.5/ds4_bridge_tray">
    <img src="https://img.shields.io/badge/Download-Tray_App_Executable-blue?style=for-the-badge&logo=c" alt="Download Tray App Executable" />
  </a>
  <br>
  <em>(Click the button to download the executable, or see the one-click terminal command below)</em>
  <br><br>
  <b>⚠️ Important:</b> If you download manually, you must make the file executable by running:<br>
  <code>chmod +x ds4_bridge_tray</code>
</div>

---

This project is a high-performance tool specifically designed to bridge **"knockoff" (fake) Sony DualShock 4 (DS4) controllers that don't work with Bluetooth or aren't recognized properly in Linux** into virtual **Microsoft Xbox 360** controllers. 

**🔥 What's new in v0.5:** Added advanced live **Button Remapping** directly from the Tray Menu (Swap face buttons, L1/R1, and L2/R2 instantly without external tools!). Also includes all v0.4 features: a purely monolithic, lightning-fast C rewrite with zero Python dependencies and no memory leaks.

## ⬇️ Download (One-Click)

To download the GTK Tray app executable instantly into your current folder, run this single command in your terminal:

```bash
wget https://github.com/engineer948/ds4-bridge-linux/releases/download/0.5/ds4_bridge_tray && chmod +x ds4_bridge_tray
```

You can also download the [C Source Code (`ds4_bridge.c`)](https://github.com/engineer948/ds4-bridge-linux/raw/main/ds4_bridge.c) and [`ds4_bridge_tray.c`](https://github.com/engineer948/ds4-bridge-linux/raw/main/ds4_bridge_tray.c) if you want to compile them yourself.

## 📌 What is this and why is it needed?

Some fake or knockoff DS4 controllers are not properly recognized as gamepads by Linux when connected to a computer (especially via cable or their own dongle), or they simply do not work in games. This program reads the signals from these controllers and creates a fully functional virtual Xbox 360 controller in the system, ensuring that games recognize the controller without any issues.

## 🐧 Which Linux Distros are Supported?

This program requires `uinput` v5+ (Linux Kernel 4.5 and newer). It works flawlessly on almost all modern Linux distros:
* **Ubuntu** (20.04, 22.04, 24.04, etc.)
* **Debian** (10, 11, 12)
* **Linux Mint**
* **Fedora**
* **Arch Linux / Manjaro**
* **Pop!_OS** and any other distro with Kernel 4.5+.

## 🛠 Dependencies (For Compilation Only)

If you want to compile the program yourself and use the graphical system tray application, you need to install the required GTK and AppIndicator development headers for your distribution (No dependencies are required just to run the pre-compiled binary!):

### Ubuntu / Debian / Linux Mint / Pop!_OS
```bash
sudo apt update
sudo apt install build-essential libgtk-3-dev libayatana-appindicator3-dev
```
*(Note: If `libayatana-appindicator3-dev` is not found, you can use `libappindicator3-dev` instead).*

### Fedora
```bash
sudo dnf install gcc gtk3-devel libayatana-appindicator-gtk3-devel
```
*(Note: If `libayatana-appindicator-gtk3-devel` is not found, use `libappindicator-gtk3-devel`)*

### Arch Linux / Manjaro
```bash
sudo pacman -S gcc gtk3 libayatana-appindicator
```

## ⚙️ Manual Compilation

Navigate to the project directory in your terminal.

**Compile the background/CLI engine (`ds4_bridge`):**
```bash
gcc -O2 ds4_bridge.c -o ds4_bridge
```

**Compile the GTK Tray Application (`ds4_bridge_tray`):**
```bash
gcc -O2 -std=gnu11 ds4_bridge_tray.c -o ds4_bridge_tray $(pkg-config --cflags --libs gtk+-3.0 ayatana-appindicator3-0.1) -lpthread
```
*(If you are using the legacy appindicator package, replace `ayatana-appindicator3-0.1` with `appindicator3-0.1` and add `-DUSE_LEGACY_APPINDICATOR` to the gcc command).*

## 🚀 Usage

You can run the program in two different ways:

### Method 1: Graphical Interface (Tray App - Recommended)
The most convenient method is to use the standalone Tray application:
```bash
./ds4_bridge_tray
```
* This will create an icon in your top panel (or system tray).
* From there, you can Start/Stop the bridge, monitor controller battery percentage, and view live logs.

### Method 2: From Terminal (CLI)
To run it directly from the terminal (note: sometimes `root` (sudo) permissions may be required or `udev` rules must be added so `/dev/uinput` can be accessed):

```bash
# Run in the foreground:
./ds4_bridge

# Run in the background (as a daemon):
./ds4_bridge -d

# Fix Analog Stick Drift (Deadzone %):
# (e.g. filters out 15% of center stick movement for cheap controllers)
./ds4_bridge -z 15

# View detailed (verbose) logs:
./ds4_bridge -v
```

*(Note: If you receive a `/dev/uinput` error, try running the program as `sudo ./ds4_bridge` or `sudo ./ds4_bridge_tray`).*

## 🛡️ VirusTotal Results

For safety, you can check the VirusTotal results of the project files once they are released:

* [Compiled CLI Executable (ds4_bridge) - 0.5](https://www.virustotal.com/gui/file/c703682bd63ca356d0b0b33d7a72a49ae93fa37a79953745fcc137bc3c5fc574?nocache=1)
* [Compiled Tray Executable (ds4_bridge_tray) - 0.5](https://www.virustotal.com/gui/file/ab33c66923c5c1af6a4a5f3c0211bb58137260e5db40c8ab6ad287b2080a5e5a?nocache=1)
* [C Code (ds4_bridge.c)](https://www.virustotal.com/gui/file/a9eb9f9c5ca37aae0ed4147a5cd2fa017351c567e319d53307ce87ea43f34f8b?nocache=1)

## 📝 Developer & Credits

* **Developer:** [engineer948](https://github.com/engineer948)
* **Repository:** [ds4-bridge-linux](https://github.com/engineer948/ds4-bridge-linux)

### Open Source Acknowledgments
This project is built using and made possible by the following open-source technologies:
* **[Linux Kernel (uinput)](https://www.kernel.org/):** Used for creating the virtual Xbox 360 controller. (GPLv2)
* **[GTK 3](https://www.gtk.org/):** Used for rendering the system tray UI. (LGPL License)

## 🔍 Keywords / Search Tags
`fake ds4 linux`, `knockoff ps4 controller linux`, `ds4 not recognized linux`, `ds4 over bluetooth linux problem`, `dualshock 4 clone linux driver`, `fake ds4 to xbox 360`, `ds4 uinput bridge`, `linux fake ds4 fix`, `ps4 controller clone linux setup`, `fake dualshock 4 bluetooth connection issue linux`, `linux ds4 vibration fix`, `fake ps4 controller rumble not working linux`, `knockoff dualshock 4 force feedback linux`, `ds4 bt connection problem ubuntu`, `third-party ps4 controller linux`, `ds4 spoof xbox 360 linux`, `ds4 evdev to uinput`, `unbranded ds4 controller ubuntu`, `generic ps4 controller linux driver`, `ds4 vibration rumble support linux`, `fake ds4 bluetooth pairing failed`, `cheap ps4 controller linux fix`, `ds4 input mapper linux`, `xpadneo alternative for fake ds4`, `linux ds4 emulation`, `fake ds4 windows vs linux`, `ps4 controller wire connection error linux`, `linux mint ds4 controller not detected`, `arch linux fake dualshock 4`, `ds4 force feedback bridge`, `bluetooth ps4 controller clone linux`, `ds4 bt mac address zero linux fix`

## ⚖️ Legal Disclaimer
This project is an independent, open-source software tool designed for hardware interoperability. It is **not** affiliated with, endorsed, sponsored, or approved by Sony Interactive Entertainment Inc. 

"PlayStation", "DualShock", and "Sony" are registered trademarks of Sony Interactive Entertainment Inc. All other trademarks are the property of their respective owners. The terms "fake" or "knockoff" are used purely for descriptive purposes to refer to third-party, unlicensed, or unbranded controllers that attempt to replicate the functionality of official hardware. This software does not bypass any DRM or piracy protection; it simply reads standard input events and translates them to an Xbox 360 virtual device.

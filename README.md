# DS4 to Xbox 360 Bridge (For Knockoff DS4 Controllers)

<div align="center">
  <a href="https://github.com/engineer948/ds4-bridge-linux/releases/download/0.7/ds4_bridge_tray">
    <img src="https://img.shields.io/badge/Download-Tray_App_Executable 0.7-blue?style=for-the-badge&logo=c" alt="Download Tray App Executable" />
  </a>
  <br>
  <em>(Click the button to download the executable, or see the one-click terminal command below)</em>
  <br><br>
  <b>⚠️ Important:</b> If you download manually, you must make the file executable by running:<br>
  <code>chmod +x ds4_bridge_tray</code>
</div>

---

This project is a high-performance tool specifically designed to bridge **"knockoff" (fake) Sony DualShock 4 (DS4) controllers that don't work with Bluetooth or aren't recognized properly in Linux** into virtual **Microsoft Xbox 360** controllers. 

Games and launchers (Steam, SDL, Proton/Wine, emulators) that only support XInput-style pads will see a standard Xbox 360 controller.

```text
[DS4 /dev/input/eventN] --(buttons / axes)--> bridge --> [/dev/uinput: Xbox 360 pad]
[DS4 /dev/input/eventN] <----(rumble FF)----- bridge <-- [game]
```

## ⬇️ Download (One-Click)

To download the GTK Tray app executable instantly into your current folder, run this single command in your terminal:

```bash
wget https://github.com/engineer948/ds4-bridge-linux/releases/download/0.7/ds4_bridge_tray && chmod +x ds4_bridge_tray
```

You can also download the [C Source Code (`ds4_bridge.c`)](https://github.com/engineer948/ds4-bridge-linux/raw/main/ds4_bridge.c) and [`ds4_bridge_tray.c`](https://github.com/engineer948/ds4-bridge-linux/raw/main/ds4_bridge_tray.c) if you want to compile them yourself.

## 📌 What is this and why is it needed?

Some fake or knockoff DS4 controllers are not properly recognized as gamepads by Linux when connected to a computer (especially via cable or their own dongle), or they simply do not work in games. This program reads the signals from these controllers and creates a fully functional virtual Xbox 360 controller in the system, ensuring that games recognize the controller without any issues.

### ✨ Features

- DS4 buttons, sticks, triggers and D-pad mapped to the Xbox 360 (xpad) layout.
- Rumble / force feedback forwarded from the game to the DS4.
- Exclusive grab of the physical DS4, so games don't see two controllers.
- Automatic reconnect: the controller is picked up again when unplugged and plugged back in.

### 🎮 Tray App Extras

- Start / stop the bridge from the tray menu.
- Up to **4 controllers** at the same time.
- Battery level in the menu, plus a desktop notification below 20%.
- Button remapping (face buttons, bumpers, triggers).
- Log file toggle and built-in log viewer.
- **🚀 Start on Login**: one-click autostart toggle for XDG-compliant desktops (GNOME, KDE, XFCE, etc.).

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
sudo pacman -S base-devel gtk3 libayatana-appindicator
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

## 🔑 Permissions (Run Without Root)

The bridge needs write access to `/dev/uinput` and to the DS4 event device. To use it without `sudo`, add a udev rule:

> **Note:** If you already have **Steam** installed on your Linux system, you might not need to do this! Steam automatically configures these permissions in the background. If the program works for you out of the box, you can skip this step.

```bash
echo 'KERNEL=="uinput", SUBSYSTEM=="misc", TAG+="uaccess", OPTIONS+="static_node=uinput"' \
  | sudo tee /etc/udev/rules.d/60-ds4aze-uinput.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```
*Log out and back in (or reboot) afterwards.*

## 🚀 Usage

You can run the program in two different ways:

### Method 1: Graphical Interface (Tray App - Recommended)
The most convenient method is to use the standalone Tray application:
```bash
./ds4_bridge_tray
```
* This will create an icon in your top panel (or system tray).
* From there, you can Start/Stop the bridge, monitor controller battery percentage, remap buttons, and view live logs.
* **Autostart:** Check **🚀 Start on Login** in the tray menu to automatically launch the bridge when you log in.

If you want to start the tray app and immediately enable the bridge from a script or terminal:
```bash
./ds4_bridge_tray --start
```

### Method 2: From Terminal (CLI)
To run the lightweight engine directly from the terminal:

```bash
# Run in the foreground:
./ds4_bridge

# Run in the background (as a daemon):
./ds4_bridge -d

# Don't grab the DS4 exclusively (games see both devices):
./ds4_bridge -n

# Fix Analog Stick Drift (Deadzone %):
# (e.g. filters out 15% of center stick movement for cheap controllers)
./ds4_bridge -z 15

# View detailed (verbose) logs:
./ds4_bridge -v
```

*(Note: If you receive a `/dev/uinput` error, ensure you have set up the `udev` rule above, or run as `sudo`).*

## 🛡️ VirusTotal Results

For safety, you can check the VirusTotal results of the project files once they are released:

* [Compiled Tray Executable (ds4_bridge_tray) - 0.7](https://www.virustotal.com/gui/file/ca4a35cee078751c9abeaa1ee55e1c94c1c9947e98b123060cfa884361020c56?nocache=1) 


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

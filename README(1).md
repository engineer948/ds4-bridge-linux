# DS4 to Xbox 360 Bridge (For Knockoff DS4 Controllers)

<div align="center">
  <a href="https://github.com/engineer948/ds4-bridge-linux/releases/download/1.0/ds4_bridge_tray">
    <img src="https://img.shields.io/badge/Download-Tray_App_Executable 1.0-blue?style=for-the-badge&logo=c" alt="Download Tray App Executable" />
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

**AppImage (recommended, works on any distro, nothing else to install):**

```bash
wget https://github.com/engineer948/ds4-bridge-linux/releases/download/1.0/DS4_Bridge-1.0-x86_64.AppImage && chmod +x DS4_Bridge-1.0-x86_64.AppImage && ./DS4_Bridge-1.0-x86_64.AppImage
```

**Plain executable** (needs GTK 3, see [Dependencies](#-dependencies)):

```bash
wget https://github.com/engineer948/ds4-bridge-linux/releases/download/1.0/ds4_bridge_tray && chmod +x ds4_bridge_tray
```

You can also download the [C Source Code (`ds4_bridge.c`)](https://github.com/engineer948/ds4-bridge-linux/raw/main/ds4_bridge.c) and [`ds4_bridge_tray.c`](https://github.com/engineer948/ds4-bridge-linux/raw/main/ds4_bridge_tray.c) if you want to compile them yourself.

## 🆕 What's New in 1.0

- **Settings window** with tabs for buttons, sticks & rumble, gyro and desktop mode.
- **Full button remapping**: every DS4 button, including Share / Options / PS, L3 / R3 and the D-pad, can be mapped to any Xbox button or disabled.
- **Per-game profiles** that switch automatically while a game is running.
- **Gyro aiming** with the right stick or the mouse pointer (needs working motion sensors).
- **Desktop mode** (PS + Options): use the controller as a mouse and keyboard.
- **Controller test window** to see stick drift and pick the right deadzone.
- **Round deadzone per stick**, rumble strength, invert Y, swap sticks, digital triggers.
- **Choose the virtual controller**: Xbox 360, Xbox One S or DualShock 4.
- **Battery per controller** with charging indicator.
- **One-click permission fix** for `/dev/uinput`.
- **Languages**: English and Azərbaycanca.
- Log file capped at 1 MiB with a live log viewer.
- Fixes: remapping while playing no longer leaves buttons stuck, the status no longer shows "Running" when `/dev/uinput` can't be opened, and the battery is no longer read from a mouse or headset.

Settings from 0.x are imported automatically.

## 📌 What is this and why is it needed?

Some fake or knockoff DS4 controllers are not properly recognized as gamepads by Linux when connected to a computer (especially via cable or their own dongle), or they simply do not work in games. This program reads the signals from these controllers and creates a fully functional virtual Xbox 360 controller in the system, ensuring that games recognize the controller without any issues.

### ✨ Features

- DS4 buttons, sticks, triggers and D-pad mapped to the Xbox 360 (xpad) layout.
- Rumble / force feedback forwarded from the game to the DS4.
- Exclusive grab of the physical DS4, so games don't see two controllers.
- Automatic reconnect: the controller is picked up again when unplugged and plugged back in.

### 🎮 Tray App Extras

- Start / stop the bridge from the tray menu.
- Up to **4 controllers** at the same time, each with its own status line and battery level (⚡ while charging), plus a desktop notification below 20%.
- **Button remapping** for every button: face buttons, bumpers, triggers, Share / Options / PS, L3 / R3 and the D-pad. Any button can also be disabled.
- **Stick deadzone** per stick (round, rescaled, so movement starts smoothly), invert Y, swap sticks, digital triggers.
- **Rumble strength** from 0% (off) to 100%.
- **Virtual controller type**: Xbox 360, Xbox One S or DualShock 4 (PlayStation button icons in games that support them).
- **Profiles per game**: a profile switches on automatically while a listed program runs (e.g. `eldenring.exe`) and switches back when it exits.
- **Gyro aiming** (controllers with working motion sensors): drives the right stick or the mouse pointer, always or while a button is held, with calibration.
- **Desktop mode** (PS + Options): the controller works as a mouse and keyboard.
- **Controller test** window: live sticks, deadzone, triggers, buttons and gyro, handy for finding the right deadzone for a drifting stick.
- **/dev/uinput permission helper**: if the bridge has no access, it offers to install the udev rule for you (asks for the admin password).
- Log file limited to 1 MiB (older lines move to `ds4_bridge.log.1`) and a live log viewer.
- Interface in **English** and **Azərbaycanca** (🌐 Language menu).
- **🚀 Start on Login**: one-click autostart toggle for XDG-compliant desktops (GNOME, KDE, XFCE, etc.).

## 🐧 Which Linux Distros are Supported?

The pre-built tray app (executable and AppImage) needs **glibc 2.34 or newer** and Linux kernel 4.5+ (`uinput` v5):

| Distro | Works | Doesn't work |
|---|---|---|
| **Ubuntu** | 22.04, 24.04 and newer | 20.04 and older |
| **Debian** | 12, 13 | 11 and older |
| **Linux Mint** | 21, 22 | 20 and older |
| **Pop!_OS / Zorin / elementary OS** | releases based on Ubuntu 22.04+ | older releases |
| **Fedora** | 35 and newer | 34 and older |
| **RHEL / AlmaLinux / Rocky** | 9 | 8 |
| **Arch / Manjaro / EndeavourOS / openSUSE Tumbleweed** | ✅ (rolling) | |

On older distros, [compile it yourself](#%EF%B8%8F-manual-compilation): built from source it works on any distro with kernel 4.5+.

## 🛠 Dependencies

**To run** the tray app you need GTK 3 (installed on practically every desktop) and, for the tray icon, an AppIndicator library. The app finds **either** `libayatana-appindicator3` **or** the older `libappindicator3` on its own, so the same binary works on every distro. Without either it falls back to a classic tray icon (KDE, XFCE, MATE, Cinnamon). On GNOME, the "AppIndicator and KStatusNotifierItem Support" extension is needed to show tray icons.

**To compile** you only need a C compiler and the GTK 3 headers:

### Ubuntu / Debian / Linux Mint / Pop!_OS
```bash
sudo apt update
sudo apt install build-essential libgtk-3-dev
```

### Fedora
```bash
sudo dnf install gcc gtk3-devel
```

### Arch Linux / Manjaro
```bash
sudo pacman -S base-devel gtk3
```

## ⚙️ Manual Compilation

Navigate to the project directory in your terminal.

**Compile the background/CLI engine (`ds4_bridge`):**
```bash
gcc -O2 ds4_bridge.c -o ds4_bridge
```

**Compile the GTK Tray Application (`ds4_bridge_tray`):**
```bash
gcc -O2 -std=gnu11 ds4_bridge_tray.c -o ds4_bridge_tray $(pkg-config --cflags --libs gtk+-3.0) -lpthread -lm -ldl
```
*(A binary only runs on systems with the same or a newer glibc than the one it was built on. Build release binaries on an older distro, e.g. Ubuntu 22.04, so they work on as many systems as possible.)*

## 🔑 Permissions (Run Without Root)

The bridge needs write access to `/dev/uinput` and to the DS4 event device. To use it without `sudo`, add a udev rule:

```bash
echo 'KERNEL=="uinput", SUBSYSTEM=="misc", TAG+="uaccess", OPTIONS+="static_node=uinput"' \
  | sudo tee /etc/udev/rules.d/60-ds4aze-uinput.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```
*Log out and back in (or reboot) afterwards.*

The tray app can do this for you: when it cannot open `/dev/uinput` it offers to install the rule (🔧 **Fix /dev/uinput Permissions…** in the menu or in Settings → General).

## 🚀 Usage

You can run the program in two different ways:

### Method 1: Graphical Interface (Tray App - Recommended)
The most convenient method is to use the standalone Tray application:
```bash
./ds4_bridge_tray
```
* This will create an icon in your top panel (or system tray).
* From there, you can Start/Stop the bridge, see each controller's battery, pick a profile, deadzone and rumble strength, and switch desktop mode on or off.
* **⚙️ Settings…** opens the full settings window (button remapping, profiles, gyro, language), and **🧪 Controller Test…** shows live stick and button input.
* **Autostart:** Check **🚀 Start on Login** in the tray menu to automatically launch the bridge when you log in.

If you want to start the tray app and immediately enable the bridge from a script or terminal:
```bash
./ds4_bridge_tray --start
```

Settings are stored in `~/.config/ds4-bridge/settings.ini` and the log in `~/.local/state/ds4-bridge/ds4_bridge.log`. A `settings.json` from older versions (next to the executable) is imported automatically on first start.

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

* [AppImage (DS4_Bridge-1.0-x86_64.AppImage) - 1.0](https://www.virustotal.com/gui/file/5a65e68e8263cfc67e5cf7b1d257be4e2c48885853c8227e014b0a78baf11017)
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

# DS4 to Xbox 360 Bridge (For Knockoff DS4 Controllers)

<div align="center">
  <a href="https://github.com/engineer948/ds4-bridge-linux/releases/download/Alpha/ds4_bridge">
    <img src="https://img.shields.io/badge/Download-Compiled_Executable-green?style=for-the-badge&logo=linux" alt="Download Executable" />
  </a>
  <a href="https://github.com/engineer948/ds4-bridge-linux/releases/download/Alpha/ds4_tray.py">
    <img src="https://img.shields.io/badge/Download-Python_Tray_App-blue?style=for-the-badge&logo=python" alt="Download Python Tray App" />
  </a>
  <br>
  <em>(Click both buttons to download the required files, or see the one-click terminal command below)</em>
</div>

---

This project is a tool specifically designed to bridge **"knockoff" (fake) Sony DualShock 4 (DS4) controllers that don't work with Bluetooth or aren't recognized properly in Linux** into virtual **Microsoft Xbox 360** controllers. It is written using only the standard C library and POSIX/Linux headers.

## ⬇️ Download (One-Click)

To download both the compiled executable and the Python tray app instantly into your current folder, run this single command in your terminal:

```bash
wget https://github.com/engineer948/ds4-bridge-linux/releases/download/Alpha/ds4_bridge https://github.com/engineer948/ds4-bridge-linux/releases/download/Alpha/ds4_tray.py && chmod +x ds4_bridge ds4_tray.py
```
You can also download the [C Source Code (`ds4_bridge.c`)](https://github.com/engineer948/ds4-bridge-linux/raw/main/ds4_bridge.c) if you want to compile it yourself.

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

## 🛠 Dependencies

If you want to compile the program yourself and use the system tray application, you need to install the required dependencies for your distribution:

### Ubuntu / Debian / Linux Mint / Pop!_OS
```bash
sudo apt update
sudo apt install gcc python3 python3-gi gir1.2-gtk-3.0 gir1.2-ayatanaappindicator3-0.1
```
*(Note: If `ayatanaappindicator3` is not found, you can use `gir1.2-appindicator3-0.1` instead).*

### Fedora
```bash
sudo dnf install gcc python3 python3-gobject gtk3 libappindicator-gtk3
```

### Arch Linux / Manjaro
```bash
sudo pacman -S gcc python python-gobject gtk3 libappindicator-gtk3
```

## ⚙️ Manual Compilation

If you want to manually compile the C code (`ds4_bridge.c`), navigate to the project directory in your terminal and run the following command:

```bash
gcc -O2 ds4_bridge.c -o ds4_bridge
```

This command will create an executable file named `ds4_bridge`.

## 🚀 Usage

You can run the program in two different ways:

### Method 1: Graphical Interface (Tray App - Recommended)
The most convenient method is to use the Tray application written in Python:
```bash
python3 ds4_tray.py
```
* This will create an icon in your top panel (or system tray).
* From there, you can Start Bridge, Stop Bridge, and View Logs.

### Method 2: From Terminal (CLI)
To run it directly from the terminal (note: sometimes `root` (sudo) permissions may be required or `udev` rules must be added so `/dev/uinput` can be accessed):

```bash
# Run in the foreground:
./ds4_bridge

# Run in the background (as a daemon):
./ds4_bridge -d

# View detailed (verbose) logs:
./ds4_bridge -v
```

*(Note: If you receive a `/dev/uinput` error, try running the program as `sudo ./ds4_bridge`).*

## 🛡️ VirusTotal Results

For safety, you can check the VirusTotal results of the project files:

* [Compiled Executable](https://www.virustotal.com/gui/file/237d81af0d7c3e0858a8cb9c5f2c0455e6bc16159b0a31d48bebfbf81cb725dd?nocache=1)
* [C Code (ds4_bridge.c)](https://www.virustotal.com/gui/file/ecf9979dabc56c0f9eb7d2b97412f589d76325d87de3d9d50483bd5b7f97bca0?nocache=1)
* [Python Code (ds4_tray.py)](https://www.virustotal.com/gui/file/e345324266f85997606692604ee794187085247e528a71a54e9c9a1a28215e3a?nocache=1)

## 📝 Credits

* **DS4 Bridge Core:** The C code of this tool is written using standard POSIX/Linux libraries to map DualShock 4 inputs to an Xbox 360 controller.
* **Tray Application:** Built using Python 3 and GTK3 / AppIndicator for ease of use.

## 🔍 Keywords / Search Tags
`fake ds4 linux`, `knockoff ps4 controller linux`, `ds4 not recognized linux`, `ds4 over bluetooth linux problem`, `dualshock 4 clone linux driver`, `fake ds4 to xbox 360`, `ds4 uinput bridge`, `linux fake ds4 fix`, `ps4 controller clone linux setup`, `fake dualshock 4 bluetooth connection issue linux`, `linux ds4 vibration fix`, `fake ps4 controller rumble not working linux`, `knockoff dualshock 4 force feedback linux`, `ds4 bt connection problem ubuntu`, `third-party ps4 controller linux`, `ds4 spoof xbox 360 linux`, `ds4 evdev to uinput`, `unbranded ds4 controller ubuntu`, `generic ps4 controller linux driver`, `ds4 vibration rumble support linux`, `fake ds4 bluetooth pairing failed`, `cheap ps4 controller linux fix`, `ds4 input mapper linux`, `xpadneo alternative for fake ds4`, `linux ds4 emulation`, `fake ds4 windows vs linux`, `ps4 controller wire connection error linux`, `linux mint ds4 controller not detected`, `arch linux fake dualshock 4`, `ds4 force feedback bridge`, `bluetooth ps4 controller clone linux`, `ds4 bt mac address zero linux fix`

## ⚖️ Legal Disclaimer
This project is an independent, open-source software tool designed for hardware interoperability. It is **not** affiliated with, endorsed, sponsored, or approved by Sony Interactive Entertainment Inc. 

"PlayStation", "DualShock", and "Sony" are registered trademarks of Sony Interactive Entertainment Inc. All other trademarks are the property of their respective owners. The terms "fake" or "knockoff" are used purely for descriptive purposes to refer to third-party, unlicensed, or unbranded controllers that attempt to replicate the functionality of official hardware. This software does not bypass any DRM or piracy protection; it simply reads standard input events and translates them to an Xbox 360 virtual device.

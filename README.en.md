<div align="center">

# 🪐 NameLess Orbit

**Your phone and computer together: files, clipboard, notifications, screen sharing, second screen, sound**

`Qt 6` · `Linux` · `Android` · `Windows` · `macOS`

[🇷🇺 Русский](README.md) · 🇬🇧 English

[Features](#-features) · [Install](#-installation) · [Update](#-updating) · [Guide](docs/USAGE.md) · [Build](#-building-from-source) · [Contribute](CONTRIBUTING.md)

</div>

---

## ✨ Features

| | |
|---|---|
| 🔐 **Pairing** | a shared check code on both screens, encrypted channel (X25519 · ChaCha20 · HMAC-SHA256), automatic reconnection |
| 📁 **Files and clipboard** | sending with the receiver's consent, clipboard text in one tap |
| 🔔 **Notifications and “find my device”** | messages between devices, a ring until “Found it” is pressed |
| 🖥 **Screen sharing** | H.264 on the GPU or CPU, tiles, JPEG; quality adapts to the network; an info bar shows who sees what |
| 🪟 **Second screen** | a device becomes one more monitor of a Hyprland computer: its own workspace, scale, monitor mirroring, protection when the link drops |
| 🔊 **Sound** | computer sound to another device — uncompressed or Opus; a separate permission per device |
| 🖱 **Control** | mouse and keyboard from the phone; a red bar while control is on |
| 🛡 **Trust** | “Ask” · “Trusted” · “Full trust” — you decide what you are asked about |
| 🌐 **Languages** | Russian and English, chosen on first launch |

Everything works **only on your network** and only between **your** devices: each one is confirmed with a code on both screens.

## 📦 Installation

**Arch Linux** (from the AUR or from this repository):

```bash
yay -S nameless-orbit-git                         # once the package is published in the AUR
# or manually from the repository sources:
cd packaging/arch && makepkg -si
```

**Other distributions** — build from source ([below](#-building-from-source)); `cmake --install` adds the menu entry.

**Android** — a ready APK is in [Releases](https://github.com/thecylindr/nameless-orbit/releases). Allow installs from that source and open the app; on first launch it offers to pick a language and asks for the notification permission. The app is called **Orbit** on Android.

**With NameLess UI** — the shell sees a running Orbit by itself: a bar icon, the device ribbon, a tile and a widget ([nameless-ui](https://github.com/thecylindr/nameless-ui)).

## 🔄 Updating

- **Arch**: `yay -Syu` (the `nameless-orbit-git` package updates with the system).
- **From source**: `git pull && cmake --build build && cmake --install build`.
- **Android**: download the new APK from Releases and install over the old one. Data and paired devices are kept; if the app had to be **uninstalled**, the phone gets a new identity — when pairing, Orbit moves trust and permissions from the old entry by itself.

> Update the app on both devices: versions that differ a lot may not understand new requests (for example, the second screen).

## 🚀 Quick start

1. Open Orbit on both devices on the same network — they find each other by themselves.
2. Compare the code and press “Codes match — pair” on each.
3. Open the device page. To watch a screen or make a second one, enable the **“Screen sharing”** permission there.

Details are in the [guide](docs/USAGE.md) (in Russian).

## 🛠 Building from source

Requires: Qt 6 (base, declarative, multimedia, svg, wayland), CMake ≥ 3.16, Ninja. For video on PC — ffmpeg (libavcodec); for screen sharing — wf-recorder; for sound — libpulse (`parec`).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build        # or: -DCMAKE_INSTALL_PREFIX=$HOME/.local
```

Android — through Qt for Android (Qt Creator, the Android arm64-v8a kit); the platform code is in `android/`.

## 🗂 Project layout

```
core/        network, encryption, pairing, trust, settings, translation dictionary
plugins/     files, clipboard, notifications, battery, player, commands, “find”
remote/      screen sharing, second screen, sound, input
platform/    system calls: Linux, Android (JNI), Windows, macOS
ui/          the QML interface
android/     manifest, service, codecs
i18n/        English translation (en.json)
packaging/   .desktop and the Arch PKGBUILD
docs/        the guide
```

## 🧯 Troubleshooting

See the “symptom → what to do” table at the end of the [guide](docs/USAGE.md#решение-проблем). The screen-sharing log (in plain language): `~/.cache/glassrice/NameLess Orbit/orbit-video.log`.

## 📄 License

**NameLess Source-Available License 1.0** — [LICENSE](LICENSE). In short: you may read, build and use the code **for yourself, non-commercially**; a fork needs the written permission of the repository owner (request it in an issue beforehand). You may not copy, fork without the owner's permission and publish the project (whole or in substantial parts), release derivative versions, use it commercially, pass it off as your own, or build datasets from the code to train models. Need more — contact the repository owner via an issue. Third-party components (Qt, FFmpeg, Quickshell…) stay under their own licences.

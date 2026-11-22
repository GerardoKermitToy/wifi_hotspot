**English** | [Русский](README.ru.md)

# WiFi Studio

A native C++20 app for Windows 11 Mobile Hotspot. Choose the Wi-Fi adapter and Internet source, set the network name, password, band, and WPA2/WPA3 security, then start or stop the hotspot.

- Session-only settings preserve the saved Windows hotspot configuration.
- Passwords are not logged or saved. Existing hotspots are not stopped or reconfigured.
- The interface follows the Windows display language: Russian for Russian, English otherwise. Optional override: `--lang en` or `--lang ru`.

## Requirements

Windows 11 24H2+ (build 26100+), a Mobile Hotspot / Wi-Fi Direct compatible adapter, and an active source connection. Available bands and security modes depend on the driver. Windows may require location / Wi-Fi permissions.

To build: Visual Studio 2026 with Desktop development with C++, Windows SDK 26100+, and CMake 4.2+.

## Build and run

```powershell
cmake --preset vs2026-x64
```

```powershell
cmake --build --preset release --parallel
```

```powershell
.\out\build\vs2026-x64\Release\WiFiStudio.exe
```

## Documentation

- [Testing and troubleshooting](docs/TESTING.md) (Russian)
- [WPA3 support](docs/WPA3.md) (Russian)

Legacy Hosted Network and standalone SoftAP without a source connection are not supported.

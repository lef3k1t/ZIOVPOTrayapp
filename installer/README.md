# ZIOVPO Tray App Installer

Run from an elevated PowerShell session:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\install.ps1
```

Uninstall:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\uninstall.ps1
```

The application is built with the static MSVC runtime, so the installer does not install a shared Visual C++ Redistributable package.

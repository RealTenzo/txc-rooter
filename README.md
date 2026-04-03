# TXC Rooter

Roots BlueStacks 5 and MSI App Player on Windows. No scripts, no third-party tools, everything's packed into one exe. GUI is ImGui over DirectX 11.

---

## Requirements

- Windows 10 or 11
- **DirectX End-User Runtime** — needed for the DX11 renderer. If the exe crashes immediately on launch, this is probably why. Grab it from [here](https://www.microsoft.com/en-us/download/details.aspx?id=35) and run it once
- Run as Administrator — the VHD and emulator files need elevated access

---

## How it works

BlueStacks locks `Root.vhd` as read-only and has `HD-Player.exe` verify disk integrity before it launches. If the VHD was touched it won't start. So there are basically two problems to solve before you can even inject anything: make the VHD writable, and stop the launcher from caring.

Here's what each button does:

**Kill Emulator Processes** — kills `HD-Player.exe`, `HD-MultiInstanceManager.exe`, and `BstkSVC.exe`. File handles need to be closed before you can do anything with the VHD.

**Fix Illegally Tampered** — patches `HD-Player.exe`. The integrity check is a `TEST AL,AL` + `JZ` sequence after a call — the tool finds it by searching for anchor strings like `"Verified the disk integrity!"` near the patch site and replaces the `JZ` with two `NOP` bytes. Backs up the original to `HD-Player.exe.bak` first, only does it once.

**Disk R/W / Disk R/O** — edits the `.bstk` config file for the selected instance, toggling the VHD entries between `type="Readonly"` and `type="Normal"`. Use R/W before editing anything, R/O to put it back.

**One Click Root** — mounts `Root.vhd` directly using an embedded ext4 library (no WSL, no kernel mounts), drops `su` into `/system/xbin/su` with suid permissions (`06755`, owner root), then edits `bluestacks.conf` to enable root for the instance. The `su` binary is stored inside the exe XOR-encoded and decrypted at runtime.

**One Click Unroot** — mounts the VHD and removes `/system/xbin/su`. That's it.

**Install SuperSU** — doesn't touch the VHD at all. Extracts `SuperSU_v2.82.apk` from the exe, then runs `adb install -r` against the running emulator. Looks for `HD-Adb.exe` in the BlueStacks install directory first, falls back to whatever `adb.exe` is in your `PATH`. Emulator has to be running for this one. After install, open the SuperSU app inside the emulator to finish setup.

**Uninstall SuperSU** — mounts the VHD and checks all the usual places: `/data/app/SuperSU.apk`, `/system/app/SuperSU/`, `/system/priv-app/SuperSU/`, `/system/xbin/su`. Removes whatever it finds.

---

## Instance handling

Both BlueStacks 5 (`BlueStacks_nxt`) and MSI App Player (`BlueStacks_msi5`) are detected from the registry automatically. If you have multiple instances, pick one from the dropdown — clones share the master instance's `Root.vhd`, so root/unroot always targets the master regardless of which clone you selected. The badge next to the dropdown tells you if the selected instance is a master or a clone.

---

## The VHD mounting

Uses [lwext4](https://github.com/gkostka/lwext4) compiled into the exe. It opens the VHD as a raw file, scans the MBR partition table for the ext4 partition, then talks to it through a custom block device interface. No external tools, no admin-required mountpoints — it's just file I/O.

---

## Building

Visual Studio 2022, C++17, x64. Open `TxcRooter.sln` and build. Everything's in-tree — ImGui, lwext4, backends. Output goes to `build/Release/TxcRooter.exe`.

Resources baked into the exe:
- `res/su_c.enc` — the `su` binary, XOR'd with `0xA7`
- `res/SuperSU_v2.82.apk` — SuperSU installer

---

## Caveats

The `HD-Player.exe` patch works by searching for known strings near the check site. If BlueStacks ships a version that restructures that code significantly, the search might not find anything and the patch will bail out with an error. It won't corrupt anything if it fails — it just won't patch.

Everything here modifies files while the emulator is stopped. If you run the emulator and it updates itself or overwrites system files, your changes can get wiped. Root the instance after updates, not before.

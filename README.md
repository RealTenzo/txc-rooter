# TXC Rooter

Roots BlueStacks 5 and MSI App Player on Windows. Single exe, no dependencies.

---

## Requirements

- Windows 10 or 11
- **DirectX End-User Runtime** needed for DX11 renderer. If exe crashes on launch, install it from here: https://www.microsoft.com/en-us/download/details.aspx?id=35
- Run as Administrator

---

## How it works

BlueStacks locks Root.vhd as read-only. HD-Player.exe checks disk integrity before launching. If the VHD was modified, it won't start. Two problems to solve: make the VHD writable, and disable the integrity check.

**Kill Emulator Processes** kills HD-Player.exe, HD-MultiInstanceManager.exe, and BstkSVC.exe. Closes file handles so you can modify the VHD.

**Fix Illegally Tampered** patches HD-Player.exe. Finds the integrity check by searching for strings like "Verified the disk integrity!" near the patch location, replaces the conditional jump with two NOP bytes. Creates HD-Player.exe.bak first.

**Disk R/W / Disk R/O** edits the .bstk config file for the selected instance, toggles VHD entries between type="Readonly" and type="Normal". Use R/W before editing, R/O to revert.

**One Click Root** mounts Root.vhd using an embedded ext4 library, drops su into /system/xbin/su with suid permissions (06755, owner root), edits bluestacks.conf to enable root. The su binary is XOR-encoded inside the exe.

**One Click Unroot** mounts the VHD and removes /system/xbin/su.

**Install SuperSU** extracts SuperSU_v2.82.apk from the exe, then runs adb install -r against the running emulator. Looks for HD-Adb.exe in BlueStacks install directory first, falls back to adb.exe in PATH. Emulator must be running. After install, open SuperSU app inside emulator to finish setup.

**Uninstall SuperSU** mounts the VHD and checks all usual places: /data/app/SuperSU.apk, /system/app/SuperSU/, /system/priv-app/SuperSU/, /system/xbin/su. Removes whatever it finds.

---

## Instance handling

Both BlueStacks 5 (BlueStacks_nxt) and MSI App Player (BlueStacks_msi5) are detected from registry automatically. If you have multiple instances, pick one from the dropdown. Clones share the master instance's Root.vhd, so root/unroot always targets the master regardless of which clone you selected. The badge next to the dropdown shows if selected instance is master or clone.

---

## VHD mounting

Uses lwext4 compiled into the exe. Opens VHD as raw file, scans MBR partition table for ext4 partition, then accesses it through custom block device interface. No external tools, no mountpoints. Just file I/O.

---

## Building

Visual Studio 2022, C++17, x64. Open TxcRooter.sln and build. Everything is in-tree: ImGui, lwext4, backends. Output goes to build/Release/TxcRooter.exe.

Resources baked into exe:
- res/su_c.enc is the su binary XORed with 0xA7
- res/SuperSU_v2.82.apk is the SuperSU installer

---

## Caveats

HD-Player.exe patch works by searching for known strings near the check site. If BlueStacks ships a version that restructures that code significantly, the search may fail and patch will bail out with an error. It won't corrupt anything if it fails, just won't patch.

Everything here modifies files while emulator is stopped. If emulator runs and updates itself or overwrites system files, changes can be wiped. Root after updates, not before.

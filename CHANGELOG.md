# Changelog

## 0.5.3

- **Fixed:** the game froze on the way back to the main menu (Quit to menu, or save and quit) with frame generation on.
  Frame generation now runs in the game scene only and the mod starts again in the menu.
- **Fixed:** DLSS on an NVIDIA card whose vendor ID a graphics wrapper reports wrongly: the NVIDIA driver in the
  process counts as well. The default upscaler on the first start follows the card's name too.
- **New:** a graphics wrapper (`dxgi.dll`, `d3d11.dll` or `d3d12.dll` not from the system folder) is named in the console:
  it can hide the video card from DLSS, Reflex and FSR.
- **Improved:** a failed shared texture names the step in `msf.log`; a NullReferenceException of the game's scripts is no
  longer logged as a crash (a real crash after it is still reported).

## 0.5.2

- **Fixed:** DLSS and Reflex detection on systems where GPU vendor ID is reported incorrectly. Now checks GPU name (NVIDIA/GeForce/RTX) in addition to vendor ID.

## 0.5.1

- **Improved:** the capabilities list in the settings page and the console is now structured (one feature per line: DLSS, DLSS FG, FSR, FSR FG, Reflex).
- **Improved:** "My Summer Frames" in the console startup message now uses yellow + green colors.

## 0.5.0

- Renamed from MSCDLSS to **My Summer Frames** (`MySummerFrames.dll`, console command `msf`). Settings of the MSCDLSS test
  builds are not carried over.
- New: sharpness slider (0 to 100 %): FSR's own RCAS, and an RCAS pass after DLSS.
- New: settings page in the style of MOP - REVIVAL 5.1, in English and Russian (the system language on the first start),
  switching the language relabels the open page.
- New: "Detailed log in the console" option. Without it the console shows only the status lines, changes, notes and problems.
- New: mod icon in the MSCLoader mod list.

## 0.4.0 (MSCDLSS, test build)

- AMD FSR upscaling and FSR frame generation (FSR SDK 2.4) through a Direct3D 11 ↔ 12 round trip.
- The crosshair and the HUD icons stay visible in the upscaling modes.

## 0.3.0 and earlier (MSCDLSS, test builds)

- DLAA and DLSS Super Resolution on the game's Direct3D 11 device, motion vectors from depth.
- DLSS frame generation (2x to 6x) on a Direct3D 12 bridge, NVIDIA Reflex.

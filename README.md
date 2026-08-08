# GameTank Emulator (CRT-friendly, Raspberry Pi / ARM64)

Fork of the [upstream GameTank Emulator](https://github.com/clydeshaffer/GameTankEmulator) with a **CRT-friendly**, controller-first WRAPPER UI for **Raspberry Pi / ARM64** (RGB-Pi and similar).

Built for low-res CRT output: compact overlay, pixel font, fixed game image scale, and gamepad navigation. Press **Select** to open the menu (ROM list, options, remapping).

## Releases (Raspberry Pi / ARM64)

Download a prebuilt package from [Releases](https://github.com/gustavostuff/GameTankEmulator/releases).

Unpack it so you have:

```text
GameTank/
  GameTankEmulator
  roms/
```

1. Put `.gtr` ROMs in `GameTank/roms/`
2. Place `GameTank/` where you want it (e.g. under your RGB-Pi ports folder) and run `GameTank/GameTankEmulator`, from wherever you need to (like a shell script).

## Build on the Pi

SSH into the Pi and paste:

```sh
curl -fsSL https://raw.githubusercontent.com/gustavostuff/GameTankEmulator/main/scripts/linux/install-rgbpi.sh | bash
```

This clones the repo, builds the ARM64 WRAPPER binary, and writes a `GameTank/` package (under `/media/usb1/roms/ports/` when that path exists, otherwise `~/GameTank`).

To rebuild later:

```sh
bash ~/GameTankEmulator/scripts/linux/install-rgbpi.sh
```

## Controls

| Input | Action |
|-------|--------|
| Select | Open / close menu |
| D-pad / stick | Navigate |
| A | Confirm / Set |
| B / X | Adjust / Clr (context-dependent) |
| L / R shoulders | Cycle tabs (in menu) or palettes (in game) |

ROMs load from `roms/` next to the binary. Settings and mappings save under the SDL pref path (`.../GameTank/Emulator/`).

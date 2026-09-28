# Changelog

## [Unreleased]

### Added

- `LightFollowsHead` and `LightMultiplier` under `[Light]`. The flashlight turns with your head at 1.5 times the head turn, as it did before; `LightMultiplier` changes how far, from 0 to 5, and `LightFollowsHead=false` leaves the beam on your aim.
- A setting set to `default` in `CameraUnlock.ini` takes its value from `Defaults.ini`, which every head tracking mod that keeps its settings in `CameraUnlock.ini` reads. Head tracking mods that keep their settings in another file do not read it, and neither do earlier versions of this mod. Writing a value in place of `default` changes that setting for this game only. When the mod saves a setting that a hotkey changed in game, it writes the new value in place of `default`, so that setting no longer follows `Defaults.ini` in this game until you set it to `default` again.
- `Defaults.ini` is `%AppData%\CameraUnlock\Defaults.ini` on Windows; `$XDG_CONFIG_HOME/CameraUnlock/Defaults.ini` on Linux, or `~/.config/CameraUnlock/Defaults.ini` where `XDG_CONFIG_HOME` is not set, under Wine and Proton too; and `~/Library/Application Support/CameraUnlock/Defaults.ini` on macOS. The mod's log, where it writes one, names the file it read.
- When the mod starts and finds no `Defaults.ini`, it creates one holding the built-in values, unless Windows runs the game as a packaged app. The mod never changes `Defaults.ini` after that.

### Changed

- Settings move to `CameraUnlock.ini`. Earlier versions of the mod kept these settings in `HeadTracking.ini`, in the same folder. The first time this version starts and finds no `CameraUnlock.ini`, it reads your settings from `HeadTracking.ini` and writes them into `CameraUnlock.ini`. It never changes `HeadTracking.ini`, and does not read it again while `CameraUnlock.ini` exists.
- A setting that the defaults the README shows set to `default` is written as `default` when you never changed it from the default of the earlier version that wrote `HeadTracking.ini`, as far as the file shows which version that was, because `HeadTracking.ini` does not hold it or holds that default. It then follows `Defaults.ini`, so it takes the value `Defaults.ini` gives it, or the built-in value where `Defaults.ini` gives none, which can differ from the default earlier versions used. A setting you changed is written with the value imported for it, or as `default` where that value equals its default at that start.
- `RotationEnabled` and `PositionEnabled` are one setting here, the tracking mode, so both are written as `default` or neither is.
- Comments, and keys the mod never read, are not carried over. Nor are these, where your old file had them:
  - A sensitivity, scale, deadzone, response curve or axis inversion you changed from its default. Set these in your tracker instead.
  - Reticle settings, and a key that toggled the reticle.
  - The setting for a feature that earlier versions shipped switched off while it was untested. It now follows the mod's default.
  - A hotkey set to Ctrl, Shift or Alt on its own. That key goes down before the key of any chord made with it, so the hotkey is left unbound, and it keeps its Ctrl+Shift chord.
- An older version of the mod reads `HeadTracking.ini` and never reads `CameraUnlock.ini`, so a setting you change after updating is not in `HeadTracking.ini`.
- Deleting only `CameraUnlock.ini` makes the next start read `HeadTracking.ini` again. To go back to the defaults, replace everything in `CameraUnlock.ini` with the defaults the README shows. Every setting they set to `default` then follows `Defaults.ini`.
- Hotkeys are written as key names, and each hotkey lists every key that triggers it, the Ctrl+Shift chord included: `ToggleKey=End, Ctrl+Shift+Y`. The chords were fixed in code before; now they can be changed or removed like any other key.
- The tracking mode (`PageUp`) and the yaw mode (`PageDown`) are saved to `CameraUnlock.ini` when they change, so the next launch starts in the mode you left. `End` still changes the session only.
- `[Network] Port` becomes `UdpPort`. The old `[Position] Enabled` becomes the tracking mode at startup, `RotationEnabled` and `PositionEnabled`. The old single `LimitY` becomes both `PositionLimitY` and `PositionLimitYDown`, which it already set, and each can now be set on its own; `LimitX`, `LimitZ` and `LimitZBack` become `PositionLimitX`, `PositionLimitZ` and `PositionLimitZBack`. `EnableOnStartup` and `WorldSpaceYaw` move to `[General]`. `[View] Fov` and `[Debug] LogToFile` keep their names.
- A number in `HeadTracking.ini` outside the range a setting takes is brought to the nearest end of that range, and the log says so: a `LimitX`, `LimitY`, `LimitZ` or `LimitZBack` above 10 metres is written as 10.
- The `HeadTracking.ini` reader is unchanged since v0.1.0, and so is how the mod starts from what it read, so apart from the changes listed here every setting you had carries over as it was.
- `uninstall.cmd` keeps `CameraUnlock.ini` and `HeadTracking.ini`. It used to delete `HeadTracking.ini`.

### Removed

- The sensitivity, scale, deadzone, response curve and axis inversion settings. Set these in your tracker app instead.
- With these settings at their shipped defaults the camera moves as it did before: `[Position] WorldScale` shipped at 39.37 Source units per metre, which the mod now applies itself.

## [0.1.1] - 2026-09-18

### Fixed

- ship a v2 manifest so the launcher deploys natively

## [0.1.0] - 2026-09-18

First release.

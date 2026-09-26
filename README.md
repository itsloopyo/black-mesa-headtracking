# Black Mesa Head Tracking

![Black Mesa running with this mod](https://raw.githubusercontent.com/itsloopyo/black-mesa-headtracking/main/assets/readme-clip.gif)

An unofficial head tracking mod for Black Mesa that moves the view with your head while your mouse or controller keeps aiming, driven by a webcam, phone, or any OpenTrack compatible tracker, with no VR headset required.

## Features

- **Decoupled look and aim** - head tracking moves the camera; aim stays on your mouse or controller
- **6DOF positional tracking** - lean, peek and duck with head position
- **Flashlight follows your head** - the flashlight turns with your head rather than your aim, by 1.5 times the head turn. `LightMultiplier` sets how far; `LightFollowsHead=false` leaves the beam on your aim.
- **Works with any OpenTrack compatible tracker** - free options available for PC, iOS and Android

## Requirements

- [Black Mesa](https://store.steampowered.com/app/362890/Black_Mesa/) on Steam (app 362890), on the build whose `bms\bin\client.dll` is dated 7 June 2025. On any other build the mod stays dormant, writes a log line saying which direction the build differs in, and the game runs vanilla. Steam is the only store the installer knows how to find; if your copy came from somewhere else, pass its folder to `install.cmd` as shown below and the mod installs the same way. Own it twice and each copy needs its own run, since the installer writes to one folder at a time.
- A tracking source that sends the OpenTrack UDP protocol, such as [OpenTrack](https://github.com/opentrack/opentrack/releases) with a webcam.
- 64-bit Windows 10 or 11. `bms.exe` is a 32-bit process, so the mod and its loader are both x86.

## Installation

### Lopari

Download [Lopari](https://lopari.app), choose **Black Mesa**, and click
**Play with head tracking**.

### Standalone Installer

1. Download the installer ZIP (`BlackMesaHeadTracking-v<version>-installer.zip`) from the [Releases page](https://github.com/itsloopyo/black-mesa-headtracking/releases).
2. Extract it anywhere.
3. Double-click `install.cmd`. It finds Black Mesa, places the loader and the mod beside `bms.exe`, and reports what it did.
4. Configure OpenTrack (or your tracker app) to send UDP to `127.0.0.1:4242`.
5. Launch the game.

If the installer cannot find your copy of the game, point it at the folder
yourself. Either pass the path as an argument:

```powershell
install.cmd "D:\Games\steamapps\common\Black Mesa"
```

or set the override environment variable before running it:

```powershell
$env:BLACK_MESA_PATH = "D:\Games\steamapps\common\Black Mesa"
.\install.cmd
```

Both expect the folder that contains `bms.exe`.

### Manual Installation

There is one release archive, the installer ZIP, and no mod-manager archive.
This mod's payload has to land at the game root, beside `bms.exe`, and a mod
manager deploys into one fixed subtree per game. Vortex ships no bundled Black
Mesa extension, so nothing establishes that it can put a file at the game root
at all - and an archive a manager deploys to the wrong place installs cleanly,
starts the game normally, writes no log, and looks exactly like a mod that does
not work. So install by hand instead:

1. Take these two files out of the installer ZIP: `plugins\BlackMesaHeadTracking.asi` and `vendor\ultimate-asi-loader\dinput8.dll`.
2. Rename `dinput8.dll` to `winmm.dll` and put it next to `bms.exe`, at the game root. Not in `bin\`: a proxy there loads if you start `bms.exe` yourself and never when you launch from Steam, because Steam injects its overlay first and that pulls `WINMM.dll` in from `C:\Windows\System32`. Windows resolves a DLL by base name, so once the system copy is loaded ours cannot be, nothing scans for the `.asi`, and the mod is silently absent. If another mod already put an Ultimate ASI Loader proxy at the game root, leave it alone and skip this step.
3. Put `BlackMesaHeadTracking.asi` beside it, at the game root.
4. Launch the game once. The mod writes `CameraUnlock.ini` and `HeadTracking.log` next to `bms.exe`, in that same folder.

## Setting Up OpenTrack

In OpenTrack, set **Output** to `UDP over network` and enter host `127.0.0.1`,
port `4242`. Map yaw, pitch and roll, and X, Y and Z as well if you want
positional tracking. Press **Start**, then launch the game.

Centering is done in your tracker. Use OpenTrack's Center bind, SteamVR's reset,
or the CENTER button in your phone app.

### VR Headset Setup

1. Connect the headset to the PC over Air Link, Virtual Desktop or a link cable.
2. Start SteamVR and let it finish setting the headset up.
3. In OpenTrack, set **Input** to the SteamVR tracker.
4. Leave **Output** on `UDP over network`, host `127.0.0.1`, port `4242`.

### Webcam Setup

Set OpenTrack's **Input** to `neuralnet tracker`. It tracks your face from a
plain webcam, with no markers, clip or IR hardware. Even lighting on your face
is what it needs most.

### Phone App Setup

The mod accepts one thing: OpenTrack UDP packets on port `4242`. A phone app is
usable here if it sends that protocol itself, or ships a PC-side companion that
does. Check your app against that first.

For an app that does send it, what decides the wiring is how much filtering it
does before the packet leaves the phone. An app that filters on-device can point
straight at this PC's LAN address (run `ipconfig` to find it) on UDP port
`4242`. A raw or lightly filtered feed sent direct will jitter, because the
mod's smoothing is sized to take the edge off a clean signal rather than to
rescue a noisy one. That app should send into OpenTrack instead, on a spare port
such as `5252`, with OpenTrack's output going to `127.0.0.1:4242` so its filters
and curves clean the feed up first.

The test is quicker than the reading: send direct, hold your head still, and if
the view drifts or shakes, route it through OpenTrack.

I made [Headcam](https://headcam.app) so decent tracking was free for anybody
with a phone already in their pocket. It filters on-device, so it can send
direct. Any other app that filters enough works exactly the same way.

A phone on WiFi is a remote connection and gets `RemoteSmoothing`. So does a
tracker running on this same PC that sends to the machine's LAN address instead
of `127.0.0.1`, because the classifier sees a transport and not a machine. Only
loopback counts as local.

## Controls

Each action has a list of keys, and any key in it fires the action. By default
each list holds a nav-cluster key and a chord, so use whichever your keyboard has:

| Action              | Default keys               | Setting                |
|---------------------|----------------------------|------------------------|
| Toggle tracking     | `End`, `Ctrl+Shift+Y`      | `ToggleKey`            |
| Cycle tracking mode | `PageUp`, `Ctrl+Shift+G`   | `CycleTrackingModeKey` |
| Toggle yaw mode     | `PageDown`, `Ctrl+Shift+H` | `YawModeKey`           |

`Page Up` / `Ctrl+Shift+G` cycles tracking mode:

1. 6DOF, rotation and position together
2. Rotation only, positional tracking off
3. Position only, rotational tracking off
4. Back to 6DOF

`Page Down` / `Ctrl+Shift+H` switches yaw between horizon-locked (yaw around the
world up axis, the default) and camera-local (yaw composed with the camera's
current pitch and roll).

The tracking mode and the yaw mode are saved to `CameraUnlock.ini` the moment
they change, so the next launch starts in the mode you left it in. `End` turns
tracking on and off for the session only and saves nothing: whether tracking is
on at launch is `EnableOnStartup`.

Every key in the three lists, the chords included, can be changed or removed
under `[Hotkeys]` in `CameraUnlock.ini`, for example `ToggleKey=F8, Ctrl+Shift+Y`.
Hotkeys only fire while the Black Mesa window has focus.

## Configuration

<!-- cameraunlock:config -->
The mod reads its settings from `CameraUnlock.ini` in the game folder, and creates the file when it starts and finds none. Edit it with any text editor.

A setting set to `default` takes its value from `Defaults.ini`, which every head tracking mod that keeps its settings in `CameraUnlock.ini` reads. Head tracking mods that keep their settings in another file do not read it, and neither do earlier versions of this mod. Writing a value in place of `default` changes that setting for this game only. When the mod saves a setting that a hotkey changed in game, it writes the new value in place of `default`, so that setting no longer follows `Defaults.ini` in this game until you set it to `default` again.

`Defaults.ini` is `%AppData%\CameraUnlock\Defaults.ini` on Windows; `$XDG_CONFIG_HOME/CameraUnlock/Defaults.ini` on Linux, or `~/.config/CameraUnlock/Defaults.ini` where `XDG_CONFIG_HOME` is not set, under Wine and Proton too; and `~/Library/Application Support/CameraUnlock/Defaults.ini` on macOS. The mod's log, where it writes one, names the file it read.

When the mod starts and finds no `Defaults.ini`, it creates one holding the built-in values, unless Windows runs the game as a packaged app. The mod never changes `Defaults.ini` after that. Edit it with any text editor.

Earlier versions of the mod kept these settings in `HeadTracking.ini`, in the same folder. The first time this version starts and finds no `CameraUnlock.ini`, it reads your settings from `HeadTracking.ini` and writes them into `CameraUnlock.ini`. It never changes `HeadTracking.ini`, and does not read it again while `CameraUnlock.ini` exists.

A setting that the defaults below set to `default` is written as `default` when the value imported for it equals its default at that start, which is the value `Defaults.ini` gives it, or the built-in value where `Defaults.ini` gives none. It then follows `Defaults.ini`. Every other setting is written with the value imported for it. `RotationEnabled` and `PositionEnabled` are one setting here, the tracking mode, so both are written as `default` or neither is.

Comments, and keys the mod never read, are not carried over. Nor are these, where your old file had them:

- Reticle settings, and a key that toggled the reticle.
- A sensitivity, scale, deadzone, response curve or axis inversion you changed from its default. Set these in your tracker instead.
- The setting for a feature that earlier versions shipped switched off while it was untested. It now follows the mod's default.

An older version of the mod reads `HeadTracking.ini` and never reads `CameraUnlock.ini`, so a setting you change after updating is not in `HeadTracking.ini`.

Deleting only `CameraUnlock.ini` makes the next start read `HeadTracking.ini` again. To go back to the defaults, replace everything in `CameraUnlock.ini` with the defaults below. Every setting they set to `default` then follows `Defaults.ini`.

The built-in value of each setting set to `default` below:

- `UdpPort=4242`
- `EnableOnStartup=true`
- `WorldSpaceYaw=true`
- `RotationEnabled=true`
- `LocalSmoothing=0.0`
- `RemoteSmoothing=0.15`
- `PositionEnabled=true`
- `PositionLimitX=0.3`
- `PositionLimitY=0.2`
- `PositionLimitYDown=0.2`
- `PositionLimitZ=0.4`
- `PositionLimitZBack=0.1`
- `ToggleKey=End, Ctrl+Shift+Y`
- `CycleTrackingModeKey=PageUp, Ctrl+Shift+G`
- `YawModeKey=PageDown, Ctrl+Shift+H`
- `LightFollowsHead=true`
- `LightMultiplier=1.5`

With every setting at its default, the file reads:

```ini
; Black Mesa head tracking settings.
; Comments start with ; and go on their own line. Text after a value is part of the value.
; Hotkeys are key names such as End, PageUp or Ctrl+Shift+Y. Separate several with commas; leave empty for none.
; A setting set to default takes its value from Defaults.ini, which every head tracking mod
; that keeps its settings in CameraUnlock.ini reads: %AppData%\CameraUnlock\Defaults.ini on
; Windows, $XDG_CONFIG_HOME/CameraUnlock/Defaults.ini (normally ~/.config/CameraUnlock) on
; Linux, under Wine and Proton too, and ~/Library/Application Support/CameraUnlock/Defaults.ini
; on macOS. The log names the file it read. Write a value instead of default to change that
; setting for this game only.

[CameraUnlock]
; Written by the mod. Leave this section in place.
ConfigFormat=1

[Network]
; UDP port the mod receives tracker data on (OpenTrack protocol).
UdpPort=default

[General]
; true: head tracking is on when the game starts. ToggleKey turns it on and off.
EnableOnStartup=default
; true: yaw turns around the world's up axis. false: around the camera's own up axis.
WorldSpaceYaw=default
; true: turning your head turns the view.
; Tracking mode at startup, with PositionEnabled. The mode hotkey changes both.
RotationEnabled=default

[Smoothing]
; Smoothing when the tracker runs on this PC. 0 is the least, 1 the most.
LocalSmoothing=default
; Smoothing when the tracker is another device on the network, such as a phone.
; 0 is the least, 1 the most.
RemoteSmoothing=default

[Position]
; true: moving your head moves the view.
; Tracking mode at startup, with RotationEnabled. The mode hotkey changes both.
PositionEnabled=default
; How far, in metres, leaning left or right can move the view.
PositionLimitX=default
; How far, in metres, raising your head can move the view.
PositionLimitY=default
; How far, in metres, lowering your head can move the view.
PositionLimitYDown=default
; How far, in metres, leaning forward can move the view.
PositionLimitZ=default
; How far, in metres, leaning back can move the view.
PositionLimitZBack=default

[Hotkeys]
; Turns head tracking on and off.
ToggleKey=default
; Changes the tracking mode: rotation and position, rotation only, position only.
CycleTrackingModeKey=default
; Switches yaw between the world's up axis and the camera's own (WorldSpaceYaw).
YawModeKey=default

[Light]
; true: a light you carry points where you look instead of where you aim.
LightFollowsHead=default
; How far the light turns for each degree your head turns.
; 1 matches the view, 0 keeps the light on your aim.
LightMultiplier=default

[View]
; Field of view in degrees, as the game's fov_desired: horizontal, at 4:3, and the mod
; widens it for your screen as the game does. 0 leaves the game's own. Otherwise 30 to
; 150, which fov_desired's own 20 to 120 does not bound. A zoom still narrows the view by
; the factor it always did. Black Mesa has no separate FOV for the weapon in your hands,
; so a wider Fov leaves it looking larger. Applies only while head tracking is on.
Fov=0.0

[Debug]
; true: write HeadTracking.log beside bms.exe, new at every launch, with the launch before
; kept as HeadTracking.prev.log. It records the game build, the tracker connection and
; the view the mod draws. Attach it to a bug report.
LogToFile=true
```
<!-- /cameraunlock:config -->

`Fov` accepts 30 to 150, or 0 for off, and is applied as a ratio against the
game's `fov_desired` cvar rather than written flat over your view, so weapon
zooms and scripted camera moves still work and scale by the same factor.
`fov_desired` is 90 by default on this build, so `Fov=100` is about eleven per
cent wider than stock.

Black Mesa has a field-of-view setting of its own: the `fov_desired` cvar, which
its settings script declares as a 20 to 120 slider and which you can also set
from the console. Use that if it is wide enough for you and leave `Fov=0`. The
key here writes into the render view instead, so it is not held to the cvar's
range and it changes nothing the game saves.

Whichever of the two you use, head tracking is worth the same amount of screen
at any field of view. When the game narrows it - the suit zoom, bound to `Z` by
default, or a scripted camera - your head movement is scaled so it covers the
same distance across the frame as it does walking around, instead of being
magnified along with everything else in the picture. Head roll is left alone,
because a tilt turns the picture by the same angle at every field of view.
The scaling is exactly 1:1 whenever the view is at your own FOV, `Fov`
included, so ordinary play is untouched; the log prints the factor and every
term behind it whenever it changes.

The settings are read once at startup, so restart the game after editing the
file.

If you play windowed, the mod centers the game window once at startup, on the
work area of whichever monitor it opened on, so the taskbar cannot sit over the
title bar. A window Black Mesa has already centered itself is left where it is,
as is a fullscreen or borderless one, and dragging the window later is never
undone.

## Troubleshooting

Read `HeadTracking.log`, next to `bms.exe`, first. It is rewritten from empty
every launch, so it holds the session you just played and nothing older; the
launch before it is kept as `HeadTracking.prev.log`, which is where a crashed
session ends up once you relaunch. The log records the build profile that
matched, whether the hooks installed, the tracker connection and which smoothing
is in force. Attach it to a bug report.

**Mod not loading (no log file at all)**

- Confirm `winmm.dll` is next to `bms.exe`, not in `<game>\Black Mesa\bin\`. A copy in `bin\` works when you run `bms.exe` directly and never when you launch from Steam - the overlay loads the system `WINMM.dll` first and ours can no longer be resolved.
- Confirm you took the x86 Ultimate ASI Loader. The x64 build cannot load into a 32-bit process.
- Confirm `BlackMesaHeadTracking.asi` is in that same folder, spelled as shipped, and that `LogToFile` under `[Debug]` in `CameraUnlock.ini` has not been set to `false`.
- The log is written next to `bms.exe`, alongside the two installed files.

**Log says the mod is staying dormant**

- Your `client.dll` is not in the mod's build profile registry. The log line names the direction: newer than the mod knows about (the game patched, check the Releases page), older (let Steam finish updating), or a repacked binary the mod will not engage on.

**No tracking response**

- Check the tracker is running with its output set to UDP `127.0.0.1:4242`, and that your firewall is not blocking that port.
- Press `End` (or `Ctrl+Shift+Y`) to toggle tracking on, and check `EnableOnStartup` in `CameraUnlock.ini` has not been set to `false`.
- If the log says `UDP 4242 not bound yet`, the mod could not open the tracker port. The `[receiver]` line just above it quotes the reason the OS gave, so read that rather than guessing: error 10048 is another program already sitting on the port, and it is not the only thing that can fail a bind. Keep playing. The receiver retries twice a second for as long as the game runs, and once the port is free it binds and has live tracking again inside half a second, with no restart and nothing to press. It writes `Bound UDP port 4242 after Ns of waiting - tracking is live` when that happens.
- Head tracking is suppressed outside single-player gameplay: while the game is paused, in deathmatch, while a level is loading, and on the menu background maps. The log names the reason each time it changes.

**The crosshair is not sitting on what the shot hits**

- Black Mesa draws its crosshair at screen center, and the mod leaves it there. Your shots still go where the mouse is pointed, which is the point of the mod, so with your head turned away from center the crosshair no longer marks the impact point. Turn your head back to center to line the two up again.

**Jittery or unstable tracking**

- Raise `RemoteSmoothing` in `CameraUnlock.ini` if the tracker is a phone or another device on the network, or route it through OpenTrack so its filters can clean the feed up.
- For webcam tracking, improve the lighting on your face.
- If the view flicks between two positions, two apps are both sending to port 4242. The log names both addresses; close the one you are not using.

**Leaning or turning moves the view the wrong way**

- The mod has no axis inversion setting. Flip the axis in your tracker app.
- If yaw feels wrong only when looking steeply up or down, toggle between horizon-locked and camera-local yaw with `Page Down` (or `Ctrl+Shift+H`).

## Updating

Download the new release and run `install.cmd` again. Your settings in `CameraUnlock.ini` are kept, and the installer does not touch `HeadTracking.ini`.

## Uninstalling

Run `uninstall.cmd`. This removes the mod DLL and the two log files from the
game root, and leaves `CameraUnlock.ini` and `HeadTracking.ini` in place, so a
reinstall keeps your settings. The mod loader (Ultimate ASI Loader) is only
removed if the installer put it there. Use `uninstall.cmd /force` to remove it
anyway.

## Building from Source

Requires [pixi](https://pixi.sh), CMake 3.20 or newer, and Visual Studio with
the x86 C++ toolset. Black Mesa is a 32-bit process, so the build targets Win32.

```powershell
git clone --recursive https://github.com/itsloopyo/black-mesa-headtracking.git
cd black-mesa-headtracking
pixi run build-release
pixi run test
pixi run package
```

`pixi run package` produces the installer ZIP in `release\`. No copy of the game
is needed to build.

## Community & Support

- [Discord](https://discord.com/invite/dxyZdyFNT9) - setup help, bug reports, and new-release announcements
- [Lopari](https://lopari.app) - free Windows launcher with one-click install and launch of head-tracking mods
- [Headcam](https://headcam.app) - free app that turns your phone into a head tracker

## License

MIT License - see [LICENSE](LICENSE) for details.

The mod statically links and redistributes third-party components under their
own licenses. Each is listed with its version and full upstream notice in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), and the release ZIP carries
`LICENSE` and `THIRD-PARTY-NOTICES.md` alongside the binary. The demo clip at
the top of this page contains Black Mesa footage, which remains the property of
Crowbar Collective and is not covered by the MIT license above.

## Credits

- Crowbar Collective, developer of Black Mesa
- Valve, for Half-Life and the Source engine Black Mesa is built on
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) - ASI loader (MIT)
- [OpenTrack](https://github.com/opentrack/opentrack) - head tracking protocol and software (ISC)
- [MinHook](https://github.com/TsudaKageyu/minhook) - function hooking library (BSD-2-Clause)
- [cameraunlock-core](https://github.com/itsloopyo/cameraunlock-core) - shared tracking pipeline (MIT)

## Disclaimer

This mod is not affiliated with, endorsed by, or supported by Crowbar Collective
or Valve. It requires a legitimately purchased copy of the game. Use at your own
risk.

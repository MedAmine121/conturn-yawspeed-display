<h1>conturn-yawspeed-display <img src="https://user-images.githubusercontent.com/16616463/182421854-486f911c-257c-403a-b9f1-423046c19243.png" width="24" height="23"></h1>

<img src="https://user-images.githubusercontent.com/16616463/191477910-0131d418-6065-45d6-b72a-a668c89b0249.png">

conturn provides an on-screen display (OSD) overlay for in-game `cl_yawspeed` in CS:GO, CS:S & Momentum Mod.

Whenever `cl_yawspeed` (or `_cl_yawspeed`) is changed or printed in console (e.g. from keybinds like `toggle cl_yawspeed 80 160 240; cl_yawspeed`), conturn automatically renders the current yawspeed value in an unobtrusive top-left on-screen overlay that fades away after 1.5 seconds.

- [Installation](#installation)
- [Usage](#usage)
- [How it works](#how-it-works)
- [Building](#building)

## Installation

**1. Run `conturn.exe`**

The program will run as Administrator in order to create the console log named pipe symlink.

Settings are stored in `<exe-name>.ini`.

To use with multiple games, copy `conturn.exe` to different names (`conturn-csgo.exe`, `conturn-cstrike.exe`, `conturn-momentum.exe`...) so that different settings files are used.

**2. Select your game `.exe` file** (`csgo.exe`/`hl2.exe`/`momentum.exe`)

On first run, conturn will ask for the location of your game `.exe` file. **The program is external to the game and does not patch or inject into it in any way**, the path is needed to know where to create 2 files - a log file (`<game>\conturn.log`) and a .cfg file (`<game>\cfg\conturn.cfg`). These are automatically deleted when you exit conturn.

**3. Attach to the game using `exec conturn`**

Once conturn is running (icon in the tray), attach it to the game by running `exec conturn` in console. This will not make any permanent changes to your configuration.

You can add it to your `autoexec.cfg`, or rebind the console key: ```bind ` "exec conturn; toggleconsole"```

## Usage

Bind your keys in-game to adjust `cl_yawspeed` and echo its value:

```cfg
bind MOUSE5 "toggle cl_yawspeed 70 140 210; cl_yawspeed"
```

Whenever the value is printed to the console, the OSD overlay will pop up with the current yawspeed.

To detach conturn without closing the program:
```cfg
conturn_off
```

## How it works

*Game console output is read through a named pipe.* The command `con_logfile` writes console output to a file. conturn creates a symbolic link from `<game>\conturn.log` to a Windows named pipe (`\\.\pipe\conturn-log`).

When you run `exec conturn`:
- It sets `con_logfile conturn.log`, piping console output directly to the application.
- It queries `cl_yawspeed` so the overlay displays the current value immediately.

The application parses the console stream for `cl_yawspeed` (handling Source engine prefix tags like `[engine] `) and renders the value on a transparent topmost overlay window using GDI.

## Building

### MSVC (Visual Studio)
Open a Visual Studio Developer Command Prompt and run:
```cmd
rc.exe conturn.rc
cl.exe /std:c++latest /O2 /D_CRT_SECURE_NO_WARNINGS conturn.cpp conturn.res /link /subsystem:windows
```

### Docker / MinGW
Run `./build` on a machine with Docker installed.

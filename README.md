# MQUIScale

A [MacroQuest](https://github.com/macroquest/macroquest) plugin that draws the EverQuest **Rain of Fear 2 (RoF2)** interface larger while the 3D world stays at native resolution. Useful on high-resolution monitors where the classic UI becomes tiny.

> **Made with AI.** The code in this repository was written by an AI (Claude, by Anthropic) in conversation with the repository owner, who tested every version in game. It works on the owner's setup, but it has not been reviewed by MacroQuest developers. Use at your own risk.

## Requirements

- **RoF2 emulator client only** (e.g. for EQEmu servers). The plugin uses hard-coded addresses in `eqgame.exe`, `eqmain.dll` and `EQGraphicsDX9.dll` of the RoF2 client. It will not work on live or test EverQuest, or on other emulator clients.
- MacroQuest **emu** build. Built and tested against MacroQuest `master` at commit [`bc19307`](https://github.com/macroquest/macroquest/commit/bc1930741e03370d806e623ec60070735df26d76) with eqlib `emu-rof2` at `3eafc82`. A plugin DLL must match the MacroQuest build it is loaded into; if your MacroQuest is a different version, build the plugin yourself (see below).
- **Default UI only.** Tested with the default EverQuest UI skin. Custom UI skins have not been tested and may not scale or position correctly.

## What it does

- Scales everything the client draws as UI (windows, icons, text) by a factor from 1.0 to 3.0, around the top-left corner of the screen. MacroQuest's own ImGui windows are not affected.
- Gives the UI a smaller "logical" screen (screen size / factor), keeps windows inside it, and moves them back when you return to 1.0 or unload.
- Corrects mouse input for the UI; clicks in the 3D world still get the real coordinates.
- Works from the login and server select screens onward; the zoning/loading screen is not scaled.
- Window positions saved by EverQuest (UI ini) stay correct, so playing at 1.0 later does not move your windows.
- Name tags above heads are left at their normal size.
- Optional "sharp text": glyphs are rendered again at 2x or 3x resolution so scaled text stays crisp instead of blurry.
- Settings are saved in `config\MQUIScale.ini` and applied again next time. Defaults for a new install: texture filter Sharp, sharp text on, wide letters Even, condense letters on, label room off, spaces may shrink 30 %, name tag filter on.

## Install

1. Download `MQUIScale.dll` from the [Releases](../../releases) page or from the [`dll`](dll) folder (or build it, see below).
2. Close EverQuest and MacroQuest, copy the DLL into your MacroQuest `plugins` folder.
3. Start MacroQuest and EverQuest, then in game: `/plugin mquiscale`
4. `/uiscale 1.5` (or use the slider in the window that `/uiscale ui` shows).

The plugin does nothing until you choose a factor above 1.0. `/plugin mquiscale unload` removes all hooks and restores the screen size and window positions.

## Commands

| Command | Effect |
|---|---|
| `/uiscale` | Show the current state and usage. |
| `/uiscale <1.0-3.0>` | Set the scale factor. |
| `/uiscale off` | Back to 1.0 immediately. |
| `/uiscale ui` | Show/hide the settings window (slider and options). |
| `/uiscale filter game\|sharp\|smooth` | Texture filtering for scaled UI: as the game does, point (sharp) or linear (smooth). |
| `/uiscale sharptext on\|off` | Render text at higher resolution for crisp scaled text. |
| `/uiscale wide even\|overhang\|squeeze\|smaller\|spacing` | How sharp text handles wide letters (W, M) that do not fit the game's letter spacing. `even` (spread letters evenly over the line) gave the best results. |
| `/uiscale condense [on\|off]` | With `wide even`: draw a crowded line slightly narrower instead of letting letters overlap. |
| `/uiscale labelroom [on\|off]` | Experimental, works poorly, best left off. |
| `/uiscale dump` | Write diagnostic information to `Logs\MQUIScale.log`. |

## Known limitations

- RoF2 only, and only the MacroQuest version mentioned above has been tested.
- Only tested with the default UI; custom UI skins are untested.
- The inventory character preview (the "doll") is not scaled.
- Wide letters with sharp text are a compromise: in short capital words (e.g. "MAG" next to "Magician") letters can still come close together. Turn `sharptext off` if you prefer the game's own (blurrier) text.
- Scaling is the same for every window; there is no per-window scale.
- Some context menus may open at the unscaled mouse position.
- Changing the scale while windows are open can occasionally put a window in an odd place; drag it back once and it stays.

## Building

1. Clone MacroQuest with submodules and check out the tested commit (or the emu build you use):
   ```
   git clone --recurse-submodules https://github.com/macroquest/macroquest.git
   cd macroquest
   git checkout bc1930741e03370d806e623ec60070735df26d76
   git submodule update --init --recursive
   ```
2. Build MacroQuest for the RoF2 emulator as described in its own documentation (Win32, Release).
3. Clone this repository into `macroquest\plugins\MQUIScale`.
4. Build the plugin, from the `macroquest` folder:
   ```
   MSBuild.exe plugins\MQUIScale\MQUIScale.vcxproj /p:Configuration=Release /p:Platform=Win32 "/p:SolutionDir=<path to macroquest>\src\"
   ```
   The DLL ends up in `build\bin\release\plugins\MQUIScale.dll`. Alternatively add the project to the MacroQuest solution in Visual Studio.

## How it works (short)

The RoF2 client draws its whole UI from `C2DPrimitiveManager::Render` in `EQGraphicsDX9.dll`, using pre-transformed (`D3DFVF_XYZRHW`) vertices. The plugin detours that function and Direct3D 9's `DrawPrimitive`/`DrawIndexedPrimitive`, copies the UI vertices into its own buffer scaled by the factor, and draws from that; the client's own buffers are never changed. The UI's screen extent, window positions and mouse input are adjusted to match. The comment at the top of `MQUIScale.cpp` explains the details.

## License

GPL-2.0, the same license as MacroQuest. See [LICENSE](LICENSE).

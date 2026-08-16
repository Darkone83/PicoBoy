# Contributing to PicoBoy

Thanks for your interest in PicoBoy! Issues, ideas, and pull requests are all
welcome. If you just want to chat or ask a quick question, hop on the
<a href="https://discord.gg/k2BQhSJ"><img src="https://github.com/Darkone83/PicoBoy/blob/main/images/discord.svg"></a>.

## License

PicoBoy is licensed under the **GNU General Public License v3.0**. By
contributing, you agree that your contributions are licensed under GPL-3.0 as
well. Please don't paste in code that isn't compatible with GPL-3.0.

## Reporting a bug

Open an issue and include:

- What you were doing and what happened (vs. what you expected).
- The system and game, if it's game-specific (e.g. "NES, Super Mario Bros").
- Your hardware (screen / battery / speaker modules, home-made vs. the PCB).
- The **status LED** colour and blink pattern if it errored — that's often the
  quickest clue (see the LED table in the README).
- A photo or short video if the problem is visual.

## Suggesting a feature

Open an issue describing the idea and why it'd be useful. Keep in mind this runs
on a small microcontroller with tight RAM and CPU limits — smaller, focused
additions are much easier to land than large ones.

## Pull requests

1. **Open an issue first** for anything non-trivial, so we can agree on the
   approach before you spend time on it.
2. Fork the repo and make your change on a branch.
3. Keep pull requests **small and focused** — one thing per PR.
4. Test on real hardware where you can, and say in the PR what you tested on.
5. Describe what changed and why.

### A few house rules

- **Match the surrounding style.** Look at the file you're editing and follow its
  conventions rather than reformatting it.
- **Don't remove attribution.** The emulator cores and libraries in this project
  keep their original credits and license headers — leave those intact (see the
  Credits section of the README).
- **Don't commit build output.** The `build/` folder and generated artifacts stay
  out of the repo.
- **One concern per commit** where practical — it makes review and history far
  easier to follow.

## Building from source

You'll need the **Raspberry Pi Pico SDK (2.x)** and a working ARM toolchain. The
easiest path is the **Raspberry Pi Pico** extension in VS Code, which handles the
SDK and toolchain for you — open the folder and use Compile.

Command line:

```
export PICO_SDK_PATH=/path/to/pico-sdk
mkdir build && cd build
cmake ..
ninja        # or: make -j
```

The build produces `build/picoboy.uf2`. Flash it by holding **BOOTSEL**, plugging
in over USB, and dragging the `.uf2` onto the **RPI-RP2** drive.

## Questions

Not sure about something? Open an issue or ask on the
<a href="https://discord.gg/k2BQhSJ"><img src="https://github.com/Darkone83/PicoBoy/blob/main/images/discord.svg"></a> — happy to help you get started.

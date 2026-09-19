> [!CAUTION]
> This project is in early development. It currently does not display a screen, due to a number of issues, but the dev team of the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) are working on making fixes, and hopefully we'll be able to see more progress soon :D

<img width="1100" height="330" alt="Comp_1_0-00-00-00" src="https://github.com/user-attachments/assets/e4bca27a-b84f-463b-bb25-52c8c78fb4c5" />

# EdgeOfTime-Recompiled

EdgeOfTime-Recompiled (codenamed "reeot") is an unofficial PC port of the Xbox 360 version of "Spider-Man: Edge Of Time" created through the process of static recompilation. The port offers Windows, macOS, and Linux support with goals for numerous built-in enhancements such as high resolutions, ultrawide support, high frame rates, improved performance and modding.

***This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build EdgeOfTime-Recompiled.***

EdgeOfTime-Recompiled uses the ReXGlue SDK to convert PowerPC Assembly to static C++ code that can be compiled to any platform, with a custom XenosRecomp fork to convert Xbox 360 Shaders from the compiled shader container to static HLSL code. Given that the system is currently in development, there are no recommended or minimum settings designed at the moment, and no playtesters to confirm or deny any settings. As mentioned in rules, ***THERE IS NO OFFICIAL RELEASE AS OF JULY 2026, DO NOT TRUST ANYONE CLAIMING THEY HAVE A PC PORT***

## Contact

[Recomp Discord Server](https://discord.gg/PsReBEDDZX) - Easiest way to connect with the development team of this project

# TODO

## Situation: Repository is DNI
Currently, this README and a majority of this repository is incomplete. The repository will give you enough information to be able to statically recompile the `default.xex` (with TU) and `GameLogic.dll` binaries of the title Spider-Man Edge of Time. Due to significant restrictions in how the emulated rendering system of ReXGlue works, we CANNOT use it.


https://github.com/user-attachments/assets/87c2a478-b94e-43d0-8f5f-5c23e10ffd2e

## Solution: Rendering replacement
Due to this significant constraint, the development team of EdgeOfTime-Recompiled has had to make a custom "native" rendering solution behind closed doors. This rendering solution replaces the fundamentals set in Xenia, and hooks directly onto the executable's embedded D3DX9 library to execute rendering calls. There is currently a private repository that [Graine25](https://github.com/Graine25) hosts away from the public. Eventually, when he is finished with updating that repository, he will send out builds to play testers and many alike to review. As some proof of existence, here is some pre-release footage:


https://github.com/user-attachments/assets/95bbe327-8363-4dbc-96ef-e0079e6e0a73

### Native Rendering Explanation

The game's rendering calls are hooked one by one and replayed on a modern GPU API (D3D12 on Windows, Vulkan elsewhere). The catch with modern APIs is the pipeline: on the Xbox 360 the game simply switched shaders and render state whenever it liked, but D3D12 wants every combination of vertex shader, pixel shader, vertex layout, blend, depth and rasteriser state compiled up front into a "pipeline state object" (PSO). The game has thousands of these combinations, and every one the port meets for the first time has to be compiled right there on the render thread, which costs several milliseconds and shows up as a stutter. That is the hitching people notice in early builds, and it has nothing to do with how fast the machine is.

reeot deals with it in three layers. First, a **shipped table**: every pipeline ever captured from a play session is stored as a row in `config/pso/eot_pipelines.csv` and compiled into the executable. Rows that belong to no particular level are built on worker threads during the intro, and rows tagged with a level package are built when that package loads, under the game's own loading screen, which the port holds until they are ready. Second, a **predictor**: when the game streams a material or a model, the port already knows its shaders, vertex layout and strides, so it crosses them with the render-state templates learned from previous captures and builds the likely pipelines before the first draw needs them. Third, **capture**: whatever still slips through and has to be built at draw time is written to `pso/pso_misses_<machine>_<time>.csv`, tagged with the level it happened in, together with a small file saying which predictions were actually used.

Those capture files are the whole point of the playtest. Each tester plays through the game and sends back their `pso/` folder; the files are merged into the table (`tools/pso/pso_merge.py`), duplicates collapse, anything the shader cache cannot build is dropped, and the templates are regenerated from what the sessions actually drew. Every round of testing shrinks the number of pipelines nobody has seen yet, and a session whose capture file comes back empty means that build already covered everything it drew. The end goal is that by release every pipeline the game can ask for is already in the executable, so nobody ever compiles one while playing and the port runs as smoothly on the first launch as it does on the tenth.

# Contributions
### What we need
At the moment, contributions to this repository are paused, given the fact that the project is NOT in a releasable state. If you wish to contribute to reeot, we desperately need people who have experience in Graphics Programming and Graphics debugging, as well as people who are experienced in reversing the game's PowerPC assembly. RenderDoc and IDA Pro are a must.

### AI Usage
Whilst the codegen, ReXGlue SDK, and overall current setup/mods were made by hand, I would not have been able to get my rendering implementation without the help of many talented individuals at the ReXGlue team. I have no problems with using AI for this project, but I do **require that everyone who contributes has a strong understanding of what they're getting into**. This repository has been setup in a way that will not allow individuals to AI generate nonsense and push it upstream. I do not plan on vibecoding this entire system as I am very passionate about the game, and I want others to enjoy it as much as I have and be able to use the knowledge learnt from reversing on other titles with a similar setup. 

Do **NOT** make pull requests spearheading with an LLM, or spam issues with information provided by one of the many providers. I would prefer to see human reports as to what is going wrong, and how you've approached the sitation. I plan on detailing the process of how to build the system and how to use it/report issues properly. I appreciate everyone who's been along for the ride, and wish to continue building this piece-by-piece :)

# Credits

Huge thanks to everyone who's put time into this. EdgeOfTime-Recompiled wouldn't be where it is without you.

* **[Graine25](https://github.com/Graine25)**: Creator of EdgeOfTime-Recompiled and maintainer of the ReXGlue SDK.
* **[Serjar](https://www.youtube.com/channel/UCaCoblwXlhhZFoJVPc8L2cg)**: one of the few people outside of the original beenox dev team who knows EdgeOfTime like the back of their hand. A lot of the reversing, between understanding the PAK format and how it interacts in the game code, would not have been possible without his help.
* The **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)** team, for the toolchain this project is built on.
* **[UnleashedRecompiled](https://github.com/hedge-dev/unleashedrecomp/)** for setting the bar on how incredible a Static Recompilation can be, and proving to a wider audience that 360 titles can be relived on modern computers. 
* The wider **Xbox 360 emulation scene**. A lot of the hardest problems were solved by them long before this project started.

## Building

The build needs a ReXGlue SDK install (its `rexglue` codegen tool on PATH),
CMake 3.25+, Ninja, Python 3 and clang. Every platform builds through the
presets in `CMakePresets.json`; `scripts/build.bat` and `scripts/build.sh`
run codegen, configure and build in one go.

### Windows

```
scripts\build.bat
```

D3D12 is the renderer (`REEOT_D3D12=ON`, the default on Windows). The
preset names the SDK source tree through `REXSDK_DIR`.

### Linux (including Steam Deck)

```
scripts/build.sh                      # linux-amd64-relwithdebinfo
scripts/build.sh linux-amd64-release
```

Vulkan is the renderer; the window and its surface come from SDL3, so X11
and Wayland both work without being named. Needs clang and lld (LLVM 19 or
newer), the Vulkan loader (`libvulkan1` on Debian and Ubuntu,
`vulkan-icd-loader` on Arch; SteamOS has it), and the `rexglue-sdk-dll`
checkout beside this repository, the same sibling the Windows preset names:
the SDK builds inside this tree, its vendored Vulkan headers stand in when
the system has none, and its codegen runs as part of the build. The result
is `out/build/<preset>/reeot` with `librexruntime.so`,
`libreeot_GameLogic.so`, `gamecontrollerdb.txt`, `build_stamp.txt` and
`reeot_icon.png` beside it; that folder is what the first-run installer
copies into `EdgeOfTimeRecompiled`. The install record lives in
`~/.config/reeot/install.toml`; the desktop entry the installer offers goes
to the desktop and to `~/.local/share/applications`.

To run against an existing game folder without installing:

```
out/build/linux-amd64-relwithdebinfo/reeot --game_data_root /path/to/EdgeOfTimeRecompiled/game
```

#### Playtest package (AppImage)

```
scripts/package-appimage.sh
```

Stages the build's program files into an AppImage (`dist/reeot-v<version>-<stamp>-x86_64.AppImage`,
copied to `~/Downloads` as the Windows zip is), after checking that nothing
bundled needs a glibc or libstdc++ newer than the host's (`MAX_GLIBC` and
`MAX_GLIBCXX` set the ceiling for a package meant for older systems; SteamOS
3.6 is glibc 2.38). The AppImage is the program, as reblue's is: its first
run is the installer, which unpacks the game into `EdgeOfTimeRecompiled`,
records it in `~/.config/reeot/install.toml` and carries on into the game
in the same process; nothing is copied beside the game and nothing restarts.
Every later run of the AppImage reads the record and boots. Its log goes to
`~/.local/state/reeot/logs`, since the mount is read-only; profiles and
saves live under the install folder.

## License

See [LICENSE](LICENSE).



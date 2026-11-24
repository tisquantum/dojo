# Dojo (Bushido Blade for PC)

A native Windows port of the decompiled game in `src/`, with three additions the console never had: frame
rates above the game's 30 (60, 90, 120, 144, 165 and beyond), any aspect ratio, and online play with rollback.

It needs the player's own disc image. Nothing from the disc is in this tree; the build and the game read it
from `game_files/` (ignored by git). The North American disc, `Bushido Blade (USA).bin` with SHA-256
`c5cba1e3…0bf`, is the supported one.

## State

The game boots and plays natively, with sound: the publisher's splash, the opening demo and its staff roll,
the title menu, character and weapon select, the Story prologue, VS bouts, and the movies (the opening in the
Movie Theater, the endings) with their sound. A Vulkan renderer draws at
any multiple of the console's resolution with sub-pixel geometry, in any aspect ratio. Combat runs natively at
the display's rate (60, 120, 144, 165 and up: the game's own logic, more often, not frames in between). Two
machines play online with rollback, at different refresh rates too. Not yet: rumble.

## Building and running

Clang (LLVM for Windows), the Visual Studio 2022 x86 libraries, CMake, Ninja, Python 3 and the Vulkan SDK (for
its shader compiler; without `VULKAN_SDK` set, the build fetches the glslang release instead). The configure
step downloads SDL 3.2.10 and the Vulkan headers (each checked by SHA-256) into the build directory.

```
cmake -S port -B port/build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build port/build
port/build/dojo.exe "game_files/Bushido Blade (USA).bin"
```

Options: **F1** (or Esc, or a gamepad's guide button) opens the options menu over the game, which waits
while it is open (online it runs on). Frame rate, resolution, full screen, vsync, volume and a frame counter
apply at once; aspect ratio and renderer on the next start; online the frame rate is the match's. Settings are
kept in `dojo.ini` beside the executable. Environment variables set them for one session:
`DOJO_ASPECT` (`16:9`, the default; `21:9`, `4:3`, ...), `DOJO_SCALE` (resolution as a multiple of the
console's, default 4), `DOJO_RENDERER=software` (the 1x CPU renderer), `DOJO_FPS` (the simulation's
rate, rounded to a multiple of 30 up to 480; the default follows the display, `30` is the console's game
exactly).

Online: **Play online** in the menu (F1) offers quick play, private matches (one player creates a room and
reads its six-letter code to the other), and King of the Hill (the winner stays on, the others wait in line;
a hill can be public or private by code), through the matchmaking server in `port/server`. A private match
can also go player to player with no server: **Host directly** waits on UDP port 7778 (`p2p_port` in
`dojo.ini`; forward it on the router to play over the internet) and shows this PC's addresses, and **Join
directly by address** connects to one. The rest go through the server (its README says
how to run it on fly.io; `server = host:port` in `dojo.ini` points the game at it, `name = ...` sets the
name others see). Holding Back (Backspace or the gamepad's back button) for two seconds leaves a queue, goes
back a page, or leaves a match. While waiting for an opponent the offline game goes on; on a hill, those in line watch the fight being fought
(the players send its settled inputs through the server and each watcher runs the same fight from the same
start a second behind; the waiting page turns it off, holding Back stops it). Each match starts from
the VS character select, the same state on both machines (each makes it from power-on and they compare);
the offline game is put aside and comes back after. Direct play without the server: one machine runs with
`DOJO_NET=host:PORT`, the other with `DOJO_NET=join:ADDRESS:PORT` (UDP; the host's port must be
reachable). `DOJO_NET_DELAY` sets the input delay in fields (default 2; the two
take the larger). The window title shows the round trip, the rollbacks and any desync. Memory cards are off in
an online match. `DOJO_NET_LAG` (ms) and `DOJO_NET_LOSS` (percent) simulate a bad network.

Keyboard: arrows, Z square, X cross, C circle, S triangle, Q L1, E R1, 1 L2, 3 R2, Enter start, Backspace
select; F11 full screen. Gamepads work as the first and second controller. Saves go to `saves/bu00`.

For development: `port/check.sh` syntax-checks the game against the SDK headers; `port/undefined.sh`
builds and lists what the link still lacks. Scripted runs use environment variables: `DOJO_PRESS`
(`field:buttons,...`, `p2` for the second pad), `DOJO_SHOT_EVERY` (BMP screenshots), `DOJO_EXIT_FIELD`,
`DOJO_FAST` (no real-time pacing), `DOJO_ONLINE` (`quick`, `create`, `join:CODE`, `koth`,
`koth:CODE`: online play from a script), `DOJO_SERVER`, `DOJO_NO_DIRECT` (always relay), `DOJO_MENU_AT` (open the options menu at a field and save its
picture), `DOJO_APPLY_AT` (`field:scale=N,field:vsync=N,field:fps=N`, settings changed mid-run), `DOJO_HASH_EVERY` (log the simulation's hash), `DOJO_STATE_TEST=F,G`
(save at F, run to G, restore and run again, compare), `DOJO_DUMP_STATE=F` (the state, its regions on
stderr). `tools/statediff.py` compares two dumps variable by variable through the builds' PDBs (pointers by
what they name), so a build can be checked against an older one even when variables were added.
`-DDOJO_TRACE=ON` keeps a call stack of the game's functions for crash reports; crashes print addresses to
resolve with `llvm-symbolizer --obj=port/build/dojo.exe`.

## Design

**The game stays C, and stays the decompilation.** The 98 units of `src/` compile as they are, but for three
places that depend on where the console's linker put things (each under `#ifdef TARGET_PC`, so the matching
build is untouched): the model arena's buffers kept end to end (`model_arena.c`), a table the select screen's
host figure writes a third row of (`fighter_pose.c`), and a fade drawn before any has started
(`screen_fade.c`); a null collider the select screen writes through (`fighter_cloth.c`); and the frame-rate
changes below.

**32 bits, with the console's memory where the console has it.** The game is 32-bit to the bone: disc files
hold absolute pointers for their load addresses, and ordering-table links hold 24-bit addresses. The port is
an x86 executable linked at 0x400000, so the image (globals, the 1 MB VRAM, the game's stack) lies below
16 MB, and it maps the console's 2 MB of RAM at 0x80000000 (`src/memory.c`), the address the console's code
and data use. A link below 0x200000 is in that RAM; any other is in the image.

**The hand-written assembly is translated, not rewritten.** `tools/mips2c.py` turns the renderer
(`src/render.s` and three small units) and LIBGTE, which the decompilation holds as assembly, into C at build
time. Every instruction keeps a code address, so the assembly's own tricks (state in registers across
calls, drawers returning into the middle of other routines, a jump into an unrolled copy) work as written;
delay slots, the stack arguments of the MIPS ABI and GTE instructions are handled exactly.

**The libraries the game calls are the port's, or the decompilation's own.**

| Library | In the port |
| --- | --- |
| LIBGTE | translated from the decompiled assembly, over a register-accurate software GTE (`src/gte.c`) |
| LIBGPU | the decompiled `prim.c` and `ext.c`; the system layer (`src/libgpu.c`) over a software rasterizer of the GPU's commands into 1 MB of VRAM (`src/gpu.c`) |
| LIBCD | a virtual drive over the disc image at command level, with LIBCD's streaming ring for the movies (`src/libcd.c`) |
| Kernel, LIBETC, pads | `src/kernel.c`: events, root counters, vertical blank, the pad buffers |
| Memory card | `src/memcard.c`: a formatted card per slot, a folder of files |
| LIBSND, LIBSPU | the decompiled libraries whole, over an emulated SPU (`src/spu.c`) behind its registers (`src/mmio.c`); XA decoded from the drive (`src/xa.c`) |
| LIBPRESS | the decompiled library (its VLC decoder translated from assembly) over an emulated MDEC (`src/mdec.c`) |
| LIBCOMB | a stand-in (`src/interim.c`): the link cable is absent |

**Hardware registers.** LIBSPU and LIBSND drive the sound processor through its registers, as they do on the
console. The I/O page is mapped read-only at 0x1F801000, kept current by the emulated devices; every write
faults, and the handler decodes the store and hands it to the device (`src/mmio.c`).

**Movies.** The player (`src/movie.c`) streams each STR file with `CdRead2` and LIBCD's ring, decodes a frame's
bitstream with LIBPRESS's `DecDCTvlc` and has the MDEC turn it into pixels. The drive reads at the console's
double speed, putting each video sector's header and data where LIBCD's interrupt handler would and handing the
interleaved XA sectors to the audio decoder, so picture and sound keep the console's timing. The MDEC sits
behind its registers and DMA channels 0 and 1 (`src/mdec.c`): run-length blocks, zigzag dequantisation, the 8x8
inverse DCT by the table LIBPRESS loads, YUV to RGB, in integers. A movie is simulation like the rest: a save
state taken mid-movie runs on identically.

**Rendering.** `src/vk_renderer.c` (Vulkan 1.3) draws the GPU's primitives at a multiple of the console's
resolution into surfaces of its own; its shaders read textures and colour tables from a copy of video memory
as the console's GPU does. Rendered pixels go back to video memory only when the game reads them. Each
projection's exact position is kept (`GtePreciseLookup`, `src/gte.c`), so geometry is placed to the sub-pixel
rather than the console's whole pixels. Widescreen widens the display buffers; the drawers' and the stage
cull's edge tests are widened to match (`tools/mips2c.py`, `src/stage_cull.c`).

**Time is emulated, so the game is deterministic.** The game runs in a context of its own (`src/context.c`)
and switches to the host at each vertical blank. Its clock advances when it reads the hblank counter or
waits for a blank, never with wall time, so the main loop's `rand()` spin runs the same number of times every
frame and a bout is a function of the pads alone: two runs with the same input are identical frame for frame.

**Save states and determinism.** Everything the simulation touches (the game, the decompiled libraries, the
translated assembly and the port's emulated hardware) is compiled into its own sections (`src/sim_sections.h`);
a save state is those, main RAM, the I/O registers and the used part of the game's stack with its saved
stack pointer, so it can be taken at any vertical blank, mid-load included (`src/savestate.c`, about 5 MB).
Host work (input, audio output) runs on the host's stack, never the game's. The simulation is a function of
the two controllers alone: its hash is the same field for field with the software renderer at 1x and Vulkan at
2x or 4x. The widescreen setting does change what the game writes (the drawers fill packets for faces in the
margin), so it is a setting of the simulation, agreed in an online match like the input delay.

**Rollback** (`src/netplay.c`). Each field each machine saves the state, runs both players (the other's input
predicted as its last), and when the real input arrives and differs, loads the state before that field and
runs forward again with drawing and sound left out. Inputs go over UDP with everything unacknowledged
repeated, so loss costs nothing; the machine that runs ahead waits a field now and then; every 30 fields both
compare the hash of a field they have both settled. Tested on one machine with 80 ms of added lag and 10%
loss each way, one side 16:9 and the other 4:3: boot to a VS bout, about 200 fields rolled back on each side,
every one of 89 settled hashes equal.

**Frame rates above 30, by the game's own code** (`include/substep.h`, `port/sdk/include/pc_substep.h`). A
console frame of combat is run as N ticks (N = rate / 30, 1 to 16); each tick runs the whole combat state and
draws a picture. Continuous motion advances by the tick's share of the frame: the animation clock (poses and
root motion already interpolate within a frame), velocities and gravity, turning, the camera's smoothing, the
look angles, particles, waves (`PC_SHARE`; a per-frame damping or approach becomes its N-th root,
`PC_DECAY_EX`, `PC_APPROACH_EX`). What happens once a frame stays once a frame, on its last tick, which is the
console's frame: pad edges, the CPU's decisions, hits and clashes, timers and lifetimes, spawns, sounds, the
cloth solve (drawn carried with the body between solves), every `rand()`. Every macro is the original code with
one tick a frame and on the console (`include/substep.h` keeps the matching build), and frames of any state that
doesn't substep (menus, loading, replays) run as one tick at any rate, so `DOJO_FPS=30` is the console's game:
a 6000-field run with three bouts compares equal to the build before these changes in every variable and all of
main RAM (`tools/statediff.py`). At 60, 120, 144 and 480 runs are deterministic and pass the save-state round
trip; online, a 60 Hz and a 120 Hz machine agree on 4 ticks and settled 149 of 149 hashes equal through rollbacks
under 80 ms lag and 10% loss. Online the rate is agreed like the input delay (the faster).

## Order of work

1. Other states at high rates (replays, the victory and story scenes run at 30 for now).
2. Rumble.

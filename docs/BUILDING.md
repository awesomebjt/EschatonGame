# Building Eschaton

All builds produce the same thing: `cmake --build <dir> --target dist` assembles
`<dir>/dist/eschaton/` (the game `eschaton`, the server `eschaton_server`,
`shaders/`, `world/`) plus an archive of it, `.tar.gz` on Linux and `.zip` on
Windows. Unpack it anywhere and run `eschaton`; it finds `world/` and `shaders/`
beside itself.

Every build needs network access on first configure (FetchContent pulls SDL3,
bgfx, EnTT, ENet and mruby) and **Ruby with rake** on the build machine (mruby is
built by rake). A first build compiles ~2,300 files, most of them bgfx's shader
compiler; keep parallelism modest (`-j4`) on machines under 16 GB.

## Linux (development, Fedora toolbox)

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-clang.cmake -B build
cmake --build build -j4 --target dist
```

The binary needs a glibc at least as new as the build machine's. To run on
older distros, build in the Steam Runtime instead.

## Linux (portable, Steam Runtime 3 "sniper" SDK)

glibc 2.31, so the result runs on effectively any current desktop distro. Not
yet tried end to end. From a host terminal (podman, not toolbox):

```bash
podman run -dit --name sniper-sdk \
  -v "$HOME/Code/EschatonGame:/src:z" -w /src \
  registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest bash
podman exec -it sniper-sdk bash

# once, inside: the SDK has no ruby; it is a build tool only, never shipped
echo 'deb http://deb.debian.org/debian bullseye main' > /etc/apt/sources.list.d/bullseye.list
apt-get update && apt-get install -y --no-install-recommends ruby rake

# the SDK's clang (11) is too old for C++20; use its GCC 14, and link its
# libstdc++ statically because the runtime ships GCC 10's
CC=gcc-14 CXX=g++-14 cmake -S . -B build-sniper -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build build-sniper -j4 --target dist
objdump -T build-sniper/eschaton | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1   # expect <= 2.31
```

## Windows (MSVC)

Not yet built on Windows; the project-side changes are in place, and the game's
own sources were checked against the Windows headers, but expect a first MSVC
build to turn up something. Tested target: Windows Server 2025 / Windows 10+.

Install (winget shown; the plain installers work the same):

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install --id Git.Git
winget install --id RubyInstallerTeam.Ruby.3.4
```

The C++ workload brings MSVC, the Windows SDK, CMake and Ninja. Then, from an
**x64 Native Tools Command Prompt for VS 2022** (mruby's build calls `cl.exe`
directly, so it needs that environment):

```bat
git clone <repo-url> EschatonGame
cd EschatonGame
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4 --target dist
```

Do not pass `toolchain-clang.cmake` here. The output is `build\dist\eschaton.zip`.

What the Windows build does differently (all guarded in `CMakeLists.txt`, so the
Linux builds are unchanged):

- **Static C runtime (`/MT`)** for every target and for mruby, so players need no
  Visual C++ redistributable; only Direct3D, which Windows ships.
- **Shaders:** `dxbc` (Direct3D 11, bgfx's default on Windows, and 12) and `spirv`
  (Vulkan). DXIL is skipped because it needs DXC beside shaderc.
- **mruby** builds with its `visualcpp` toolchain, run as `ruby -S rake`.
- **ENet** consumers get `NOMINMAX` and `WIN32_LEAN_AND_MEAN`, since `enet.h`
  includes `<windows.h>`.

## Running a multiplayer test

- Server: `eschaton_server` (or `eschaton --server`, the same thing). It prints
  an invite code, new on every start, and listens on **UDP 47210** (`--port n`).
  `eschaton_server` is the one for a headless machine: ~0.4 MB, no window, world
  or shaders, and nothing beyond the C and C++ runtimes (no X11, GL or Wayland
  libraries, which the game binary needs even with `--server`). Built in the
  Fedora 44 toolbox it needs glibc 2.34 or newer (Amazon Linux 2023, Ubuntu 22.04,
  Debian 12, RHEL 9). It uses about 5 MB of RAM and ~2% of one core; four players
  move about 11 KB/s in and 32 KB/s out, so the smallest instance is plenty and
  the region (latency to the players) matters more than the size.
  Open that port on the server: `firewall-cmd --add-port=47210/udp` on Fedora,
  `New-NetFirewallRule -DisplayName Eschaton -Direction Inbound -Protocol UDP -LocalPort 47210 -Action Allow`
  on Windows, and the security group on EC2.
- Clients: `eschaton --connect <server>[:port] <invite>`.
- A client stays connected while its game stalls (loading, a minimised window
  under Wayland, a debugger): networking runs on its own thread. Its player just
  stands still for the others until the game resumes. The game itself still
  stops rendering while the compositor isn't presenting its window, so for
  scripted runs (`--shot`, `--exec`) keep the window visible or use
  `SDL_VIDEO_DRIVER=x11`; several windows opened at once from the background may
  all be held back by GNOME.

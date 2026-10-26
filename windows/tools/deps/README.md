# Native graphics dependencies

Three scripts build the pinned, patched graphics libraries the port needs.
Each takes `WORK_DIR PREFIX` (setup.sh passes `<game repo>/build.nosync/deps/<name>-work` and `.../<name>`).
Each script skips any step whose output already exists, so you can run it again after a failure.

| Script | Source pin | Patches | Install output | Prefix variable |
|---|---|---|---|---|
| `build_dxvk.sh` | DXVK `02e2028c3fe954c8913b1f39bce2f581739f0c41` (git) | `tools/dxvk_test/patches/0001-0004` (`git am`) | `lib/libdxvk_d3d9.0.dylib`, `include/dxvk` | `DXVK_PREFIX` |
| `build_vkd3d.sh` | vkd3d `fed0c3418f815b0bc07533f6fc6f4aa06e8712c3` (git); widl from Wine `840a939ee526137639665ff5f587224d6a36403f`; Perl JSON 4.11 | `tools/vkd3d/patches/0001` (`git apply`) | `lib/libvkd3d-shader.1.dylib`, `include/vkd3d_shader.h` | `VKD3D_PREFIX` |
| `build_kosmickrisp.sh` | Mesa 26.2.5 tarball (sha256 checked); LLVM 23 | `tools/package/patches/kosmickrisp-*.patch` (`patch -p1`) | `lib/libvulkan_kosmickrisp.dylib`, `share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json` | `KK_PREFIX` |

```
windows/tools/deps/build_dxvk.sh <deps>/dxvk-work <deps>/dxvk
windows/tools/deps/build_vkd3d.sh <deps>/vkd3d-work <deps>/vkd3d
windows/tools/deps/build_kosmickrisp.sh <deps>/kosmickrisp-work <deps>/kosmickrisp     # long: Mesa with LLVM
```

## Requirements

An arm64 Mac with the Xcode Command Line Tools, git, curl and network access.
The scripts do not install Homebrew packages. They stop and list any that are missing.

- DXVK: `meson ninja pkgconf glslang sdl2`
- vkd3d: `autoconf automake libtool bison flex mingw-w64 pkgconf vulkan-headers vulkan-loader`
- KosmicKrisp: `meson ninja pkgconf bison glslang llvm spirv-llvm-translator spirv-tools zstd python@3` (`llvm` must be version 23)

The scripts set PATH to Homebrew and the system tools only. An x86_64 tool early in PATH breaks `xcrun` and configure.
Set `BREW_PREFIX` if Homebrew is not in `/opt/homebrew`, and set `JOBS` to change the build parallelism (the default is the core count).

## Notes

- DXVK's version string comes from `git describe`. The script applies the patches with a fixed committer and date, so every machine builds the same commit ids.
  The version is `v3.1.1-157-gbf8a13bc`. The source tree is identical to the hand-built one (`v3.1.1-157-g04e66af0`); only the commit ids differ.
- The install names of the libraries are absolute paths in the prefix. `make_app.sh` rewrites them when it bundles the app.

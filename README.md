# Visual Pinball (Android Fork)

Fork of [vpinball/vpinball](https://github.com/vpinball/vpinball) (v10.8.1) with Android platform modifications. All credit for the core engine goes to Randy Davis and the [Visual Pinball development team](https://github.com/vpinball).

## What this fork adds

- OpenGLES and Vulkan rendering via BGFX for Android
- Memory optimizations (scaled JPEG/PNG/EXR decode, texture thread capping)
- Input polling inside the physics loop for reduced flipper latency
- Wine VBScript class emulation and script patching for table compatibility
- ScoreView capture via BGFX screenshot for JNI integration
- Render thread starvation fixes for Android

## Script patcher authoring rule

The Wine VBScript runtime on Android is stricter than the MS VBScript parser most tables were authored against. `SimpleScriptPatcher` (`src/ui/simplescriptpatcher.cpp`) rewrites table scripts to work around those strictness differences. One invariant matters above all others:

**Never emit rewrites joined by `:`. Always expand to multi-line.**

Wine's parser tolerates `: Select Case X` or `: For Each v In coll` at top level but collapses when the colon-joined statement is nested inside another `Case` body, an outer `If...Then`, or similar structures. The collapse manifests as `Script Error at line 1: Description unavailable` — Wine's parser has bailed with no useful location info, so bugs of this class are expensive to diagnose from logs.

Two fixes so far have applied this rule:

- **`PatchSingleLineForEach`** — single-line `For Each v In coll : body : Next` was originally rewritten to indexed `For i = 0 To UBound(coll)`, which silently skipped VPX Collections (Monster Bash "invisible walls" bug). Now expands to multi-line `For Each` (commit `c28a06c`).
- **`PatchSelectCaseArrayAccess`** — `Select Case Foo(i)` was originally rewritten as `vpx_ssc_tmp = Foo(i) : Select Case vpx_ssc_tmp`, which broke when the original `Select Case` was nested inside an outer `Case` body (Bigus MOD Medieval Madness `PrimStandupTgtMove`). Now emitted on two lines.

A regression harness is planned — see [tests/scriptpatcher/TESTING_PLAN.md](tests/scriptpatcher/TESTING_PLAN.md). Until it lands, any new patcher rewriter is release-blocking: every regex change has table-library-wide blast radius.

## Upstream

This fork is based on Visual Pinball X 10.8.1. For the upstream project, see [github.com/vpinball/vpinball](https://github.com/vpinball/vpinball).

## License

GPLv3+ — same as upstream. See [LICENSE](LICENSE) for details.

## Original README

*An open source pinball table editor and simulator.*

### Features

- Simulates pinball table physics and renders the table with DirectX, OpenGL or [bgfx](https://bkaradzic.github.io/bgfx/overview.html)
- Simple editor to (re-)create any kind of pinball table
- Live editing of most content within the rendered viewport
- Table logic (and game rules) can be controlled via Visual Basic Script
- Over 1050 real/unique pinball machines from ~100 manufacturers, plus over 550 original creations were rebuilt/designed using the Visual Pinball X editor
- Emulation of real pinball machines via [PinMAME](https://github.com/vpinball/pinmame) is possible via Visual Basic Script (Visual PinMAME), or via the libPinMAME-API/plugin
- Supports configurable camera views (e.g. to allow for correct display in virtual pinball cabinets)
- Support for Tablet/Touch input, Joypads, or specialized pinball controllers
- Support for Stereo3D output
- Support for Head tracking via BAM
- Support for VR/XR HMD rendering (including [PUP](https://www.nailbuster.com/wikipinup), [B2S](https://github.com/vpinball/b2s-backglass) backglass and DMD output support)
- Support for WCG/HDR rendering (for now only via the BGFX (D3D11/12) build)
- Support for Windows (x86), Linux (x86/Arm, incl. RaspberryPi and RK3588), macOS, iOS/tvOS, Android
- Plugin system to drive/fuel all kinds of displays (DMD, backglass, etc), add custom/dynamically-changed content (PUP, Serum, etc), direct output framework (DOF), sensors, and much more (WIP)

### Documentation

Documentation is currently sparse. Check the [docs](docs) directory for various guides and references.

### How to build

Build instructions are available in the [make directory README](make/README.md).

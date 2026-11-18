# Bono DMC HUD - Skyrim SE/AE port (1.6.1170)

Rewrites of `BonoDMCRank.dll` (and later `BonoDMCHUD.dll`) using CommonLibSSE-NG + Address Library,
so one build runs on SE 1.5.97 **and** AE 1.6.x including 1.6.1170.
Your existing INI / ESP / SWF / sound files from Bono's mod are used unchanged.

## Build (no local tools needed - GitHub builds it)
1. GitHub Desktop -> File -> New repository (name: `BonoDMC-AE-Port`). Copy everything from this folder into the
   repo folder (including the hidden-looking `.github` folder and `.gitignore`).
2. Commit -> Publish repository (private is fine).
3. On github.com open the repo -> **Actions** tab. Enable workflows if asked. The "Build DLLs" run starts on push
   (or use *Run workflow*). First run takes ~10-20 min.
4. When it's green, open the run -> **Artifacts** -> `BonoDMC-AE-DLLs`. It contains `SKSE/Plugins/BonoDMCRank.dll`.
   If it's red, open the failed step and send me the log text.

## Install
- Needs: SKSE64 for 1.6.1170 and **Address Library for SKSE Plugins** (AE version, `versionlib-1-6-1170-0.bin`).
- Keep Bono's original mod installed (SWFs, sounds, ESP, INI). Replace only `Data/SKSE/Plugins/BonoDMCRank.dll`
  with the new one (the old one will not load on AE anyway).
- To debug: set `LogEnabled=1` (and optionally `LogAnimEvents=1`) in `BonoDMCRank.ini`, play, and read
  `Data/SKSE/Plugins/BonoDMCRank.log` (MO2: overwrite folder).

# baresip-qt

Qt tray/call UI module for baresip, packaged for Arch/Manjaro (KDE Plasma 6, Wayland, PipeWire).

## Branches

- `call-dialogue` — published branch tracked by the AUR package `baresip-qt-gui-git`.
- `upstream-core` — Option B branch: core (`src/`, `include/`, `modules/*` except `qt/`) is kept byte-identical to upstream baresip; ALL fork-specific logic lives in `modules/qt/` (PID lock, config migration, contacts seeding) and `share/`.
- `call-dialogue-import` — historical CSV import development branch (merged).

## Upstream tracking (upstream-core branch)

- Remote `upstream` = https://github.com/baresip/baresip.git
- To track upstream: `git fetch upstream && git merge upstream/main`
- The core must stay unmodified — if a change seems to need core edits, move it into `modules/qt/` instead.
- Fresh installs rely on `share/baresip-qt-handler` launching `baresip -m qt` because the upstream config template does not list `module_app qt`; the qt module's `module_init()` migrates the config to add it.

## Build / test locally

```bash
cd /tmp/baresip-local-build && makepkg -cf
sudo pacman -U baresip-qt-gui-git-*.pkg.tar.zst
```

The local PKGBUILD builds from /home/www/KDE-trunk/baresip-qt (whatever branch is checked out).

## Publication

1. Push source branch to GitHub (origin).
2. AUR repo: /home/www/DEV-trunk/CxOrg-AUR/baresip-qt-gui — bump `pkgrel`, regenerate `.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO`, commit, push to `aur` remote.
3. Keep `pkgver` static (4.10.1); only `pkgrel` increments per AUR update.
4. Never update AUR without explicit user request; build and test locally first.

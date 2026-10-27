# Mark

Mark files and folders with seven colours in Dolphin. Right-click → **Mark** →
choose a colour. Choose it again to remove it, or use **Clear marks**.
Multiple colours appear together in one icon badge. Mixed selections gain the
chosen colour.

Marks survive renames on the same filesystem. File contents and existing KDE
text tags are untouched. Copying across filesystems preserves marks only when
the transfer preserves extended attributes.

## Install

Requires Linux, Dolphin with Qt 6 / KDE Frameworks 6, Python 3.10+, and PyQt6.

Debian 13 / amd64 packages:
https://github.com/emptyname-org/kde-mark/releases

```sh
sudo apt install ./kde-mark_0.1.0_amd64.deb
```

Restart Dolphin, then right-click a local file and choose **Mark**. If the menu
is hidden, enable **Mark** in **Configure Dolphin → Context Menu**.

## Build

Development dependencies: CMake, a C++17 compiler, Qt 6 Core/DBus/Test development
files and tools, and KF6 KIO development headers. On Debian 13:

```sh
sudo apt install build-essential cmake qt6-base-dev libkf6kio-dev python3-pyqt6
make build CMAKE_ARGS="-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr"
make test
make lint
sudo cmake --install tmp/build
```

`make package` creates a Debian package under `tmp/packages/`. It also requires
`dpkg-dev`. The package declares its Python and shared-library dependencies.

The build accepts `BUILD_DIR` and `CMAKE_ARGS`. `MARK_PLUGIN_DIR` can override
the overlay module's installation directory. That directory must be under
Qt 6's plugin search path, ending in `kf6/overlayicon`.

To try a built plugin without installation:

```sh
python3 tools/preview.py
```

The preview opens a separate Dolphin window with sample files and keeps its
settings and files under `tmp/preview/`. It requires `dbus-run-session`.

## Limits

- Local regular files and folders only; symbolic links and remote URLs are excluded.
- Requires read permission and a filesystem supporting user extended attributes.
- These visual marks do not sync with Finder metadata or Dolphin's searchable text tags.
- Up to 8,192 selected files. Cancelling keeps changes already made.

## Verification

Python `unittest` covers metadata and cancellation. Qt tests exercise the compiled
plugin, notifications, stale replies, retries, timeouts, and worker cleanup.
Storage code has a 90% statement-coverage target.

## Uninstall

```sh
sudo apt remove kde-mark
```

For source installations, remove only files listed in the build's
`install_manifest.txt`. Uninstalling leaves marks on files; use **Clear marks**
first to remove that metadata.

## License

[CC0 1.0 Universal](LICENSE).

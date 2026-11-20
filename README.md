# LFS build script

One script that builds [Linux From Scratch](https://www.linuxfromscratch.org/lfs/)
12.4 (SysVinit) into a bootable disk image, with Windows program support from
[lfs-pe-inspect](https://github.com/fguy06790-code/lfs-pe-inspect) built in.

It follows the LFS book itself. The script downloads the book, pulls every
section's commands out of it, and runs them in order. Only the steps that need
a person (passwords, `make menuconfig`, device names, placeholders) are
replaced, and each replacement is a readable file in [`overrides/`](overrides).

## What it does

1. **Checks the host** and asks for the new system's root password.
2. **Downloads [lfs-pe-inspect](https://github.com/fguy06790-code/lfs-pe-inspect)**
   and builds it on the host, so a broken package shows up before the long build.
3. **Downloads the LFS book**, the source list, and the checksums, and turns the
   book's commands into numbered scripts.
4. **Creates a disk image**, partitions it, and mounts it.
5. **Downloads all sources** and checks their MD5 sums, falling back to the LFS
   mirror for any that fail.
6. **Builds LFS**: the cross toolchain and temporary tools as user `lfs`
   (chapters 5–6), then the chroot, the final system, configuration, kernel,
   and GRUB (chapters 7–10).
7. **Installs Windows program support**: `pe-loader`, `pe-inspect`, and a boot
   script that registers `.exe` files with the kernel (`binfmt_misc`), so
   `./program.exe` runs directly.

## Usage

Run as root on a Linux host. WSL2 with Debian or Ubuntu works.

```sh
git clone https://github.com/fguy06790-code/LFS.git
cd LFS
./build-lfs.sh --fix-host
```

`--fix-host` installs missing build tools with apt and points `/bin/sh` at bash,
as LFS requires. Leave it off if your host is already set up.

| Option | Effect |
|---|---|
| `--fix-host` | install missing host tools first (Debian/Ubuntu) |
| `--stop-after PHASE` | stop after a phase, e.g. `--stop-after sources` |
| `--config FILE` | use a settings file (default: `lfs.conf` next to the script) |
| `--with-node` | also build Node.js, so `pe-inspect` runs on the new system |
| `--keep-tests` | run the book's test suites (much slower) |

Settings such as the time zone, host name, image size, and network are in
[`lfs.conf.example`](lfs.conf.example). Copy it to `lfs.conf` to change them.

The build takes several hours. Each section's output goes to
`/root/lfs-work/logs/`. Set `VERBOSE=1` to watch it instead.

## If a step fails

The script prints the end of the failing section's log and stops. Fix the
cause and run it again: it skips everything that already finished and
continues from the failed section.

To redo a phase, delete its marker in `/root/lfs-work/state/`, for example
`phase-disk` to recreate the image.

## Booting the result

QEMU:

```sh
qemu-system-x86_64 -m 4G -smp 4 -enable-kvm \
  -drive file=/root/lfs-work/lfs.img,format=raw -nic user,model=e1000 -serial mon:stdio
```

Hyper-V: convert the image with `qemu-img convert -O vhdx lfs.img lfs.vhdx`
and attach it to a Generation 1 VM as an IDE disk.

Log in as root, then:

```sh
pe-loader program.exe    # or simply ./program.exe
```

## Files

| Path | Purpose |
|---|---|
| `build-lfs.sh` | the build script |
| `tools/extract-book.py` | pulls each section's commands out of the book |
| `tools/run-section.sh` | unpacks a package, runs one section, cleans up |
| `overrides/` | replacements for interactive or placeholder sections |
| `stages/extras.sh` | installs lfs-pe-inspect and release files |
| `lfs.conf.example` | settings |

## License

MIT. See [LICENSE](LICENSE). The LFS book is © the LFS project and is
downloaded at build time, not included here.

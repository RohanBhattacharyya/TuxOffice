# Packaging and releases

TuxOffice ships one portable runner inside native packages for each
distribution family, plus a tarball for everything else.

| File | For |
| --- | --- |
| `tuxoffice_VERSION-1_amd64.deb` | Debian 13+, Ubuntu 24.04+ and derivatives (`apt`) |
| `tuxoffice-VERSION-1.x86_64.rpm` | Fedora 40+, openSUSE Tumbleweed (`dnf`, `zypper`) |
| `tuxoffice-VERSION-1-x86_64.pkg.tar.zst` | Arch Linux and derivatives (`pacman`) |
| `tuxoffice-VERSION-x86_64.tar.xz` | per-user installs (`install.sh --user`) and the AUR |
| `aur/PKGBUILD`, `aur/.SRCINFO` | the `tuxoffice-bin` AUR package |
| `SHA256SUMS` | checksums the installer verifies |

## Layout of an installed package

```
/usr/bin/tuxoffice                -> ../share/tuxoffice/tuxoffice
/usr/share/tuxoffice/             launcher, VERSION, config/, lib/ helpers
/usr/lib/tuxoffice/runner/        the TuxOffice Wine runner, DXVK bundle and
                                  bin/tuxoffice-file-dialog
/usr/share/doc/tuxoffice/README.md
/usr/share/licenses/tuxoffice/LICENSE
```

The launcher finds the runner relative to itself, so the tarball works the
same way from `~/.local/share/tuxoffice/app/usr`. Each user's Office
environment stays in `~/.local/share/tuxoffice` and is never touched by
package upgrades or removal.

## Building a release

```sh
packaging/build-release.sh          # writes dist/
```

The script builds from the committed tree and needs only Docker:

1. `container-build.sh` runs in `ubuntu:24.04`: it installs Wine's build
   dependencies, downloads and verifies the pinned Soda release, and runs
   `runner/build.sh`. glibc 2.39 keeps the runner usable on every
   distribution the Soda base supports (glibc 2.38 and newer).
2. The rebuilt libraries are checked for the features a missing development
   package would silently drop (Wayland, xkbcommon, FreeType, Fontconfig,
   GnuTLS, libsecret), and the DXVK bundle and file chooser helper must be
   present.
3. The tree is staged and packaged by [nfpm](https://nfpm.goreleaser.com)
   from [`nfpm.yaml`](nfpm.yaml), which lists each family's dependencies;
   the AUR files are generated from [`arch/PKGBUILD.in`](arch/PKGBUILD.in).

Downloads and compiler work are cached in `~/.cache/tuxoffice-release`, so
later releases rebuild only what changed. `TUXOFFICE_REUSE_RUNNER=1`
repackages an existing `dist/runner` without rebuilding it.

## Publishing

Push a tag that matches [`VERSION`](../VERSION):

```sh
git tag v0.1.0 && git push origin v0.1.0
```

[`.github/workflows/release.yml`](../.github/workflows/release.yml) builds
the files and attaches them to a GitHub release; `install.sh` installs the
latest release from there. To update the AUR package, copy `dist/aur/PKGBUILD`
and `dist/aur/.SRCINFO` into the `tuxoffice-bin` AUR repository and push.

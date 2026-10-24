#!/bin/sh
# ============================================================================
# Reverie Engine — scripts/make-package.sh
#
# Builds a binary package for the CURRENT environment — run inside the
# matching distro container (CI's package job does exactly that). The
# package carries BOTH binaries (reverie + revtool) plus the docs: the
# "all in one package" release artifact.
#
# usage: make-package.sh <debian|fedora|opensuse|arch|alpine|void|tarball>
#                        [version=0.1.0] [release=1]
#
# Every distro branch SELF-TESTS: the freshly built package is installed
# back into the container and the binaries must answer --help. A broken
# package format fails the CI step that built it, not a user's machine.
# ============================================================================
set -eu

DISTRO="${1:?usage: make-package.sh <debian|fedora|opensuse|arch|alpine|void|tarball> [version] [release]}"
VER="${2:-0.1.0}"
REL="${3:-1}"
case "$VER" in *.*) ;; *) echo "bad version '$VER' (want MAJOR.MINOR.PATCH)" >&2; exit 2;; esac

MAINT="FemBoyGamerTechGuy <FemBoyGamerTechGuy@users.noreply.github.com>"
URL="https://github.com/FemBoyGamerTechGuy/Reverie-Engine"
DESC_SHORT="Reverie — live-wallpaper engine (Faded Dream Project)"
DESC_LONG="Reverie is an interactive wallpaper engine: animated .rev scenes,
video and static-image wallpapers, hand-written X11 and Wayland backends,
Vulkan/OpenGL/CPU renderer tiers, IPC runtime control, session restore
and autostart. Part of the Faded Dream Project."

# ---- the staging tree: /usr/bin + /usr/share/doc, identical everywhere -----
STG=$(mktemp -d)
trap 'rm -rf "$STG"' EXIT
mkdir -p "$STG/usr/bin" "$STG/usr/share/doc/reverie/docs"
install -m 755 build/reverie build/revtool "$STG/usr/bin/"
install -m 644 README.md CHANGELOG.md LICENSE "$STG/usr/share/doc/reverie/"
install -m 644 docs/*.md "$STG/usr/share/doc/reverie/docs/"

helptest() {
        # the installed engine must answer --help without a display
        # (both binaries exit 0 on --help — verified above)
        "$1" --help >/dev/null 2>&1 || {
                echo "FAIL: installed $1 did not answer --help" >&2; exit 1; }
}

case "$DISTRO" in

# ---- Debian family: .deb ----------------------------------------------------
debian)
        command -v dpkg-deb >/dev/null || { echo "dpkg-deb missing" >&2; exit 2; }
        mkdir "$STG/DEBIAN"
        {
                echo "Package: reverie"
                echo "Version: $VER"
                echo "Architecture: amd64"
                echo "Maintainer: $MAINT"
                echo "Section: x11"
                echo "Priority: optional"
                echo "Homepage: $URL"
                echo "Description: $DESC_SHORT"
                # fold the long description to 7-bit + leading space, deb-style
                echo "$DESC_LONG" | fold -s -w 76 | sed 's/^/ /'
        } > "$STG/DEBIAN/control"
        PKG="reverie_${VER}_amd64.deb"
        dpkg-deb --build --root-owner-group "$STG" "$PKG"
        dpkg -i "$PKG"
        helptest /usr/bin/reverie
        helptest /usr/bin/revtool
        dpkg -r reverie
        echo "built+verified: $PKG"
        ;;

# ---- Fedora / RHEL family: .rpm ---------------------------------------------
fedora|opensuse)
        command -v rpmbuild >/dev/null || { echo "rpmbuild missing" >&2; exit 2; }
        RPMDIR=$(mktemp -d)
        {
                echo "Name: reverie"
                echo "Version: $VER"
                echo "Release: ${REL}%{?dist}"
                echo "Summary: $DESC_SHORT"
                echo "URL: $URL"
                echo "License: Proprietary"
                echo "Packager: $MAINT"
                echo "ExclusiveArch: x86_64"
                echo "%description"
                echo "$DESC_LONG"
                echo "%files"
                echo "/usr/bin/reverie"
                echo "/usr/bin/revtool"
                echo "%dir /usr/share/doc/reverie"
                echo "%doc /usr/share/doc/reverie/README.md"
                echo "%doc /usr/share/doc/reverie/CHANGELOG.md"
                echo "%doc /usr/share/doc/reverie/LICENSE"
                echo "%doc /usr/share/doc/reverie/docs/*.md"
                echo "%install"
                echo "rm -rf %{buildroot}"
                echo "mkdir -p %{buildroot}/usr/bin"
                echo "install -m 755 $STG/usr/bin/reverie $STG/usr/bin/revtool %{buildroot}/usr/bin/"
                echo "mkdir -p %{buildroot}/usr/share/doc/reverie/docs"
                echo "install -m 644 $STG/usr/share/doc/reverie/*.md %{buildroot}/usr/share/doc/reverie/"
                echo "install -m 644 $STG/usr/share/doc/reverie/LICENSE %{buildroot}/usr/share/doc/reverie/"
                echo "install -m 644 $STG/usr/share/doc/reverie/docs/*.md %{buildroot}/usr/share/doc/reverie/docs/"
        } > "$RPMDIR/reverie.spec"
        rpmbuild -bb \
                --define "_topdir $RPMDIR" \
                --define "_rpmdir $RPMDIR/out" \
                --define "_buildrootdir $RPMDIR/buildroot" \
                "$RPMDIR/reverie.spec" >/dev/null
        OUT=$(find "$RPMDIR/out" -name 'reverie-*.rpm' | head -1)
        [ -n "$OUT" ] || { echo "FAIL: rpmbuild produced nothing" >&2; exit 1; }
        PKG="reverie-${VER}-${REL}.${DISTRO}.x86_64.rpm"
        cp "$OUT" "$PKG"
        rpm -Uvh "$PKG" >/dev/null
        helptest /usr/bin/reverie
        helptest /usr/bin/revtool
        rpm -e reverie >/dev/null
        echo "built+verified: $PKG"
        ;;

# ---- Arch: .pkg.tar.zst -----------------------------------------------------
arch)
        command -v zstd >/dev/null || { echo "zstd missing" >&2; exit 2; }
        SIZE=$(du -sk "$STG/usr" | cut -f1)
        OUTDIR=$(pwd)
        {
                echo "pkgname = reverie"
                echo "pkgver = ${VER}-${REL}"
                echo "pkgdesc = $DESC_SHORT"
                echo "url = $URL"
                echo "builddate = $(date -u +%s)"
                echo "packager = $MAINT"
                echo "size = ${SIZE}"
                echo "arch = x86_64"
                echo "license = (custom: Proprietary)"
        } > "$STG/.PKGINFO"
        PKG="reverie-${VER}-${REL}-x86_64.pkg.tar.zst"
        ( cd "$STG" && tar -I zstd -cf "$OUTDIR/$PKG" .PKGINFO usr/ )
        pacman -U --noconfirm "$PKG" >/dev/null 2>&1
        helptest /usr/bin/reverie
        helptest /usr/bin/revtool
        pacman -R --noconfirm reverie >/dev/null 2>&1
        echo "built+verified: $PKG"
        ;;

# ---- Alpine: .apk (v2 format, two concatenated tar.gz sections) -------------
alpine)
        command -v tar >/dev/null || { echo "tar missing" >&2; exit 2; }
        WORK=$(mktemp -d)
        # data section first: its hash goes into .PKGINFO
        ( cd "$STG" && tar -czf "$WORK/data.tar.gz" usr/ )
        DATAHASH=$(sha256sum "$WORK/data.tar.gz" | cut -d' ' -f1)
        DATASIZE=$(wc -c < "$WORK/data.tar.gz" | tr -d ' ')
        {
                echo "pkgname = reverie"
                echo "pkgver = ${VER}-r${REL}"
                echo "arch = x86_64"
                echo "pkgdesc = $DESC_SHORT"
                echo "url = $URL"
                echo "size = ${DATASIZE}"
                echo "builddate = $(date -u +%s)"
                echo "license = Proprietary"
                echo "datahash = ${DATAHASH}"
        } > "$STG/.PKGINFO"
        ( cd "$STG" && tar -czf "$WORK/control.tar.gz" .PKGINFO )
        PKG="reverie-${VER}-r${REL}.apk"
        cat "$WORK/control.tar.gz" "$WORK/data.tar.gz" > "$PKG"
        rm -rf "$WORK"
        apk add --allow-untrusted "./$PKG" >/dev/null
        helptest /usr/bin/reverie
        helptest /usr/bin/revtool
        apk del reverie >/dev/null
        echo "built+verified: $PKG"
        ;;

# ---- Void: .xbps -------------------------------------------------------------
void)
        command -v xbps-create >/dev/null || { echo "xbps-create missing (xbps package)" >&2; exit 2; }
        PKG="reverie-${VER}_${REL}.x86_64.xbps"
        # well-known xbps-create flags only: -n pkgver, -A arch, -s synopsis,
        # -D description, -m maintainer, -l license
        xbps-create -A x86_64 \
                -n "reverie-${VER}_${REL}" \
                -s "$DESC_SHORT" \
                -D "$DESC_LONG" \
                -m "$MAINT" \
                -l "Proprietary" \
                "$STG" >/dev/null
        [ -f "$PKG" ] || { echo "FAIL: xbps-create produced nothing" >&2; exit 1; }
        xbps-rindex -a "$PKG" >/dev/null 2>&1 || true
        xbps-install -y -R "$(pwd)" reverie >/dev/null 2>&1 || \
                { echo "FAIL: xbps self-install of the package failed" >&2; exit 1; }
        helptest /usr/bin/reverie
        helptest /usr/bin/revtool
        xbps-remove -y reverie >/dev/null 2>&1 || true
        echo "built+verified: $PKG"
        ;;

# ---- portable bundle: .tar.gz (no package manager) ---------------------------
tarball)
        OUTDIR=$(pwd)
        PKG="reverie-${VER}-linux-x86_64.tar.gz"
        BND=$(mktemp -d)
        mkdir -p "$BND/reverie-${VER}/docs"
        install -m 755 build/reverie build/revtool "$BND/reverie-${VER}/"
        install -m 644 README.md CHANGELOG.md LICENSE "$BND/reverie-${VER}/"
        install -m 644 docs/*.md "$BND/reverie-${VER}/docs/"
        ( cd "$BND" && tar -czf "$OUTDIR/$PKG" "reverie-${VER}" )
        rm -rf "$BND"
        # self-test: extract elsewhere, run --help from the extracted tree
        T=$(mktemp -d)
        tar -xzf "$PKG" -C "$T"
        helptest "$T/reverie-${VER}/reverie"
        helptest "$T/reverie-${VER}/revtool"
        rm -rf "$T"
        echo "built+verified: $PKG"
        ;;

*)
        echo "unknown distro '$DISTRO'" >&2
        exit 2
        ;;
esac

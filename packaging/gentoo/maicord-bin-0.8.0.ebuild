# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

# maicord-bin: the release tarball (packaging/build_tarball.py) of MaiCord, for the GURU overlay (net-im/maicord-bin) or a
# personal overlay. The program is built upstream with LLVM 24 under the project's freestanding contract, so the
# release's program is installed as it is. The tarball's .maicord-portable marker is not installed: portage owns these files,
# so the program does not update itself. Per release: copy this file to maicord-bin-<new version>.ebuild and `pkgdev manifest`.

EAPI=8

inherit optfeature xdg

DESCRIPTION="Lightweight, native Discord client (unofficial), from the release tarball"
HOMEPAGE="https://maicord.com/ https://github.com/maj113/MaiCord"
SRC_URI="https://github.com/maj113/MaiCord/releases/download/v${PV}/maicord-${PV}-linux-x86_64.tar.gz"
S="${WORKDIR}/maicord-${PV}-linux-x86_64"

LICENSE="MPL-2.0 OFL-1.1 BSD MIT ISC CC-BY-4.0 Unicode-3.0"
SLOT="0"
KEYWORDS="-* ~amd64"
# A freestanding, stripped, size-optimized image: portage must leave its bytes as released.
RESTRICT="mirror strip"
QA_PREBUILT="usr/lib/maicord/MaiCord"

# What the program cannot start without: OpenSSL 3 or 4 (it opens libssl.so.4, else libssl.so.3; the subslot is the soname), the
# Wayland client libraries, xkbcommon, EGL and GLESv2. Everything else it opens with dlopen when it is there (see the optional
# dependencies).
RDEPEND="
	|| (
		dev-libs/openssl:0/4
		dev-libs/openssl:0/3
	)
	dev-libs/wayland
	media-libs/libglvnd
	media-libs/mesa
	x11-libs/libxkbcommon
	sys-libs/glibc
"

pkg_postinst() {
	xdg_pkg_postinst
	optfeature "voice and notification audio" media-libs/libpulse
	optfeature "Go Live screen sharing and stream audio" media-video/pipewire
	optfeature "system fonts and font fallback" media-libs/fontconfig
	optfeature "the file chooser, settings and global shortcut portals" sys-apps/xdg-desktop-portal
	optfeature "colour emoji" media-fonts/noto-emoji
	optfeature "security keys for two-factor sign-in" dev-libs/libfido2
	optfeature "text-to-speech" app-accessibility/speech-dispatcher
	optfeature "spell checking" app-text/hunspell
}

src_install() {
	exeinto /usr/lib/maicord
	doexe MaiCord
	dosym ../lib/maicord/MaiCord /usr/bin/maicord

	insinto /usr/lib/maicord/fonts
	doins fonts/*
	insinto /usr/lib/maicord/licenses
	doins licenses/*

	insinto /usr/share
	doins -r share/applications share/icons share/metainfo
}

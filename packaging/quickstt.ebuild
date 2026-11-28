# Copyright 2024-2026 QuickSTT Team
# Distributed under the terms of the MIT License.
#
# Binary repackage of the official universal tarball.
# Install: copy this file as quickstt-1.0.0.ebuild into a local overlay at
# app-accessibility/quickstt/, run `ebuild quickstt-1.0.0.ebuild manifest`,
# then `emerge -av quickstt`.

EAPI=8

inherit desktop xdg

DESCRIPTION="Floating offline voice typing pill (X11 & Wayland/Hyprland)"
HOMEPAGE="https://github.com/xyzyt010/quickstt"
SRC_URI="
	amd64? ( https://github.com/xyzyt010/quickstt/releases/download/desktop-v${PV}/quickstt-desktop-v${PV}-x86_64.tar.gz -> quickstt-${PV}-x86_64.tar.gz )
	arm64? ( https://github.com/xyzyt010/quickstt/releases/download/desktop-v${PV}/quickstt-desktop-v${PV}-aarch64.tar.gz -> quickstt-${PV}-aarch64.tar.gz )
"
S="${WORKDIR}"

LICENSE="MIT"
SLOT="0"
KEYWORDS="~amd64 ~arm64"
RESTRICT="strip"

RDEPEND="
	x11-libs/gtk+:3
	media-libs/alsa-lib
	media-libs/libpulse
	media-libs/portaudio
	sci-libs/openblas
	x11-libs/libX11
	x11-libs/libXi
	x11-libs/libXtst
	x11-misc/xdotool
	x11-misc/xclip
"
DEPEND="${RDEPEND}"

QA_PREBUILT="opt/quickstt/*"

src_install() {
	insinto /opt/quickstt
	doins -r "${S}"/.
	fperms 0755 /opt/quickstt/QuickSTT
	# Bundled STT engine binaries, where present in the tarball.
	local tool
	for tool in tools/whisper_cpp/whisper-cli tools/nemotron/transcribe-cli \
		tools/parakeet/parakeet_engine tools/lfm3_audio/llama-cli; do
		[ -f "${ED}/opt/quickstt/${tool}" ] && fperms 0755 "/opt/quickstt/${tool}"
	done

	dosym /opt/quickstt/QuickSTT /usr/bin/quickstt

	if [ -f "${ED}/opt/quickstt/assets/quickstt.desktop" ]; then
		domenu "${ED}/opt/quickstt/assets/quickstt.desktop"
	fi
	if [ -f "${ED}/opt/quickstt/icon_app.png" ]; then
		doicon -s 256 "${ED}/opt/quickstt/icon_app.png"
	fi
}

pkg_postinst() {
	xdg_pkg_postinst
	elog "QuickSTT installed. Run: quickstt"
	elog "Wayland typing: emerge gui-apps/wtype (and/or sys-apps/ydotool)."
	elog "Models download on demand from the in-app Speech Models tab."
}

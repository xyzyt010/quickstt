# QuickSTT - binary repackage of the official universal tarball.
# Build on Fedora/RHEL-family:
#   cp quickstt-desktop-v1.0.0-*.tar.gz ~/rpmbuild/SOURCES/
#   rpmbuild -bb quickstt.spec
%global debug_package %{nil}
%global __strip /bin/true

Name:           quickstt
Version:        1.0.0
Release:        1%{?dist}
Summary:        Floating offline voice typing pill (X11 & Wayland/Hyprland)
License:        MIT
URL:            https://github.com/xyzyt010/quickstt
# Tarball suffix matches the target arch: x86_64 or aarch64.
Source0:        https://github.com/xyzyt010/quickstt/releases/download/desktop-v%{version}/quickstt-desktop-v%{version}-%{_arch}.tar.gz

ExclusiveArch:  x86_64 aarch64
Requires:       gtk3, alsa-lib, pulseaudio-libs, portaudio, libX11, libXi, libXtst, xdotool, xclip, openblas
Recommends:     wtype, wl-clipboard

%description
Floating offline voice typing widget for Linux - X11 & Wayland/Hyprland,
transparent always-on-top. Includes integrated STT backends: Vosk small EN,
Parakeet TDT 0.6B, Nemotron 3.5 streaming. Models download on demand;
nothing leaves the machine.

%prep
%setup -c -n %{name}-%{version}

%build
# Prebuilt binary payload - nothing to compile.

%install
mkdir -p %{buildroot}/opt/quickstt
cp -a . %{buildroot}/opt/quickstt/
chmod 755 %{buildroot}/opt/quickstt/QuickSTT
find %{buildroot}/opt/quickstt/tools -type f \( -name 'whisper-cli' -o -name 'transcribe-cli' -o -name 'parakeet_engine' -o -name 'llama-cli' \) -exec chmod 755 {} + 2>/dev/null || true

mkdir -p %{buildroot}%{_bindir}
ln -s /opt/quickstt/QuickSTT %{buildroot}%{_bindir}/quickstt

if [ -f %{buildroot}/opt/quickstt/assets/quickstt.desktop ]; then
    mkdir -p %{buildroot}%{_datadir}/applications
    install -m 644 %{buildroot}/opt/quickstt/assets/quickstt.desktop %{buildroot}%{_datadir}/applications/quickstt.desktop
    sed -i 's|^Exec=.*|Exec=/usr/bin/quickstt|' %{buildroot}%{_datadir}/applications/quickstt.desktop
fi
if [ -f %{buildroot}/opt/quickstt/icon_app.png ]; then
    mkdir -p %{buildroot}%{_datadir}/pixmaps
    install -m 644 %{buildroot}/opt/quickstt/icon_app.png %{buildroot}%{_datadir}/pixmaps/quickstt.png
fi

%files
/opt/quickstt
%{_bindir}/quickstt
%{_datadir}/applications/quickstt.desktop
%{_datadir}/pixmaps/quickstt.png

%changelog
* Sat Nov 28 2026 QuickSTT Team <support@quickstt.com> - 1.0.0-1
- Initial RPM recipe for the v1.0.0 universal tarball.

# MaiCord from the release tarball (packaging/build_tarball.py), for Fedora COPR and the openSUSE Build Service (one spec for
# both, and for other rpm distributions the Build Service targets). The program is built upstream with LLVM 23 under the
# project's freestanding contract, which a distribution builder does not offer, so this package installs the release's program
# as it is and compiles nothing. The tarball's .maicord-portable marker is left out: the package manager owns the files, so
# the program does not update itself.
#
# Per release (packaging/README.md): set Version, add a %changelog entry, and for the Build Service the version in _service.
%global application_id io.github.maj113.MaiCord
# The image is already stripped and size-optimized, and has no build id the debug tools could use.
%global debug_package %{nil}
%global __strip /bin/true
%global _build_id_links none
%global __brp_strip %{nil}
%global __brp_strip_static_archive %{nil}
%global __brp_strip_comment_note %{nil}
%global __brp_check_rpaths %{nil}
%global __requires_exclude ^lib(c|dl|pthread)\\.so.*$

Name:           maicord
Version:        0.6.6
Release:        1%{?dist}
Summary:        A lightweight, native Discord client (unofficial)
License:        MPL-2.0 AND OFL-1.1 AND BSD-3-Clause AND MIT AND ISC AND CC-BY-4.0 AND Unicode-3.0
URL:            https://maicord.com/
Source0:        https://github.com/maj113/MaiCord/releases/download/v%{version}/maicord-%{version}-linux-x86_64.tar.gz
ExclusiveArch:  x86_64
BuildRequires:  desktop-file-utils
# What the program cannot start without, by the sonames it opens (the names of the packages differ between distributions,
# the capabilities do not): OpenSSL for TLS, the Wayland client libraries, xkbcommon, and the EGL and GLESv2 loaders.
Requires:       libssl.so.3()(64bit)
Requires:       libwayland-client.so.0()(64bit)
Requires:       libwayland-egl.so.1()(64bit)
Requires:       libwayland-cursor.so.0()(64bit)
Requires:       libxkbcommon.so.0()(64bit)
Requires:       libEGL.so.1()(64bit)
Requires:       libGLESv2.so.2()(64bit)
# Everything else is opened with dlopen when it is there.
Recommends:     xdg-desktop-portal
Recommends:     gnome-keyring
Suggests:       speech-dispatcher
%if 0%{?suse_version}
BuildRequires:  update-desktop-files
Recommends:     Mesa-dri
Recommends:     libpulse0
Recommends:     libfontconfig1
Recommends:     google-noto-coloremoji-fonts
Recommends:     libhunspell-1_7-0
Suggests:       google-noto-sans-cjk-fonts
Suggests:       myspell-en_US
Suggests:       libfido2-1
%else
Recommends:     mesa-dri-drivers
Recommends:     pulseaudio-libs
Recommends:     fontconfig
Recommends:     google-noto-color-emoji-fonts
Recommends:     hunspell
Suggests:       google-noto-sans-cjk-fonts
Suggests:       hunspell-en-US
Suggests:       enchant2
Suggests:       libfido2
%endif

%description
MaiCord draws its own interface on the GPU; there is no Electron, browser engine
or web view. It is not affiliated with, endorsed by or supported by Discord Inc.

This package installs the upstream release program, which is built from the
source at https://github.com/maj113/MaiCord with LLVM 23.

%prep
%setup -q -n maicord-%{version}-linux-x86_64

%build
# Nothing to compile: the release program is installed as it is.

%install
install -Dm755 MaiCord %{buildroot}%{_prefix}/lib/maicord/MaiCord
install -d %{buildroot}%{_bindir}
ln -s %{_prefix}/lib/maicord/MaiCord %{buildroot}%{_bindir}/maicord
install -d %{buildroot}%{_prefix}/lib/maicord/fonts %{buildroot}%{_prefix}/lib/maicord/licenses
install -m644 fonts/* %{buildroot}%{_prefix}/lib/maicord/fonts/
install -m644 licenses/* %{buildroot}%{_prefix}/lib/maicord/licenses/
install -d %{buildroot}%{_datadir}
cp -r share/. %{buildroot}%{_datadir}/
find %{buildroot}%{_datadir} -type f -exec chmod 0644 {} +

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{application_id}.desktop

%if 0%{?suse_version}
%post
%desktop_database_post
%icon_theme_cache_post

%postun
%desktop_database_postun
%icon_theme_cache_postun
%endif

%files
%license LICENSE
%{_prefix}/lib/maicord/
%{_bindir}/maicord
%{_datadir}/applications/%{application_id}.desktop
%{_datadir}/icons/hicolor/*/apps/%{application_id}.png
%{_datadir}/metainfo/%{application_id}.metainfo.xml

%changelog
* Thu Nov 05 2026 maj113 <https://github.com/maj113/MaiCord/issues> - 0.6.6-1
- MaiCord 0.6.6 from the release tarball

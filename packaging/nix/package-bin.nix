# MaiCord from the release tarball (packaging/build_tarball.py, attached to every GitHub release): the same program the
# source package builds, patched for Nix. The image is a freestanding glibc PIE with libc as its only
# DT_NEEDED entry and every other library opened with dlopen: autoPatchelfHook gives it the Nix loader and glibc,
# runtimeDependencies puts the dlopen()ed libraries on its run path, and appendRunpaths adds /run/opengl-driver/lib for
# the GPU drivers. The tarball's .maicord-portable marker is not installed: the Nix store is read only, so the program does
# not update itself.
{
  lib,
  stdenvNoCC,
  fetchurl,
  autoPatchelfHook,
  addDriverRunpath,
  openssl,
  wayland,
  libxkbcommon,
  libglvnd,
  fontconfig,
  libpulseaudio,
  hunspell,
  enchant,
  stdenv,
  version ? "0.6.6",
  # Not called src: callPackage would fill that from the (renamed) top-level attribute of nixpkgs.
  tarball ? fetchurl {
    url = "https://github.com/maj113/MaiCord/releases/download/v${version}/maicord-${version}-linux-x86_64.tar.gz";
    # TODO(owner): the release's .sha256 asset, per version (nix-prefetch-url or the .sha256 file; hex is accepted).
    sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
  },
}:

stdenvNoCC.mkDerivation {
  pname = "maicord-bin";
  inherit version;
  src = tarball;

  nativeBuildInputs = [ autoPatchelfHook ];
  buildInputs = [ stdenv.cc.libc ];
  # The library output of each (openssl's default output is its command line tool, not libssl).
  runtimeDependencies = map lib.getLib [
    openssl
    wayland
    libxkbcommon
    libglvnd
    fontconfig
    libpulseaudio
    hunspell
    enchant
  ];
  # The GPU drivers of NixOS (and of non-NixOS systems with nixGL-style setups) live under /run/opengl-driver.
  appendRunpaths = [ "${addDriverRunpath.driverLink}/lib" ];

  dontConfigure = true;
  dontBuild = true;
  dontStrip = true;
  sourceRoot = "maicord-${version}-linux-x86_64";

  installPhase = ''
    runHook preInstall
    install -Dm755 MaiCord $out/lib/maicord/MaiCord
    install -d $out/bin
    ln -s $out/lib/maicord/MaiCord $out/bin/maicord
    cp -r fonts licenses $out/lib/maicord/
    mkdir -p $out/share
    cp -r share/. $out/share/
    runHook postInstall
  '';

  meta = {
    description = "A lightweight, native Discord client (unofficial), from the release tarball";
    homepage = "https://maicord.com/";
    license = lib.licenses.mpl20;
    sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
    mainProgram = "maicord";
    platforms = [ "x86_64-linux" ];
  };
}

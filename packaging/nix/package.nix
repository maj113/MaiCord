# MaiCord built from source, the recipe of nixpkgs' pkgs/by-name/ma/maicord/package.nix. The nixpkgs copy is this file as
# it is at the tag it builds, without this paragraph and with the three places marked "nixpkgs:" changed
# (packaging/README.md, "Nix (nix/) and nixpkgs").
#
# The decisions are explained beside the lines they concern: a stock LLVM 23 for the project's contract, the run-time
# libraries on the program's run path, and the tarball's layout without the self-updater's marker. The shaders need no
# tools beyond python.
{
  lib,
  callPackage,
  stdenv,
  # nixpkgs: fetchFromGitHub instead of these two; the flake passes its checkout and the project version.
  src,
  version,
  cmake,
  ninja,
  python3,
  llvmPackages_23,
  autoPatchelfHook,
  addDriverRunpath,
  versionCheckHook,
  nix-update-script,
  # Opened with dlopen at run time, by soname: see "runtimeDependencies" below.
  openssl,
  wayland,
  libxkbcommon,
  libglvnd,
  fontconfig,
  libpulseaudio,
  pipewire,
  libva,
  dav1d,
  openh264,
  libfido2,
  speechd-minimal,
  hunspell,
}:

let
  # The freestanding contract (-nostdinc, its own headers, its own _start, no libc beyond a generated stub) builds with a
  # stock clang and lld 23; upstream builds with LLVM 24 of maj113/llvm-pe, whose extra code generation options the contract
  # adds only when the compiler takes them, so this build is the same program without them, somewhat bigger.
  llvm = llvmPackages_23;
in
stdenv.mkDerivation (finalAttrs: {
  pname = "maicord";
  # nixpkgs: version = "X.Y.Z"; and src = fetchFromGitHub { owner = "maj113"; repo = "MaiCord"; tag = "v${finalAttrs.version}";
  # hash = "sha256-..."; } (no submodules, no Git LFS, no export-ignore: the tag's archive is the whole tree).
  inherit src version;

  strictDeps = true;
  __structuredAttrs = true;

  nativeBuildInputs = [
    cmake
    ninja
    python3
    # The unwrapped compilers: the build is freestanding and the nixpkgs wrappers (libc paths, hardening flags and
    # a program interpreter of their own at link time) would only fight its contract.
    llvm.clang-unwrapped
    llvm.lld
    llvm.llvm
    autoPatchelfHook
  ];

  # The program's only DT_NEEDED entry is libc.so.6 (a generated stub), which autoPatchelfHook resolves to the glibc of
  # the stdenv. Every other library is opened with dlopen by soname, and a missing one only turns a feature off, so the
  # loader never reports it: runtimeDependencies puts them on the run path of the program, and the GPU drivers (Mesa,
  # NVIDIA, the VA-API drivers) of NixOS live under /run/opengl-driver.
  runtimeDependencies = map lib.getLib [
    openssl
    wayland
    libxkbcommon
    libglvnd
    fontconfig
    libpulseaudio
    pipewire
    libva
    dav1d
    openh264
    libfido2
    speechd-minimal
    hunspell
  ];
  appendRunpaths = [ "${addDriverRunpath.driverLink}/lib" ];

  cmakeFlags = [
    (lib.cmakeFeature "CMAKE_C_COMPILER" "${llvm.clang-unwrapped}/bin/clang")
    (lib.cmakeFeature "CMAKE_CXX_COMPILER" "${llvm.clang-unwrapped}/bin/clang++")
    (lib.cmakeFeature "CMAKE_AR" "${llvm.llvm}/bin/llvm-ar")
    (lib.cmakeFeature "CMAKE_RANLIB" "${llvm.llvm}/bin/llvm-ranlib")
    # The unwrapped clang cannot link a hosted program: the compiler check only needs to compile.
    (lib.cmakeFeature "CMAKE_TRY_COMPILE_TARGET_TYPE" "STATIC_LIBRARY")
  ];

  env = {
    # Numbers the build (there is no git checkout to take it from). MAICORD_LINUX_RELEASES is not set, so this build
    # never carries the self-updater: it is for the tarball install, whose .maicord-portable marker the store has not.
    MAICORD_UPDATE_VERSION = finalAttrs.version;
  };

  preConfigure = ''
    # The generated sources must not land inside the source tree, so the build directory is beside it.
    cmakeDir=$PWD
    cmakeBuildDir=$NIX_BUILD_TOP/build
  '';

  # The project's audit of the image (no GLIBC_ symbol above 2.34, libc.so.6 the only DT_NEEDED entry, no run path, no
  # test hooks): it forbids what autoPatchelfHook adds below, so it runs before the fixup. The script is the one in the
  # source being built (tools/audit_elf.py up to 0.6.6).
  postBuild = ''
    python3 -B $cmakeDir/tools/audit_image.py --no-test-hooks MaiCord
  '';

  installPhase = ''
    runHook preInstall

    # The program finds fonts/ and licenses/ next to its real executable (/proc/self/exe), so the store keeps them
    # together, and bin/maicord is a link to it.
    install -Dm755 MaiCord $out/lib/maicord/MaiCord
    cp -r fonts licenses $out/lib/maicord/
    mkdir -p $out/bin
    ln -s $out/lib/maicord/MaiCord $out/bin/maicord

    # The desktop entry (discord:// and maicord:// links), the icons and the AppStream metadata.
    mkdir -p $out/share/metainfo
    cp -r packaging/applications packaging/icons $out/share/
    substitute $cmakeDir/packaging/metainfo/io.github.maj113.MaiCord.metainfo.xml.in \
      $out/share/metainfo/io.github.maj113.MaiCord.metainfo.xml --replace-fail '@RELEASES@' ""

    runHook postInstall
  '';

  doInstallCheck = true;
  nativeInstallCheckInputs = [ versionCheckHook ];

  passthru = {
    tests.gpu = callPackage ./gpu-tests.nix { maicord = finalAttrs.finalPackage; };
    updateScript = nix-update-script { };
  };

  meta = {
    description = "Unofficial native Discord client written in C++";
    longDescription = ''
      MaiCord is a Discord client written from scratch in C++: it shapes its own text and draws its own interface on
      the GPU, with no Electron, browser engine or web view. It runs on Wayland.

      It is not affiliated with, endorsed by or supported by Discord Inc., and using a third-party client may violate
      Discord's Terms of Service.
    '';
    homepage = "https://maicord.com/";
    changelog = "https://github.com/maj113/MaiCord/releases/tag/v${finalAttrs.version}";
    license = with lib.licenses; [
      mpl20
      # The bundled fonts, the Twemoji flags, Opus's notice, the emoji tables, Lucide icons and Unicode data.
      ofl
      cc-by-40
      cc0
      bsd3
      mit
      isc
      unicode-30
    ];
    mainProgram = "maicord";
    # nixpkgs: maintainers = with lib.maintainers; [ maj113 ]; (the handle is added to maintainers/maintainer-list.nix first).
    maintainers = [ ];
    platforms = [ "x86_64-linux" ];
  };
})

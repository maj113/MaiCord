# MaiCord built from source: the recipe of nixpkgs' pkgs/by-name/ma/maicord/package.nix, which differs from this file only in
# where the source and version come from (src and version are arguments here, fetchFromGitHub of the tag there) and in the
# maintainers. Keep the two in step (packaging/README.md, "Nix (nix/) and nixpkgs"). The decisions are explained beside the
# lines they concern: LLVM 23 as the project's contract wants it, nixpkgs' DXC and SPIRV-Cross for the shaders (no downloads),
# the run-time libraries on the program's run path, and the tarball's layout without the self-updater's marker.
{
  lib,
  callPackage,
  stdenv,
  # The checkout the flake is in and its version; nixpkgs fetches the tag instead.
  src,
  version,
  cmake,
  ninja,
  python3,
  llvmPackages_23,
  autoPatchelfHook,
  addDriverRunpath,
  directx-shader-compiler,
  spirv-cross,
  versionCheckHook,
  nix-update-script,
  # Opened with dlopen at run time, by soname: see "runtimeDependencies" below.
  openssl,
  wayland,
  libxkbcommon,
  libglvnd,
  fontconfig,
  libpulseaudio,
  libva,
  dav1d,
  openh264,
  libfido2,
  speechd-minimal,
  hunspell,
}:

let
  # The client is written for exactly this LLVM major: its freestanding contract (-nostdinc, its own headers, its own
  # _start, no libc beyond a generated stub) is checked by upstream with clang and lld 23 only.
  llvm = llvmPackages_23;
in
stdenv.mkDerivation (finalAttrs: {
  pname = "maicord";
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
    # The shaders are HLSL, translated at build time with DXC and SPIRV-Cross from nixpkgs instead of the pinned
    # downloads of tools/linux_shader_toolchain.json (mc_shadertool is compiled against this SPIRV-Cross).
    (lib.cmakeFeature "MAICORD_DXC" (lib.getExe' directx-shader-compiler "dxc"))
    (lib.cmakeFeature "MAICORD_SPIRV_CROSS_PREFIX" "${spirv-cross}")
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
    # The shader tool is compiled here: its cache must be writable (the default is under $HOME).
    export MAICORD_TOOLCHAIN_CACHE=$NIX_BUILD_TOP/maicord-toolchain
  '';

  # Upstream's audit of the image (no GLIBC_ symbol above 2.34, libc.so.6 the only DT_NEEDED entry, no run path, no test
  # hooks): it forbids what autoPatchelfHook adds below, so it runs before the fixup.
  postBuild = ''
    python3 -B $cmakeDir/tools/audit_elf.py --no-test-hooks MaiCord
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
    # nixpkgs: maintainers = with lib.maintainers; [ maj113 ]; the flake's nixpkgs-unstable has no such handle until that lands.
    maintainers = [ ];
    platforms = [ "x86_64-linux" ];
  };
})

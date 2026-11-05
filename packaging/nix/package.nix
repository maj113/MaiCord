# MaiCord built from source with LLVM 23 (the toolchain the Linux port is written for: tools/build_linux.sh refuses any
# other major), the way tools/build_linux.sh builds it, but offline: the shader toolchain that script fetches (DXC, the
# pinned SPIRV-Cross) is made by the derivations below from the pins of tools/linux_shader_toolchain.json and handed to
# tools/fetch_linux_shader_toolchain.py as its cache, which finds it ready and fetches nothing.
#
# The image is a freestanding glibc PIE: its only DT_NEEDED entry is libc (a generated stub),
# and everything else is opened with dlopen. autoPatchelfHook gives it the Nix loader and glibc, runtimeDependencies puts
# the dlopen()ed libraries on its run path, and appendRunpaths adds /run/opengl-driver/lib for the GPU drivers.
# nixpkgs' compiler wrappers and hardening flags are not used: the build runs the unwrapped clang, and the contract
# (-nostdinc, its own headers, no stack protector) is the project's.
#
# buildTarget is the CMake target whose image becomes bin/maicord. While the client target is not in the tree
# (the Linux port's tests are what builds), `nix build .#build-check` builds a test image in its place to prove the
# toolchain, the shader pack and the patching; the build then needs the tests configured (buildTests).
{
  lib,
  stdenv,
  runCommand,
  fetchurl,
  fetchFromGitHub,
  cmake,
  ninja,
  python3,
  llvmPackages_23,
  autoPatchelfHook,
  addDriverRunpath,
  zlib,
  openssl,
  wayland,
  libxkbcommon,
  libglvnd,
  fontconfig,
  libpulseaudio,
  hunspell,
  enchant,
  src,
  version,
  buildTarget ? "MaiCord",
  buildTests ? false,
}:

let
  pins = builtins.fromJSON (builtins.readFile ../../tools/linux_shader_toolchain.json);

  dxcTarball = fetchurl {
    url = pins.dxc.url;
    sha256 = pins.dxc.sha256;
  };

  spirvCrossSource = fetchFromGitHub {
    owner = "KhronosGroup";
    repo = "SPIRV-Cross";
    rev = pins.spirv_cross.commit;
    hash = "sha256-9ET3qtdCeCUC069lAtEhDCUi85qedllOncwn7Hx6zJk=";
  };

  # SPIRV-Cross as static libraries with the options tools/fetch_linux_shader_toolchain.py configures it with.
  spirvCross = stdenv.mkDerivation {
    pname = "spirv-cross-maicord";
    version = pins.spirv_cross.tag;
    src = spirvCrossSource;
    nativeBuildInputs = [
      cmake
      ninja
    ];
    hardeningDisable = [ "all" ];
    # Only the static C++ libraries are used; the C API's pkg-config file is not even enabled and nixpkgs rejects its paths.
    postInstall = ''
      rm -rf $out/lib/pkgconfig
    '';
    cmakeFlags = [
      "-DSPIRV_CROSS_ENABLE_TESTS=OFF"
      "-DSPIRV_CROSS_CLI=OFF"
      "-DSPIRV_CROSS_STATIC=ON"
      "-DSPIRV_CROSS_SHARED=OFF"
      "-DSPIRV_CROSS_ENABLE_GLSL=ON"
      "-DSPIRV_CROSS_ENABLE_HLSL=OFF"
      "-DSPIRV_CROSS_ENABLE_MSL=OFF"
      "-DSPIRV_CROSS_ENABLE_CPP=OFF"
      "-DSPIRV_CROSS_ENABLE_REFLECT=OFF"
      "-DSPIRV_CROSS_ENABLE_C_API=OFF"
      "-DSPIRV_CROSS_ENABLE_UTIL=OFF"
      "-DSPIRV_CROSS_SKIP_INSTALL=OFF"
    ];
  };

  # The cache directory the fetch script keeps: directories named by their pin, each with a .ready marker written last.
  shaderToolchain =
    runCommand "maicord-shader-toolchain"
      {
        nativeBuildInputs = [
          autoPatchelfHook
          python3
          stdenv.cc
        ];
        buildInputs = [
          stdenv.cc.cc.lib
          zlib
        ];
      }
      ''
        dxc=$out/dxc-${builtins.substring 0 12 pins.dxc.sha256}
        mkdir -p $dxc
        tar -xzf ${dxcTarball} -C $dxc
        chmod +x $dxc/${pins.dxc.executable}
        # runCommand has no fixup phase, so the hook's function is called here: DXC gets the Nix loader and libstdc++.
        autoPatchelf $dxc
        echo -n ${pins.dxc.version} > $dxc/.ready

        cross=$out/spirv-cross-${builtins.substring 0 12 pins.spirv_cross.commit}
        mkdir -p $cross
        ln -s ${spirvCross} $cross/install
        echo -n ${pins.spirv_cross.commit} > $cross/.ready

        # The host tool that links SPIRV-Cross, made by the project's own script (it finds DXC and SPIRV-Cross ready and
        # compiles only mc_shadertool). The compiler is part of the tool's cache key, so the build below names the same one.
        mkdir -p $TMPDIR/project
        cp -r ${src}/tools $TMPDIR/project/tools
        MAICORD_TOOLCHAIN_CACHE=$out CXX=${stdenv.cc}/bin/c++ python3 -B $TMPDIR/project/tools/fetch_linux_shader_toolchain.py
      '';

  llvm = llvmPackages_23;
in
stdenv.mkDerivation {
  pname = "maicord";
  inherit version src;

  nativeBuildInputs = [
    cmake
    ninja
    python3
    llvm.clang-unwrapped
    llvm.lld
    llvm.llvm
    autoPatchelfHook
  ];
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
  # The GPU drivers of NixOS live under /run/opengl-driver.
  appendRunpaths = [ "${addDriverRunpath.driverLink}/lib" ];

  hardeningDisable = [ "all" ];
  dontUseCmakeConfigure = true;
  dontStrip = true;

  configurePhase = ''
    runHook preConfigure
    # A writable copy: the fetch script takes a lock file and builds its host tool in the cache.
    cp -r ${shaderToolchain} $TMPDIR/toolchain
    chmod -R u+w $TMPDIR/toolchain
    export MAICORD_TOOLCHAIN_CACHE=$TMPDIR/toolchain
    # The generated sources must be outside the source tree: the build directory is beside it.
    cmake -S . -B $TMPDIR/build -G Ninja \
      -DCMAKE_C_COMPILER=${llvm.clang-unwrapped}/bin/clang \
      -DCMAKE_CXX_COMPILER=${llvm.clang-unwrapped}/bin/clang++ \
      -DCMAKE_AR=${llvm.llvm}/bin/llvm-ar \
      -DCMAKE_RANLIB=${llvm.llvm}/bin/llvm-ranlib \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
      -DMAICORD_BUILD_TESTS=${if buildTests then "ON" else "OFF"}
    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    export MAICORD_TOOLCHAIN_CACHE=$TMPDIR/toolchain
    export CXX=${stdenv.cc}/bin/c++
    cmake --build $TMPDIR/build --target ${buildTarget}
    runHook postBuild
  '';

  # The release tarball's layout, made by the same script the release uses, then installed like the -bin package.
  installPhase = ''
    runHook preInstall
    python3 packaging/build_tarball.py --binary $TMPDIR/build/${buildTarget} --version ${version} --output-dir $TMPDIR/tarball
    tar -xzf $TMPDIR/tarball/maicord-${version}-linux-x86_64.tar.gz -C $TMPDIR/tarball
    cd $TMPDIR/tarball/maicord-${version}-linux-x86_64
    install -Dm755 MaiCord $out/lib/maicord/MaiCord
    install -d $out/bin
    ln -s $out/lib/maicord/MaiCord $out/bin/maicord
    cp -r fonts licenses $out/lib/maicord/
    mkdir -p $out/share
    cp -r share/. $out/share/
    runHook postInstall
  '';

  meta = {
    description = "A lightweight, native Discord client (unofficial)";
    homepage = "https://maicord.com/";
    license = lib.licenses.mpl20;
    mainProgram = "maicord";
    platforms = [ "x86_64-linux" ];
  };
}

{
  lib,
  maicord,
  mesa,
  libglvnd,
}:

# The GPU tests of the client (tests/gpu: the shader pack, the ui quads, glyph, media and effect scenes against the
# pictures Direct3D 11 made), run on Mesa's llvmpipe in the sandbox. They are the check that the GLSL pack made from
# nixpkgs' DXC and SPIRV-Cross renders like the one made from the toolchain upstream pins.
#
# A build with tests defines test-only switches (MAICORD_TEST_HOOKS), so this is never the package that is installed:
# the result is only the log of the run.
maicord.overrideAttrs (old: {
  pname = "maicord-gpu-tests";

  cmakeFlags = old.cmakeFlags ++ [ (lib.cmakeBool "MAICORD_BUILD_TESTS" true) ];
  ninjaFlags = [
    "MaiCordGpuSyncTests"
    "MaiCordGpuClearReadbackTests"
    "MaiCordGpuShaderPackTests"
    "MaiCordGpuUiQuadsTests"
    "MaiCordGpuGlyphsTests"
    "MaiCordGpuMediaTests"
    "MaiCordGpuEffectsTests"
    "MaiCordGpuBitmapGlyphTests"
  ];
  # Only the test programs are built, and they carry the test hooks the client's audit refuses.
  postBuild = "";

  doCheck = true;
  # The test programs are freestanding images like the client, and need the loader and libraries the way it does.
  preCheck = ''
    for test in MaiCordGpu*Tests; do
      patchelf --set-interpreter "$(< "$NIX_BINTOOLS/nix-support/dynamic-linker")" \
        --set-rpath "${lib.makeLibraryPath (old.runtimeDependencies ++ [ libglvnd ])}" "$test"
    done
    export LIBGL_ALWAYS_SOFTWARE=1
    export __EGL_VENDOR_LIBRARY_FILENAMES=${mesa}/share/glvnd/egl_vendor.d/50_mesa.json
    export LIBGL_DRIVERS_PATH=${mesa}/lib/dri
  '';
  checkPhase = ''
    runHook preCheck
    ctest --tests-regex '^maicord_gpu_(sync|clear_readback|shader_pack|ui_quads|glyphs|media|effects|bitmap_glyph)' \
      --output-on-failure --parallel "$NIX_BUILD_CORES"
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out
    cp Testing/Temporary/LastTest.log $out/
    runHook postInstall
  '';

  doInstallCheck = false;
  passthru = { };
  meta = old.meta // {
    description = "GPU tests of MaiCord on llvmpipe";
  };
})

{
  description = "MaiCord, a lightweight native Discord client (unofficial)";

  # This flake lives in packaging/nix and builds the repository around it:
  #   nix build github:maj113/MaiCord?dir=packaging/nix              the source build (LLVM 23)
  #   nix build github:maj113/MaiCord?dir=packaging/nix#maicord-bin  the release tarball
  #   nix run   github:maj113/MaiCord?dir=packaging/nix
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      lib = pkgs.lib;

      root = ../..;
      version = builtins.head (
        builtins.match ".*project\\(MaiCord VERSION ([0-9]+\\.[0-9]+\\.[0-9]+).*" (
          builtins.readFile (root + "/CMakeLists.txt")
        )
      );
      src = lib.cleanSourceWith {
        src = root;
        filter =
          path: type:
          !(builtins.elem (baseNameOf path) [
            "docs"
            "Analysis"
            ".claude"
            ".github"
          ]);
      };

      maicord = pkgs.callPackage ./package.nix { inherit src version; };
      maicord-bin = pkgs.callPackage ./package-bin.nix { inherit version; };
    in
    {
      packages.${system} = {
        default = maicord;
        inherit maicord maicord-bin;
        # Until the client target exists: builds the shader pack test through the whole source pipeline (LLVM 23, DXC and
        # SPIRV-Cross, the GLSL pack, patching) in the client's place.
        build-check = maicord.override {
          buildTarget = "MaiCordGpuShaderPackTests";
          buildTests = true;
        };
      };

      apps.${system}.default = {
        type = "app";
        program = "${maicord}/bin/maicord";
        meta.description = "MaiCord";
      };

      overlays.default = final: _prev: {
        maicord = final.callPackage ./package.nix { inherit src version; };
        maicord-bin = final.callPackage ./package-bin.nix { inherit version; };
      };
    };
}

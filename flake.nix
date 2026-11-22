{
  # Development shells for niu. niu itself needs no Nix: `cc niu.c -o niu`.
  #
  #   nix develop               libclang 21 (nixpkgs' default LLVM)
  #   nix develop .#libclang18  libclang 18, the oldest niu supports
  #   nix develop .#latest      the newest LLVM in nixpkgs
  #
  # Then: make test   (or: cc niu.c -o niu && bash tests/run.sh)
  description = "niu development shells";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      shellWith =
        pkgs: llvm:
        pkgs.mkShell {
          packages =
            [
              llvm.clang # a C compiler for the tests (gcc comes with mkShell)
              pkgs.gnumake
              pkgs.bash
              pkgs.actionlint
              pkgs.shellcheck
            ]
            ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [ pkgs.tinycc ];

          # Point niu at this shell's libclang, so the version under test is
          # the one the shell is named after and not whatever the system has.
          # Unset it to test niu's own search.
          NIU_LIBCLANG = "${llvm.libclang.lib}/lib/libclang${pkgs.stdenv.hostPlatform.extensions.sharedLibrary}";

          shellHook = ''
            echo "niu dev shell: libclang ${llvm.libclang.version} (NIU_LIBCLANG=$NIU_LIBCLANG)"
          '';
        };
    in
    {
      devShells = forAllSystems (pkgs: {
        default = shellWith pkgs pkgs.llvmPackages;
        libclang18 = shellWith pkgs pkgs.llvmPackages_18;
        latest = shellWith pkgs pkgs.llvmPackages_latest;
      });
    };
}

{
  description = "libcmc: an explicit-state model checker for C protocol code";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        lib = pkgs.lib;

        # The library, the man pages, and `make check` (engine tests, bug
        # gates, purity gate, docs gate, fast calibration) as the check
        # phase: it needs only the compiler and binutils, so it runs in
        # the sandbox.  The purity gate reads x86-64 instructions;
        # elsewhere the check phase is off rather than half-run.
        mkLibcmc = stdenv: stdenv.mkDerivation {
          pname = "libcmc";
          version = "0-unstable-${self.lastModifiedDate or "19700101"}";
          src = lib.cleanSource ./.;

          nativeBuildInputs = [ pkgs.mandoc ];
          makeFlags = [ "CC=${stdenv.cc.targetPrefix}cc" "PREFIX=$(out)" ];  # cc is clang under clangStdenv
          buildFlags = [ "all" ];

          doCheck = stdenv.hostPlatform.isx86_64;
          preCheck = ''
            export HOME=$TMPDIR CMC_SCRATCH=$TMPDIR/cmc
            patchShebangs test scripts calib
          '';
          checkTarget = "check";

          meta = {
            description = "Explicit-state model checker for C protocol code";
            homepage = "https://gregburd.codeberg.page/libcmc/";
            license = lib.licenses.asl20;
            platforms = lib.platforms.unix;
          };
        };

        libcmc = mkLibcmc pkgs.stdenv;
      in {
        packages = {
          default = libcmc;
          inherit libcmc;
        };

        checks = {
          gcc = libcmc;
        } // lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
          clang = mkLibcmc pkgs.clangStdenv;
        };

        devShells.default = pkgs.mkShell {
          packages = (with pkgs; [
            gcc
            clang
            gnumake
            mandoc
            # the documentation site (docs/, Jekyll) and its link check
            ruby
            lychee
            # the calibration oracle: calib/hc/tlc.sh
            tlaplus
          ]) ++ lib.optionals pkgs.stdenv.hostPlatform.isLinux [ pkgs.gdb pkgs.valgrind ];
          shellHook = ''
            echo "libcmc: make check | make CC=clang check | make check-msan | make check-all"
            echo "docs:   make docs-site"
          '';
        };
      });
}

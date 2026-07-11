{
  rev,
  clangStdenv,
  mkShell,
  callPackage,
}: let
  defaultPackage = callPackage ./default.nix {inherit rev;};
in
  mkShell.override {stdenv = clangStdenv;} {
    inputsFrom = [defaultPackage];

    # add more dev packages here if needed
    packages = [];
  }

{
  inputs = {
    nixpkgs.url = "https://channels.nixos.org/nixpkgs-unstable/nixexprs.tar.xz";
  };

  outputs = {
    self,
    nixpkgs,
  }: let
    forAllSystems = function:
      nixpkgs.lib.genAttrs nixpkgs.lib.systems.flakeExposed (
        system: function nixpkgs.legacyPackages.${system}
      );
    rev = self.dirtyRev or self.rev;
  in {
    packages = forAllSystems (pkgs: {
      nari = pkgs.callPackage ./nix/default.nix {inherit rev;};
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.nari;
    });

    devShells = forAllSystems (pkgs: {
      default = pkgs.callPackage ./nix/shell.nix {inherit rev;};
    });
  };
}

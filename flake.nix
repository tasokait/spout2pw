{
  description = "Spout2 to PipeWire video bridge";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flakelight.url = "github:nix-community/flakelight";
    flakelight.inputs.nixpkgs.follows = "nixpkgs";
    self.submodules = true;
    subprojects-libfunnel.url = "path:subprojects/libfunnel";
    subprojects-libfunnel.flake = false;
    subprojects-pipewire-static.url = "path:subprojects/pipewire-static";
    subprojects-pipewire-static.flake = false;
    subprojects-spoutdxtoc.url = "path:subprojects/spoutdxtoc";
    subprojects-spoutdxtoc.flake = false;
  };

  outputs =
    { flakelight, ... }:
    flakelight ./. {
      packages.spout2pw = import ./nix/package.nix;
    };
}

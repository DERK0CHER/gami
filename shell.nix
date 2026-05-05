{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  packages = with pkgs; [
    meson
    ninja
    pkg-config
    gcc
    gtk4
    libadwaita
    glib
  ];
}

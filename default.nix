{
  lib,
  hyprland,
  hyprlandPlugins,
}:
hyprlandPlugins.mkHyprlandPlugin {
  pluginName = "hyprtouchbar";
  version = "1.4.0";
  src = ./.;

  inherit (hyprland) nativeBuildInputs;

  meta = with lib; {
    homepage = "https://github.com/minec-aw/htb";
    description = "Modern touch-friendly, CSD-aware titlebars for Hyprland";
    license = licenses.bsd3;
    platforms = platforms.linux;
  };
}

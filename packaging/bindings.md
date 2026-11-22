# Optional desktop bindings

These are examples only. Snipline never edits compositor configuration.
Use your compositor's current configuration syntax and resolve conflicts first.

## Hyprland

For traditional `hyprland.conf` bindings:

```ini
bind = , Print, exec, snipline capture region
bind = SHIFT, Print, exec, snipline capture monitor
bind = SUPER, Print, exec, snipline capture window
bind = CTRL, Print, exec, snipline record region
```

If using a Lua configuration frontend, bind these same argument vectors with
that frontend's documented process-launch API. There is no universal Hyprland
Lua configuration API to safely assume.

## niri

Add inside your existing `binds` block:

```kdl
Print { spawn "snipline" "capture" "region"; }
Shift+Print { spawn "snipline" "capture" "monitor"; }
Mod+Print { spawn "snipline" "capture" "window"; }
Ctrl+Print { spawn "snipline" "record" "region"; }
```

## Optional login startup

The package supplies `/usr/share/snipline/snipline-autostart.desktop`, not an
enabled system autostart entry. To opt in on desktops that support XDG autostart,
copy it to `~/.config/autostart/`. Remove your copied entry to opt out.
On compositors without XDG autostart, launch `snipline daemon --tray` using their
documented startup mechanism instead. Do not use both mechanisms.

`--tray` is optional; without it the resident runs without a tray. A tray host
(StatusNotifierItem support) must be available to display the icon.

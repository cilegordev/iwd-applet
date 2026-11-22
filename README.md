# iwd-applet

A small Wi-Fi tray applet for [iwd](https://iwd.wiki.kernel.org/), written in C with GTK 3.
It talks to iwd over D-Bus and lets you pick a network, enter passwords, and manage saved
networks straight from the system tray.

> **Status:** stable. Version 0.2.2 is ready for daily use; see [Known limitations](#known-limitations) for what is not implemented yet.

## Features

- Lists every network iwd has found, with signal-strength icons (a lock icon marks secured networks)
- A status line at the top shows the current connection and its signal strength
- Connect, disconnect, rescan, and switch Wi-Fi on or off
- Password prompts through iwd's agent interface
- Connect to hidden networks and forget saved ones
- Left click opens the network menu, right click shows **About** and **Quit**
- **Rescan** keeps the menu open and updates the list when the scan finishes
- Long lists are split into columns instead of scrolling, so nothing is hidden
- An About dialog showing the version and the library versions the applet was built with
- Optional desktop notifications (libnotify)
- `iwtui`: a text-mode interface that runs in any terminal (see [iwtui](#iwtui))

## Requirements

| | |
|---|---|
| Build tools | a C compiler, [Meson](https://mesonbuild.com/) ≥ 0.60, Ninja, pkg-config |
| Libraries (pkg-config modules) | `gtk+-3.0` ≥ 3.24, `gio-unix-2.0` ≥ 2.74, `dbusmenu-glib-0.4` (default backend), `libnotify` (optional), `ncursesw` (optional, for `iwtui`) |
| Runtime | `iwd`, D-Bus, an icon theme that provides the standard `network-wireless-*` icons (for example Adwaita) |

You also need something that assigns IP addresses, for example `systemd-networkd`, `dhcpcd`, or
iwd's own `EnableNetworkConfiguration=true`. The applet does not touch IP configuration.

Example commands to install the dependencies (package names can differ between releases):

```sh
# Debian / Ubuntu
sudo apt install build-essential meson ninja-build pkg-config libgtk-3-dev \
                 libdbusmenu-glib-dev libnotify-dev libncurses-dev desktop-file-utils iwd

# Fedora
sudo dnf install gcc meson ninja-build pkgconf-pkg-config gtk3-devel \
                 libdbusmenu-devel libnotify-devel ncurses-devel desktop-file-utils iwd

# Arch Linux
sudo pacman -S --needed base-devel meson pkgconf gtk3 libdbusmenu-glib \
                 libnotify ncurses desktop-file-utils iwd
```

## Building

```sh
meson setup --prefix=/usr build
ninja -C build
./build/iwd-applet            # try the tray applet without installing
./build/iwtui                 # try the text-mode interface
sudo ninja -C build install   # also installs an autostart entry
```

Build options:

| Option | Values | Default | Description |
|---|---|---|---|
| `-Dtray` | `sni`, `appindicator`, `statusicon` | `sni` | Tray backend, see below |
| `-Dnotify` | `auto`, `enabled`, `disabled` | `auto` | Desktop notifications via libnotify |
| `-Dtui` | `auto`, `enabled`, `disabled` | `auto` | Build `iwtui` (needs ncursesw); skipped automatically if ncursesw is missing |

## iwtui

`iwtui` is a text-mode interface for iwd. It runs in any terminal, including a plain console
without a graphical session, and does not need GTK. It has a black background and three entries:

1. **Connect to a Wi-Fi network**: shows the networks found; pick one and, if it needs a password,
   type it right there
2. **Turn Wi-Fi on / off**
3. **Exit**

Keys: `Up`/`Down` (or `j`/`k`) select, `Enter` confirms, `Esc` goes back, `r` rescans in the network
list, `Tab` shows or hides the password while typing, `q` exits.

To ask for a password, `iwtui` registers itself as iwd's agent. If another program has already
registered an agent (for example the tray applet), `iwtui` can still connect to open and saved
networks, but it cannot ask for a new password until that program is closed.

## Tray backends

The applet can show its icon in three ways. Which one works depends on what your system tray
supports.

| Backend | How it works | Extra dependency |
|---|---|---|
| `sni` (default) | Speaks the StatusNotifierItem protocol directly over GDBus. Left click opens the network menu, which the applet draws itself. Right click shows a small menu (About, Quit) that the tray draws. | `dbusmenu-glib-0.4` |
| `appindicator` | Uses libappindicator, which only exposes a single menu that the tray draws. Both clicks open it, with About and Quit at the bottom. The menu closes after every click. | `appindicator3-0.1` (libappindicator) |
| `statusicon` | Uses the legacy `GtkStatusIcon` (XEmbed) tray. Both menus are drawn by the applet, so they look compact. X11 only, and `GtkStatusIcon` is deprecated in GTK 3. | none |

libappindicator does not report right clicks to the application, which is why the `sni` backend
exists. In that backend the right-click menu is drawn by the tray, so any blank space before its
icons is up to the tray and cannot be changed by the applet.

## System notes

- Your user must be allowed by iwd's D-Bus policy (typically `iwd-dbus.conf` in
  `/usr/share/dbus-1/system.d/`, which grants access to a group such as `netdev` or `wheel`).
  Otherwise the applet cannot talk to iwd or register its agent.
- Only one service should manage a given Wi-Fi device. Make sure nothing else controls the same
  interface as iwd.
- Only one instance runs at a time (application ID `org.iwdapplet.IwdApplet`).

## Known limitations

- Not implemented yet: 802.1x / EAP, hidden networks in `iwtui`, sharing a network as a QR code, access point mode.
- While you are connected, iwd does not scan periodically, so the list can be stale; use
  **Rescan** to refresh it.
- **Forget Network** only lists saved networks that are currently in range.
- Menu placement was verified on X11 only.

## Testing without a Wi-Fi card

The D-Bus bus type can be overridden at build time, which lets the applet talk to a mock iwd on
the session bus:

```sh
meson setup build-test -Dc_args='-DIWD_BUS_TYPE=G_BUS_TYPE_SESSION'
```

Regular builds always use the system bus.

## License

GPL-3.0-or-later.

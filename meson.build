project('iwd-applet', 'c',
  version: '0.2.2',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.60.0',
  default_options: ['c_std=gnu11', 'warning_level=2'])

cc = meson.get_compiler('c')
add_project_arguments(
  cc.get_supported_arguments(['-Wno-deprecated-declarations', '-Wno-unused-parameter']),
  language: 'c')

tray_backend = get_option('tray')

gtk_dep          = dependency('gtk+-3.0', version: '>= 3.24')
gio_dep          = dependency('gio-unix-2.0', version: '>= 2.74')
notify_dep       = dependency('libnotify', required: get_option('notify'))
appindicator_dep = dependency('appindicator3-0.1', required: tray_backend == 'appindicator')
dbusmenu_dep     = dependency('dbusmenu-glib-0.4', required: tray_backend == 'sni')
ncurses_dep      = dependency('ncursesw', required: get_option('tui'))

conf = configuration_data()
conf.set('HAVE_LIBNOTIFY', notify_dep.found())
conf.set_quoted('APP_VERSION', meson.project_version())
conf.set_quoted('TRAY_BACKEND', tray_backend)
# Compile-time versions, shown in the About dialog
conf.set_quoted('DEP_GTK', gtk_dep.version())
conf.set_quoted('DEP_GLIB', gio_dep.version())
conf.set_quoted('DEP_NOTIFY', notify_dep.found() ? notify_dep.version() : '')
conf.set_quoted('DEP_APPINDICATOR',
  (tray_backend == 'appindicator' and appindicator_dep.found()) ? appindicator_dep.version() : '')
conf.set_quoted('DEP_DBUSMENU',
  (tray_backend == 'sni' and dbusmenu_dep.found()) ? dbusmenu_dep.version() : '')
conf.set_quoted('BUILD_COMPILER', cc.get_id() + ' ' + cc.version())
conf.set_quoted('BUILD_MESON', meson.version())
configure_file(output: 'config.h', configuration: conf)

sources = files(
  'src/main.c',
  'src/iwd-client.c',
  'src/iwd-agent.c',
  'src/menu.c',
  'src/dialogs.c',
  'src/app-notify.c',
)
deps = [gtk_dep, gio_dep, notify_dep]

if tray_backend == 'sni'
  sources += files('src/tray-sni.c')
  deps += dbusmenu_dep
elif tray_backend == 'appindicator'
  sources += files('src/tray-appindicator.c')
  deps += appindicator_dep
else
  sources += files('src/tray-statusicon.c')
endif

executable('iwd-applet', sources,
  dependencies: deps,
  include_directories: include_directories('.'),
  install: true)

# iwtui: text-mode interface. It shares the iwd client with the tray applet but needs no GTK.
if ncurses_dep.found()
  executable('iwtui',
    files('src/iwtui.c', 'src/iwtui-agent.c', 'src/iwd-client.c'),
    dependencies: [gio_dep, ncurses_dep],
    include_directories: include_directories('.'),
    install: true)
endif

desktop_file = 'data/org.iwdapplet.IwdApplet.desktop'
install_data(desktop_file, install_dir: get_option('datadir') / 'applications')
install_data(desktop_file, install_dir: get_option('sysconfdir') / 'xdg' / 'autostart')

dfv = find_program('desktop-file-validate', required: false)
if dfv.found()
  test('validate-desktop', dfv, args: [files(desktop_file)])
endif

project(
  'lunduke-about',
  'cpp',
  version: '0.2.6',
  license: 'GPL-3.0-or-later',
  meson_version: '>=0.56.0',
  default_options: [
    'cpp_std=c++17',
    'warning_level=2',
    'werror=false',
  ],
)

gtkmm_dep = dependency('gtkmm-3.0', version: '>=3.24')
gdk_x11_dep = dependency('gdk-x11-3.0')
x11_dep = dependency('x11')
gdkpixbuf_dep = dependency('gdk-pixbuf-2.0')

conf = configuration_data()
conf.set_quoted('APP_VERSION', meson.project_version())
conf.set_quoted('APP_ID', 'org.lunduke.AboutThisComputer')
conf.set_quoted('DATADIR', join_paths(get_option('prefix'), get_option('datadir'), 'lunduke-about'))
conf.set_quoted('SOURCE_DATADIR', join_paths(meson.current_source_dir(), 'data'))

configure_file(
  output: 'config.h',
  configuration: conf,
)

lunduke_about_inc = include_directories('src', '.')

lunduke_about_sources = files(
  'src/main.cpp',
  'src/application.cpp',
  'src/main_window.cpp',
  'src/system_info.cpp',
  'src/window_enum.cpp',
  'src/app_row.cpp',
  'src/memory_bar.cpp',
)

executable(
  'lunduke-about',
  lunduke_about_sources,
  dependencies: [gtkmm_dep, gdk_x11_dep, x11_dep, gdkpixbuf_dep],
  include_directories: lunduke_about_inc,
  install: true,
)

install_data(
  'data/supporters.txt',
  install_dir: join_paths(get_option('datadir'), 'lunduke-about'),
)

install_data(
  'data/pixmaps/lcos-logo-black.svg',
  'data/pixmaps/lcos-logo-black-128.png',
  'data/pixmaps/lcos-logo-black-256.png',
  'data/pixmaps/lcos-logo-black-preview.png',
  'data/pixmaps/lcos-outline-32.png',
  'data/pixmaps/lcos-outline-256.png',
  install_dir: join_paths(get_option('datadir'), 'lunduke-about', 'pixmaps'),
)

# Simple LCOS outline for window/menu icon (not the full rings mark).
icon_sizes = ['16x16', '32x32', '48x48', '128x128', '256x256']
foreach size : icon_sizes
  install_data(
    join_paths('data', 'icons', 'hicolor', size, 'apps', 'org.lunduke.AboutThisComputer.png'),
    install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', size, 'apps'),
  )
endforeach

install_data(
  'data/org.lunduke.AboutThisComputer.desktop',
  install_dir: join_paths(get_option('datadir'), 'applications'),
)

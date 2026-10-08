project(
  'lunduke-about',
  'cpp',
  version: '0.9',
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
  'src/about_logic.cpp',
  'src/window_enum.cpp',
  'src/app_row.cpp',
  'src/memory_bar.cpp',
)

about_logic_test = executable(
  'about-logic-test',
  files('tests/logic_test.cpp', 'src/about_logic.cpp', 'src/system_info.cpp'),
  include_directories: lunduke_about_inc,
  dependencies: [x11_dep],
)
test(
  'about-logic',
  about_logic_test,
  timeout: 60,
  env: [
    'LUNDUKE_SUPPORTERS_TXT=' + join_paths(meson.project_source_root(), 'data', 'supporters.txt'),
  ],
)

about_ui_test = executable(
  'about-ui-test',
  files(
    'tests/ui_test.cpp',
    'src/main_window.cpp',
    'src/system_info.cpp',
    'src/about_logic.cpp',
    'src/window_enum.cpp',
    'src/app_row.cpp',
    'src/memory_bar.cpp',
  ),
  dependencies: [gtkmm_dep, gdk_x11_dep, x11_dep, gdkpixbuf_dep],
  include_directories: lunduke_about_inc,
)
xvfb_run = find_program('xvfb-run', required: true)
test(
  'about-ui',
  xvfb_run,
  args: ['-a', about_ui_test],
  timeout: 120,
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

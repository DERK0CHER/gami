project('origami-fold', 'c',
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ])

cc = meson.get_compiler('c')

add_project_arguments(
  cc.get_supported_arguments([
    '-Wno-unused-parameter',
    '-Wno-missing-field-initializers',
  ]),
  language: 'c')

deps = [
  dependency('gtk4', version: '>= 4.10'),
  dependency('libadwaita-1', version: '>= 1.5'),
  cc.find_library('m', required: false),
]

sources = files(
  'src/main.c',
  'src/window.c',
  'src/canvas.c',
  'src/canvas3d.c',
  'src/paper.c',
  'src/crease.c',
  'src/crease_canvas.c',
  'src/crease_canvas3d.c',
)

executable('origami-fold',
  sources,
  dependencies: deps,
  install: true)

# Headless smoke test for the new crease-graph model. Links only glib.
glib_dep = dependency('glib-2.0', version: '>= 2.74')
executable('test-crease',
  files('src/crease.c', 'src/test_crease.c'),
  dependencies: [glib_dep, cc.find_library('m', required: false)],
  install: false)

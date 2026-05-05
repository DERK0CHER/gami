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
  'src/paper.c',
)

executable('origami-fold',
  sources,
  dependencies: deps,
  install: true)

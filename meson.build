project('7mm_udp_rx', ['c', 'cpp', 'cuda'],
  version: '1.0',
  default_options: ['warning_level=2', 'c_std=gnu11', 'cpp_std=c++20']
)

inc = include_directories('include')

dpdk_dep = dependency('libdpdk', required: true)
cuda_dep = dependency('cuda', required: true)

cc = meson.get_compiler('cuda')
cufft_lib = cc.find_library('cufft', required: true)
spdlog_dep = dependency('spdlog')
yaml = dependency('yaml-cpp', required: true)

dpdk_lib = static_library('dpdk',
  'src/dpdk.cpp',
  include_directories: inc,
  dependencies: dpdk_dep,
)

executable('7mm',
  ['src/main.cu',
  'src/init.cu',
  ],
  include_directories: inc,
  dependencies:  [cuda_dep, cufft_lib,yaml,spdlog_dep],
  link_with: dpdk_lib,
  override_options: ['cuda_args= -arch=sm_86']
)

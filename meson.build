project(
    '7mm_udp_rx',
    ['c', 'cpp', 'cuda'],
    version: '1.0',
    default_options: [
        'warning_level=2',
        'c_std=gnu11',
        'cpp_std=c++17',
        'cuda_args=-arch=sm_86',
    ]
)

inc = include_directories('include')

dpdk_dep = dependency('libdpdk', required: true)
cuda_dep = dependency('cuda', required: true)
spdlog_dep = dependency('spdlog', required: true)
yaml_dep = dependency('yaml-cpp', required: true)
fmt_dep = dependency('fmt', required: true)

cuda = meson.get_compiler('cuda')

cudart = cuda.find_library(
    'cudart',
    dirs: '/usr/local/cuda/lib64',
    required: true
)

cufft = cuda.find_library(
    'cufft',
    dirs: '/usr/local/cuda/lib64',
    required: true
)

dpdk_lib = static_library(
    'dpdk',
    'src/dpdk.cpp',
    include_directories: inc,
    dependencies: dpdk_dep,
)

executable(
    '7mm',
    [
        'src/main.cu',
        'src/init.cu',
    ],
    include_directories: inc,
    dependencies: [
        cuda_dep,
        cudart,
        cufft,
        yaml_dep,
        spdlog_dep,
        fmt_dep,
    ],
    link_with: dpdk_lib,
)
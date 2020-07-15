project(
  'cpp-irc-bot',
  'cpp', 'c',
  version: '0.0.1',
  default_options: ['cpp_std=c++17'],
)

compiler = meson.get_compiler('cpp')

threads = dependency('threads')
fmt = compiler.find_library('fmt', static: true, dirs: [meson.current_source_dir() + '/third_party/fmt/build'] )
openssl = compiler.find_library('ssl', static: true, dirs: [meson.current_source_dir() + '/third_party/openssl'] )
crypto = compiler.find_library('crypto', static: true, dirs: [meson.current_source_dir() + '/third_party/openssl'] )
dl = compiler.find_library('dl', static: true, dirs: [meson.current_source_dir() + '/third_party/openssl/openssl-1.1.1g'] )
curl = compiler.find_library('curl', static: true, dirs: [meson.current_source_dir() + '/third_party/curl/lib/.libs'] )
cares = compiler.find_library('cares', static: true, dirs: [meson.current_source_dir() + '/third_party/c-ares/build/lib'] )
z = compiler.find_library('z', static: true)

includes = include_directories('third_party/nlohmann_json/single_include', 'third_party/ctre/single-header')

exe = executable(
  'main',
  'main.cpp',
  'CurlEngine.cpp',
  'find_youtube_ids.cpp',
  include_directories: [
    includes,
    include_directories('third_party/fmt/include'),
  ],
  dependencies : [
    threads,
    fmt,
    openssl,
    crypto,
    curl,
    dl,
    z,
    cares,
  ],
  link_args: [
    '-static',
    '-static-libgcc',
  ],
)

gtest = dependency('gtest', required: false)

test_exe = executable(
  'tests',
  'test_main.cpp',
  'test_irc_stream.cpp',
  include_directories: [
    includes,
    include_directories('third_party/fmt/include'),
  ],
  dependencies : [
    gtest,
    fmt,
  ],
)

test('tests', test_exe)

# mruby build configuration for the in-game debug console.
#
# CMake runs mruby's rake build with MRUBY_CONFIG pointing here and
# MRUBY_BUILD_DIR inside the CMake build tree, producing host/lib/libmruby.a.
#
# No IO gems: the console replaces print/puts/p with its own scrollback, and a
# debug interpreter has no business opening sockets.

MRuby::Build.new do |conf|
  conf.toolchain :clang

  conf.gembox 'stdlib'
  conf.gembox 'stdlib-ext'
  conf.gembox 'math'
  conf.gembox 'metaprog'      # mruby-compiler (needed to eval typed lines), eval, method
end

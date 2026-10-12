# mruby build configuration for the in-game debug console.
#
# CMake runs mruby's rake build with MRUBY_CONFIG pointing here and
# MRUBY_BUILD_DIR inside the CMake build tree, producing host/lib/libmruby.a.
#
# No IO gems: the console replaces print/puts/p with its own scrollback, and a
# debug interpreter has no business opening sockets.

MRuby::Build.new do |conf|
  # CMake passes the toolchain matching its compiler (clang, gcc or visualcpp);
  # clang when run by hand.
  toolchain = (ENV['MRUBY_TOOLCHAIN'] || 'clang').to_sym
  conf.toolchain toolchain

  # MSVC: the static C runtime (/MT), matching CMAKE_MSVC_RUNTIME_LIBRARY, so the
  # game links one CRT and needs no redistributable.
  if toolchain == :visualcpp
    [conf.cc, conf.cxx].each do |c|
      c.flags = c.flags.flatten.map { |f| f == '/MD' ? '/MT' : f }
    end
  end

  conf.gembox 'stdlib'
  conf.gembox 'stdlib-ext'
  conf.gembox 'math'
  conf.gembox 'metaprog'      # mruby-compiler (needed to eval typed lines), eval, method
end

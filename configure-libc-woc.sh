# Use -fno-builtin to make sure our hooked versions of glibc functionality (like strlen) aren't replaced by gcc builtins
CFLAGS="-O2 -fno-builtin" ../configure --enable-stackguard-randomization --enable-obsolete-rpc --enable-pt_chown --with-selinux --enable-lock-elision=no --enable-addons=nptl --prefix=$HOME/glibc-build --sysconfdir=$HOME/glibc-build/etc/

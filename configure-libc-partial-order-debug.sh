# Use -fno-builtin to make sure our hooked versions of glibc functionality (like strlen) aren't replaced by gcc builtins
# Use -Wno-alloc-larger-than to squash the warning that suddenly pops up when strlen is not a builtin on -O1 (what?)
CFLAGS="-Wno-maybe-uninitialized -fno-stack-protector -O1 -ggdb -fno-omit-frame-pointer -fno-builtin -Wno-alloca-larger-than -DMVEE_USE_TOTALPARTIAL_AGENT" ../configure --enable-stackguard-randomization --enable-obsolete-rpc --enable-pt_chown --with-selinux --enable-lock-elision=no --enable-addons=nptl --prefix=$HOME/glibc-build  --sysconfdir=/etc/

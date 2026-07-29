# This is a -*-perl-*- script
#
# Set variables that were defined by configure, in case we need them
# during the tests.

%CONFIG_FLAGS = (
    AM_LDFLAGS   => '-Wl,--export-dynamic',
    AR           => 'ar',
    CC           => 'musl-gcc',
    CFLAGS       => '-O2 -march=x86-64 -mtune=generic -fno-pie ',
    CPP          => 'musl-gcc -E',
    CPPFLAGS     => '',
    GUILE_CFLAGS => '',
    GUILE_LIBS   => '',
    LDFLAGS      => '-static -no-pie',
    LIBS         => ''
);

1;

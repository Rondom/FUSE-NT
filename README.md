What is this?
=============

FUSE-NT is really two pieces of software:

1. A Windows IFS driver component (ifs/), and
2. A set of patches to the FUSE userspace libraries (fuse-2.8.5/)

Together, these pieces seek to enable FUSE filesystems written against the
latest version of the libfuse API to run unmodified on Cygwin on Windows
systems.


Building: IFS driver
====================

On Fedora 20, with the mingw64-headers and other mingw64 packages installed:

    $ cd ifs/fuse/wxp
    $ mingw64-env
    $ make

Will build a kernel module named `ntfuse.sys`.


Building: FUSE
==============

Work in progress ... this is the idea:

    $ cd fuse-*
      # Apply fuse-NT patches:
    $ quilt push -a
      # Set-up MingW64 build environment
    $ mingw64-env
      # Typical autoconf and build:
    $ ./configure --target=x86_64-w64-mingw32
    $ make


Errata
======

Lots of things are broken; in particular, none of the userspace stuff is
remotely thread-safe yet; nothing exits cleanly or cleans up after itself;
and the userspace code tends to segfault in various places. When it breaks,
you get to keep all of the pieces! :-)

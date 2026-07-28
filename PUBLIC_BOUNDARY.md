# Public distribution boundary

This repository is a clean community source distribution. It intentionally
contains no private environment configuration, credentials, player records,
logs, backups, database files or deployment snapshots.

The repository includes source code for the native HTTP/API bridge, the
embedded static web panel, the server Lua bridge and the UE4SS C++ modules. It
deliberately does not include compiled UE4SS binaries, UE4SS import libraries,
server executables or locally generated build output. Developers must build
against their own matching local SDK and runtime.

It also intentionally excludes game files, extracted images, maps, catalogs and
other material whose redistribution rights are not documented. Empty templates
are supplied only so the panel and bridge handle the absence of optional data
without failing.

If you add local material for your own installation, keep it outside version
control and verify that you have the right to use and distribute it. Before a
pull request or release, run a private review for credentials, personal data,
environment identifiers and unlicensed content.

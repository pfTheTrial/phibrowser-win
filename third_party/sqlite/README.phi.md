# SQLite (vendored)

- Version: 3.53.4
- Source: https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
  (SHA3-256 628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e,
  as published on sqlite.org/download.html)
- License: public domain (https://www.sqlite.org/copyright.html)

Contents kept: `sqlite3.c` and `sqlite3.h`, unmodified. The client's local
user-data store (client/phi_local_store.*, the Windows twin of the Mac's
SwiftData LocalStore — docs/user-data-parity.md) is the only user.

Why a copy rather than Chromium's: the framework link kit carries no sqlite
objects, Chromium's copy is renamed (`chrome_sqlite3_*`) and patched, and the
client must also build from a framework SDK with no Chromium checkout.
build-client.py compiles `sqlite3.c` as C with the kit's target flags and the
compile-time options listed there; the host tests link the system libsqlite3.

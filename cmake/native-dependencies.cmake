find_package(flatbuffers CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
# A project's versions are a git repository (core/src/versions.cc).
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
# A static libgit2 (nix/libgit2-local.nix) brings none of its own
# dependencies: add what its pkg-config file lists as private, after it.
if(LIBGIT2_LINK_LIBRARIES MATCHES "libgit2\\.a$")
  set(_libgit2_private ${LIBGIT2_STATIC_LIBRARIES})
  list(REMOVE_ITEM _libgit2_private git2)
  target_link_directories(PkgConfig::LIBGIT2 INTERFACE ${LIBGIT2_STATIC_LIBRARY_DIRS})
  target_link_libraries(PkgConfig::LIBGIT2 INTERFACE ${_libgit2_private} ${LIBGIT2_STATIC_LDFLAGS_OTHER})
endif()
# The embedded Nix evaluator: the same Nix version as the environment's daemon.
pkg_check_modules(NIX REQUIRED IMPORTED_TARGET nix-expr nix-store nix-fetchers nix-flake nix-util)
# Hashing: CommonCrypto on macOS, OpenSSL's libcrypto elsewhere.
if(NOT APPLE)
  pkg_check_modules(CRYPTO REQUIRED IMPORTED_TARGET libcrypto)
endif()
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL 3 REQUIRED)

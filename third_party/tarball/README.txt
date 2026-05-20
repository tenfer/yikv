Offline / air-gapped workflow
-----------------------------

1) Populate this directory (checksum-verified):

   bash scripts/fetch_bazel_tarballs.sh

2) Optional: populate local vendor/ (ignored by git) after MODULE.bazel or MODULE.bazel.lock changes:

   bazel vendor --vendor_dir=vendor

   (Creates/updates vendor/VENDOR.bazel locally when needed.)

3) Optional strict offline build:

   bazel build --config=vendor_offline //:yikv_server

Basenames here must match the basename of the upstream URL for --distdir
(bazel_features-*, libpfm-4.11.0.tar.gz, cmake-3.23.2-linux-x86_64.tar.gz). nlohmann_json and brpc
are wired via MODULE.bazel local_tarball() and use the filenames listed in
BUILD.bazel in this directory.

Commit the *.tar.gz files if your policy requires a fully self-contained clone.

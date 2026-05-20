"""Extract a committed tarball from the main workspace into an external repository."""

def _local_tarball_impl(rctx):
    src = rctx.path(rctx.attr.src)
    if not src.exists:
        fail(
            "Missing local tarball %s. Run:  bash scripts/fetch_bazel_tarballs.sh" % (src,),
        )
    rctx.extract(src, stripPrefix = rctx.attr.strip_prefix)
    if rctx.attr.build_file_content:
        rctx.file("BUILD.bazel", rctx.attr.build_file_content)

local_tarball = repository_rule(
    implementation = _local_tarball_impl,
    attrs = {
        "src": attr.label(mandatory = True, allow_single_file = True),
        "strip_prefix": attr.string(mandatory = True),
        "build_file_content": attr.string(),
    },
)

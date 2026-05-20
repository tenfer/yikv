"""Compile flags for yikv code under libs/yikv.

Headers use `#include "db/db.h"` / `"alloc/..."` / … resolved from **`libs/yikv`** via
`-Ilibs/yikv`. Append **`YIKV_COMPAT_COPTS`** alongside `-std=` on any target that compiles these
sources or pulls these headers."""


YIKV_COMPAT_COPTS = ["-Ilibs/yikv"]

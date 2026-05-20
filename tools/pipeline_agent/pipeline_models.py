"""Pydantic request bodies shared by pipeline_agent HTTP and pipeline_ops."""

from __future__ import annotations

from pydantic import BaseModel, Field, model_validator


class PipelineError(Exception):
    """Non-HTTP error with status code for conversion to HTTPException in FastAPI routes."""

    def __init__(self, status_code: int, detail: str) -> None:
        self.status_code = status_code
        self.detail = detail
        super().__init__(detail)


_CLOUD_URI_PREFIXES = ("s3://", "oss://", "cos://", "obs://", "gs://")


def is_cloud_uri(s: str) -> bool:
    t = s.strip()
    low = t.lower()
    return any(low.startswith(p) for p in _CLOUD_URI_PREFIXES)


class BuildIndexBody(BaseModel):
    table: str = Field(..., min_length=1)
    input: str | list[str] | None = Field(
        None,
        description="Local file, local directory, cloud URI(s), or JSON array of files/URIs (maps to --input / --input_dir).",
    )
    data_dir: str | None = Field(
        None,
        description="Deprecated: same as input when input is a single local directory path; do not set together with input.",
    )
    schema_json: str | None = Field(None, description="Defaults to SCHEMA_JSON env / repo schema.json")
    create_if_missing: bool = Field(
        False,
        description="Ignored; yikv_import_pipeline always runs with --create_if_missing after removing BUILD_DB/<table>.",
    )
    recreate: bool = Field(
        False,
        description="Ignored for import; every build always deletes BUILD_DB/<table> on this host then creates a new index directory.",
    )

    @model_validator(mode="after")
    def input_xor_data_dir(self) -> BuildIndexBody:
        has_dd = self.data_dir is not None and str(self.data_dir).strip() != ""
        inp = self.input
        if isinstance(inp, list):
            has_in = len(inp) > 0
        elif isinstance(inp, str):
            has_in = bool(inp.strip())
        else:
            has_in = False
        if has_dd and has_in:
            raise ValueError("set only one of input or data_dir")
        if not has_dd and not has_in:
            raise ValueError("set input (string or non-empty array) or legacy data_dir")
        return self


class PublishIndexBody(BuildIndexBody):
    """Build + push; use on build host."""

    build_id: str | None = Field(None, description="Optional explicit build_id for push; else auto")
    cleanup_build_db_after_push: bool = Field(
        False,
        description="If true, remove BUILD_DB/<table> after a successful push to free local disk.",
    )


class DeployIndexBody(BaseModel):
    """Pull (switch active) + symlink server table + reload; use on online host."""

    table: str = Field(..., min_length=1)
    build_id: str | None = None
    force_refresh: bool = False
    max_local_versions: int = Field(2, ge=0)


class PushIndexBody(BaseModel):
    table: str = Field(..., min_length=1)
    build_id: str | None = Field(None, description="omit for auto timestamp id")


class PullIndexBody(BaseModel):
    table: str = Field(..., min_length=1)
    build_id: str | None = None
    force_refresh: bool = False
    max_local_versions: int = Field(2, ge=0)
    switch_active: bool = False


class SwitchReloadBody(BaseModel):
    table: str = Field(..., min_length=1)
    build_id: str | None = Field(None, description="Version dir under releases/<table>/; omit = lexicographic max local")


def resolve_import_cli(body: BuildIndexBody) -> tuple[str, list[str]]:
    """Return (mode, paths): mode is input_dir or input; paths always non-empty."""
    from pathlib import Path

    if body.data_dir is not None and str(body.data_dir).strip():
        dpath = Path(str(body.data_dir).strip()).expanduser().resolve()
        if not dpath.is_dir():
            raise PipelineError(400, f"data_dir not a directory: {dpath}")
        return ("input_dir", [str(dpath)])

    assert body.input is not None
    raw = body.input
    if isinstance(raw, list):
        out: list[str] = []
        for i, item in enumerate(raw):
            s = str(item).strip()
            if not s:
                raise PipelineError(400, f"input[{i}] is empty")
            if is_cloud_uri(s):
                out.append(s)
                continue
            p = Path(s).expanduser().resolve()
            if not p.is_file():
                raise PipelineError(400, f"input[{i}] not a file: {p}")
            out.append(str(p))
        return ("input", out)

    s = str(raw).strip()
    if is_cloud_uri(s):
        return ("input", [s])
    p = Path(s).expanduser().resolve()
    if p.is_dir():
        return ("input_dir", [str(p)])
    if p.is_file():
        return ("input", [str(p)])
    raise PipelineError(400, f"input path not found (not a file or directory): {p}")

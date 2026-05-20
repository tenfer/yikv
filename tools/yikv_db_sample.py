#!/usr/bin/env python3
"""Sample rows from a running yikv_server via gRPC (FlatBuffers payload).

  pip install -r requirements.txt   # run from yikv-server repo root
  (grpc + flatbuffers only; gRPC wrapper uses a tiny manual protobuf wire encoder, no protobuf Python package.)

Examples:
  python3 tools/yikv_db_sample.py --server 127.0.0.1:9000 --table mytbl --keys k1,k2
  python3 tools/yikv_db_sample.py --server 127.0.0.1:9000 --table mytbl --keys-file pks.txt
  python3 tools/yikv_db_sample.py --server 127.0.0.1:9000 --table mytbl \\
      --parquet /data/x.parquet --pk-column key --limit 32 --schema-json ./schema.json

Server responses include optional field_name per FieldValue; --schema-json is only needed
for older binaries or when names are omitted.

Env: YIKV_GRPC_TARGET if --server omitted.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

_TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(_TOOLS / "yikv_fb_gen"))

import flatbuffers  # noqa: E402
import grpc  # noqa: E402
from yikv.BatchGetRequest import (  # noqa: E402
    BatchGetRequestAddPks,
    BatchGetRequestAddTableName,
    BatchGetRequestEnd,
    BatchGetRequestStart,
    BatchGetRequestStartPksVector,
)
from yikv.BatchGetResponse import BatchGetResponse  # noqa: E402
from yikv.GetRequest import (  # noqa: E402
    GetRequestAddPk,
    GetRequestAddTableName,
    GetRequestEnd,
    GetRequestStart,
)
from yikv.GetResponse import GetResponse  # noqa: E402
from yikv.Row import Row  # noqa: E402
from yikv.ValueType import ValueType  # noqa: E402


def _fb_get_request(pk: str, table: str) -> bytes:
    b = flatbuffers.Builder(256)
    pk_o = b.CreateString(pk)
    tn_o = b.CreateString(table)
    GetRequestStart(b)
    GetRequestAddPk(b, pk_o)
    GetRequestAddTableName(b, tn_o)
    root = GetRequestEnd(b)
    b.Finish(root)
    return bytes(b.Output())


def _fb_batch_get_request(pks: list[str], table: str) -> bytes:
    b = flatbuffers.Builder(4096)
    offs = [b.CreateString(p) for p in pks]
    BatchGetRequestStartPksVector(b, len(offs))
    for o in reversed(offs):
        b.PrependUOffsetTRelative(o)
    vec = b.EndVector(len(offs))
    tn_o = b.CreateString(table)
    BatchGetRequestStart(b)
    BatchGetRequestAddPks(b, vec)
    BatchGetRequestAddTableName(b, tn_o)
    root = BatchGetRequestEnd(b)
    b.Finish(root)
    return bytes(b.Output())


def _pb_encode_varint(n: int) -> bytes:
    out = bytearray()
    while n >= 0x80:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    out.append(n)
    return bytes(out)


def _pb_decode_varint(buf: bytes, pos: int) -> tuple[int, int]:
    x = 0
    shift = 0
    while pos < len(buf):
        b = buf[pos]
        pos += 1
        x |= (b & 0x7F) << shift
        if not (b & 0x80):
            return x, pos
        shift += 7
    raise ValueError("truncated protobuf varint")

def _pb_wrap_fb_request(fb_body: bytes) -> bytes:
    """Encode yikv.db.FbRpcRequest { required bytes payload = 1; } without protobuf runtime."""
    tag = (1 << 3) | 2
    return _pb_encode_varint(tag) + _pb_encode_varint(len(fb_body)) + fb_body


def _pb_unwrap_fb_response(buf: bytes) -> bytes:
    """Decode yikv.db.FbRpcResponse { optional bytes payload = 1; }; return payload bytes."""
    pos = 0
    payload = b""
    while pos < len(buf):
        key, pos = _pb_decode_varint(buf, pos)
        field = key >> 3
        wt = key & 7
        if wt == 2:
            ln, pos = _pb_decode_varint(buf, pos)
            chunk = buf[pos : pos + ln]
            pos += ln
            if field == 1:
                payload = bytes(chunk)
        elif wt == 0:
            _, pos = _pb_decode_varint(buf, pos)
        elif wt == 5:
            pos += 4
        elif wt == 1:
            pos += 8
        else:
            raise ValueError(f"unsupported protobuf wire type {wt}")
    return payload


def _grpc_unary(channel: grpc.Channel, method: str, fb_body: bytes) -> bytes:
    stub = channel.unary_unary(
        method,
        request_serializer=lambda x: x,
        response_deserializer=lambda x: x,
    )
    req_wire = _pb_wrap_fb_request(fb_body)
    resp_wire = stub(req_wire, timeout=60)
    return _pb_unwrap_fb_response(resp_wire)


def _fb_scalar_err(e) -> str:
    if e is None:
        return ""
    if isinstance(e, (bytes, bytearray)):
        return e.decode("utf-8", errors="replace")
    return str(e)


def _field_value_to_python(fv) -> object:
    vt = fv.Vtype()
    if vt == ValueType.BOOL:
        return bool(fv.I32())
    if vt == ValueType.I32:
        return fv.I32()
    if vt == ValueType.I64:
        return fv.I64()
    if vt == ValueType.F32:
        return fv.F32()
    if vt == ValueType.F64:
        return fv.F64()
    if vt == ValueType.STRING:
        s = fv.S()
        if isinstance(s, (bytes, bytearray)):
            return s.decode("utf-8", errors="replace")
        return s
    if vt == ValueType.BYTES:
        n = fv.RawLength()
        return bytes([fv.Raw(j) & 0xFF for j in range(n)]) if n else b""
    if vt == ValueType.ARR_I32:
        return [fv.Ai32(j) for j in range(fv.Ai32Length())]
    if vt == ValueType.ARR_I64:
        return [fv.Ai64(j) for j in range(fv.Ai64Length())]
    if vt == ValueType.ARR_F32:
        return [fv.Af32(j) for j in range(fv.Af32Length())]
    if vt == ValueType.ARR_F64:
        return [fv.Af64(j) for j in range(fv.Af64Length())]
    if vt == ValueType.ARR_STRING:
        return [fv.As_(j) for j in range(fv.As_Length())]
    return None


def _row_to_dict(row: Row | None, id_to_name: dict[int, str] | None) -> dict[str, object]:
    if row is None or row.FieldsLength() == 0:
        return {}
    out: dict[str, object] = {}
    for i in range(row.FieldsLength()):
        fv = row.Fields(i)
        if fv is None:
            continue
        fid = int(fv.FieldId())
        fn = fv.FieldName()
        if fn:
            key = fn.decode("utf-8") if isinstance(fn, (bytes, bytearray)) else str(fn)
        else:
            key = id_to_name.get(fid, f"field_id_{fid}") if id_to_name else f"field_id_{fid}"
        out[key] = _field_value_to_python(fv)
    return out


def _load_field_names(schema_path: Path | None) -> dict[int, str] | None:
    if not schema_path or not schema_path.is_file():
        return None
    data = json.loads(schema_path.read_text(encoding="utf-8"))
    m: dict[int, str] = {}
    for f in data.get("fields") or []:
        if "field_id" in f and "name" in f:
            m[int(f["field_id"])] = str(f["name"])
    return m


def _pks_from_parquet(path: Path, pk_column: str, limit: int) -> list[str]:
    import pyarrow.parquet as pq  # type: ignore

    t = pq.read_table(path, columns=[pk_column])
    col = t[pk_column]
    out: list[str] = []
    for v in col.to_pylist()[:limit]:
        if v is None:
            continue
        out.append(str(v))
    return out


def _pks_from_file(path: Path) -> list[str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    return [ln.strip() for ln in lines if ln.strip()]


def main() -> None:
    ap = argparse.ArgumentParser(description="yikv_server Get/BatchGet sampler over gRPC")
    ap.add_argument("--server", default=os.environ.get("YIKV_GRPC_TARGET", "127.0.0.1:9000"))
    ap.add_argument("--table", required=True, help="Logical table name")
    ap.add_argument("--get", dest="single_pk", default=None, help="Single PK Lookup (Get)")
    ap.add_argument("--keys", default=None, help="Comma-separated PKs for BatchGet")
    ap.add_argument("--keys-file", type=Path, default=None, help="One PK per line")
    ap.add_argument("--parquet", type=Path, default=None, help="Parquet file to sample PKs from")
    ap.add_argument("--pk-column", default=None, help="Parquet column name (PK)")
    ap.add_argument("--limit", type=int, default=32, help="Max PKs from parquet")
    ap.add_argument("--schema-json", type=Path, default=None, help="Map field_id -> column name in output")
    ap.add_argument("--batch-size", type=int, default=64, help="BatchGet chunk size")
    args = ap.parse_args()

    id_to_name = _load_field_names(args.schema_json)

    pks: list[str] = []
    if args.single_pk:
        pks = [args.single_pk]
    elif args.keys:
        pks = [s.strip() for s in args.keys.split(",") if s.strip()]
    elif args.keys_file:
        pks = _pks_from_file(args.keys_file)
    elif args.parquet:
        if not args.pk_column:
            ap.error("--pk-column required with --parquet")
        pks = _pks_from_parquet(args.parquet, args.pk_column, args.limit)
    else:
        ap.error("provide one of --get, --keys, --keys-file, or --parquet")

    ch = grpc.insecure_channel(args.server)

    if len(pks) == 1 and args.single_pk is not None:
        fb = _fb_get_request(pks[0], args.table)
        pl = _grpc_unary(ch, "/yikv.db.YikvDb/Get", fb)
        gr = GetResponse.GetRootAsGetResponse(pl, 0)
        if gr.Err():
            print(json.dumps({"ok": False, "err": _fb_scalar_err(gr.Err())}))
            sys.exit(2)
        row_d = _row_to_dict(gr.Row(), id_to_name)
        print(json.dumps({"ok": bool(gr.Found()), "pk": pks[0], "row": row_d, "index_get_ns": int(gr.IndexGetNs())}, ensure_ascii=False))
        return

    # BatchGet in chunks
    batch_size = max(1, int(args.batch_size))
    for i in range(0, len(pks), batch_size):
        chunk = pks[i : i + batch_size]
        fb = _fb_batch_get_request(chunk, args.table)
        pl = _grpc_unary(ch, "/yikv.db.YikvDb/BatchGet", fb)
        br = BatchGetResponse.GetRootAsBatchGetResponse(pl, 0)
        if br.Err():
            print(json.dumps({"ok": False, "err": _fb_scalar_err(br.Err()), "chunk_start": i}))
            sys.exit(3)
        if br.RowsLength() != len(chunk):
            print(
                json.dumps(
                    {
                        "ok": False,
                        "err": f"rows length {br.RowsLength()} != pks {len(chunk)}",
                        "chunk_start": i,
                    }
                )
            )
            sys.exit(4)
        for j, pk in enumerate(chunk):
            row = br.Rows(j)
            empty = row is None or row.FieldsLength() == 0
            row_d = _row_to_dict(row, id_to_name)
            print(
                json.dumps(
                    {"ok": not empty, "pk": pk, "row": row_d},
                    ensure_ascii=False,
                )
            )


if __name__ == "__main__":
    main()

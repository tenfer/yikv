# yikv Java client (`io.yikv`)

gRPC client for `yikv.db.YikvDb`: requests and responses carry FlatBuffers root tables inside
`FbRpcRequest` / `FbRpcResponse` (see `proto/yikv_grpc.proto`).

## Build

Run Maven from a working copy where `client/java/../../proto` exists (repository root).

```bash
mvn -f client/java/pom.xml test
```

**Docker:** bind-mount the **repository root**, not only `client/java`, so `../../proto` resolves inside the container.

```bash
docker run --rm -v "$(pwd):/repo" -w /repo/client/java maven:3.9.6-eclipse-temurin-17 mvn test
```

Override the `.proto` directory if needed:

```bash
mvn -f client/java/pom.xml -Dyikv.proto.dir=/abs/path/to/proto test
```

## FlatBuffers Java (`yikv.*` types)

The `src/main/java/yikv/*.java` tree is generated from `proto/yikv_server.fbs` (`flatc --java`). It is
checked in so the module builds without the `flatc` binary. After schema changes, regenerate and replace
that tree, for example:

```bash
flatc --java -o /tmp/yikv_fb_java proto/yikv_server.fbs
# copy generated files into client/java/src/main/java/yikv/
```

## Usage

- **`YikvDbClient`** – interface; **`GrpcYikvDbClient`** – Netty gRPC implementation.
- Pass **serialized FlatBuffers request roots** as `byte[]` (`GetRequest`, `BatchGetRequest`, `PutRequest`,
  `PutBatchRequest`), same as `tools/yikv_db_sample.py`.
- **Decoded responses:** `get` → `GetResult` (`found`, `err`, `indexGetNs`, `row` as `Map<String, Object>`);
  `batchGet` → `BatchGetResult`; `put` / `putBatch` → `PutResult` / `PutBatchResult`.
- **Raw read path:** `getRaw` / `batchGetRaw` return the FlatBuffers **response** bytes only (no
  `FieldValue` decoding). Callers own error/table semantics on the wire.
- **Column keys:** prefer `field_name` on each `FieldValue`; otherwise `ClientOptions.fieldIdToName`
  (like `--schema-json` in the Python tool) or fallback `field_id_<id>` (same as `yikv_db_sample.py`).

### Multiple endpoints (round robin)

By default, several `host:port` values use one channel with target `static://h1:p1,h2:p2` and
`round_robin`. If that is unsuitable in your environment, use:

```java
ClientOptions.builder().perAddressChannels(true).build();
```

to open one channel per address and round-robin **stubs** per RPC.

## Optional smoke test

With a reachable server (channel only; no RPC required):

```bash
export YIKV_GRPC_TARGET=127.0.0.1:9000
mvn -f client/java/pom.xml test
```

Enables `GrpcSmokeOptionalTest` via JUnit’s `@EnabledIfEnvironmentVariable`.
## Verify deployed `product` data (E2E)

After publish + deploy of `product` Parquet (e.g. `product100k.parquet`), check rows against the same
rules as `deploy/e2e/scripts/generate_product_parquet.py`:

```bash
kubectl port-forward -n yikv svc/yikv-server 9000:9000
export YIKV_GRPC_TARGET=127.0.0.1:9000
mvn -f client/java/pom.xml test -Dtest=ProductDataVerifyIT
```

`ProductDataVerifyIT` asserts `id`, `brand_id`, `click_num`, `title`, `category_ids` on sample PKs.


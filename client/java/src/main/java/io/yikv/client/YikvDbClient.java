package io.yikv.client;

import java.io.Closeable;

/**
 * yikv DB gRPC facade: FlatBuffers request bodies (caller-built) and decoded FlatBuffers responses.
 */
public interface YikvDbClient extends Closeable {

  GetResult get(byte[] flatBufferRequest);

  BatchGetResult batchGet(byte[] flatBufferRequest);

  PutResult put(byte[] flatBufferRequest);

  PutBatchResult putBatch(byte[] flatBufferRequest);

  /**
   * Returns the raw FlatBuffers response root bytes from {@link yikv.db.YikvGrpc.FbRpcResponse} (no
   * {@code FieldValue} decoding).
   */
  byte[] getRaw(byte[] flatBufferRequest);

  /** Same semantics as {@link #getRaw(byte[])} for {@code BatchGet}. */
  byte[] batchGetRaw(byte[] flatBufferRequest);

  @Override
  void close();
}

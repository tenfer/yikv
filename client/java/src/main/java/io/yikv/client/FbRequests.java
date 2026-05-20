package io.yikv.client;

import com.google.flatbuffers.FlatBufferBuilder;
import java.util.List;
import yikv.BatchGetRequest;
import yikv.GetRequest;

public final class FbRequests {

  private FbRequests() {}

  public static byte[] get(String pk, String table) {
    FlatBufferBuilder b = new FlatBufferBuilder(256);
    int pkOff = b.createString(pk);
    int tableOff = b.createString(table);
    int root = GetRequest.createGetRequest(b, pkOff, tableOff);
    b.finish(root);
    return b.sizedByteArray();
  }

  public static byte[] batchGet(List<String> pks, String table) {
    FlatBufferBuilder b = new FlatBufferBuilder(Math.max(256, pks.size() * 32));
    int[] pkOffs = new int[pks.size()];
    for (int i = 0; i < pks.size(); i++) {
      pkOffs[i] = b.createString(pks.get(i));
    }
    int vec = BatchGetRequest.createPksVector(b, pkOffs);
    int tableOff = b.createString(table);
    int root = BatchGetRequest.createBatchGetRequest(b, vec, tableOff);
    b.finish(root);
    return b.sizedByteArray();
  }
}

package io.yikv.client;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import java.util.Map;
import java.util.HashMap;
import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.condition.EnabledIfEnvironmentVariable;

@EnabledIfEnvironmentVariable(named = "YIKV_GRPC_TARGET", matches = ".+")
class ProductDataVerifyIT {

  private static final String TABLE =
      System.getenv().getOrDefault("YIKV_TABLE", ProductFixture.TABLE);

  private static YikvDbClient client;

  @BeforeAll
  static void connect() {
    Map<Integer, String> fieldIds = new HashMap<>();
    fieldIds.put(0, "id");
    fieldIds.put(1, "brand_id");
    fieldIds.put(2, "click_num");
    fieldIds.put(3, "title");
    fieldIds.put(4, "category_ids");
    client =
        GrpcYikvDbClient.create(
            System.getenv("YIKV_GRPC_TARGET").trim(),
            ClientOptions.builder().fieldIdToName(fieldIds).build());
  }

  @AfterAll
  static void close() {
    if (client != null) {
      client.close();
    }
  }

  private static void assertNoErr(String err) {
    assertTrue(err == null || err.isBlank(), () -> "server err=" + err);
  }

  @Test
  void getFirstMiddleLastRows() {
    for (int index : new int[] {0, 1, 49_999, 99_999}) {
      String pk = ProductFixture.pkForIndex(index);
      GetResult r = client.get(FbRequests.get(pk, TABLE));
      assertNoErr(r.err());
      assertTrue(r.found(), () -> "pk=" + pk + " not found");
      ProductFixture.assertRowMatches(index, r.row());
    }
  }

  @Test
  void batchGetSampleRows() {
    List<String> pks =
        List.of(
            ProductFixture.pkForIndex(0),
            ProductFixture.pkForIndex(123),
            ProductFixture.pkForIndex(50_000),
            ProductFixture.pkForIndex(99_999));
    BatchGetResult br = client.batchGet(FbRequests.batchGet(pks, TABLE));
    assertNoErr(br.err());
    assertEquals(pks.size(), br.rows().size());
    record Sample(int index, String pk) {}
    List<Sample> samples =
        List.of(
            new Sample(0, pks.get(0)),
            new Sample(123, pks.get(1)),
            new Sample(50_000, pks.get(2)),
            new Sample(99_999, pks.get(3)));
    for (int i = 0; i < samples.size(); i++) {
      Sample s = samples.get(i);
      assertFalse(br.rows().get(i).isEmpty(), () -> "miss pk=" + s.pk());
      ProductFixture.assertRowMatches(s.index(), br.rows().get(i));
    }
  }

  @Test
  void missingPkNotFound() {
    GetResult r = client.get(FbRequests.get("999999", TABLE));
    assertNoErr(r.err());
    assertFalse(r.found());
  }
}

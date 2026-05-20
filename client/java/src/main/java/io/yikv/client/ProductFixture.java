package io.yikv.client;

import java.util.HashMap;
import java.util.Map;

/** Expected product rows from generate_product_parquet.py (0-based row index). */
public final class ProductFixture {

  public static final String TABLE = "product";

  private ProductFixture() {}

  public static long idForIndex(int index) {
    return (long) index + 1L;
  }

  public static String pkForIndex(int index) {
    return Long.toString(idForIndex(index));
  }

  public static String titleAt(int index) {
    String s = String.format("pd%05da%d", index % 50_000, (index * 7) % 10_000);
    if (s.length() < 8) {
      s = s + "xy";
    }
    return s.length() <= 50 ? s : s.substring(0, 50);
  }

  public static String categoryAt(int index) {
    int a = (index % 98) + 1;
    int b = ((index / 100) % 98) + 1;
    if (index % 3 == 0) {
      return Integer.toString(a);
    }
    return a + "," + b;
  }

  public static Map<String, Object> expectedRow(int index) {
    long idx = index;
    Map<String, Object> row = new HashMap<>();
    row.put("id", idForIndex(index));
    row.put("brand_id", (int) ((idx * 17) % 1000 + 1));
    row.put("click_num", (int) ((idx * 13) % 1_000_000));
    row.put("title", titleAt(index));
    row.put("category_ids", categoryAt(index));
    return Map.copyOf(row);
  }

  public static void assertRowMatches(int index, Map<String, Object> actual) {
    Map<String, Object> expected = expectedRow(index);
    for (Map.Entry<String, Object> e : expected.entrySet()) {
      Object got = actual.get(e.getKey());
      if (got == null && !actual.containsKey(e.getKey())) {
        throw new AssertionError(
            "index=" + index + " pk=" + pkForIndex(index) + " missing field " + e.getKey());
      }
      if (!valuesEqual(e.getValue(), got)) {
        throw new AssertionError(
            "index="
                + index
                + " pk="
                + pkForIndex(index)
                + " field "
                + e.getKey()
                + " expected "
                + e.getValue()
                + " got "
                + got);
      }
    }
  }

  private static boolean valuesEqual(Object expected, Object got) {
    if (expected instanceof Number en && got instanceof Number gn) {
      return en.longValue() == gn.longValue();
    }
    return expected.equals(got);
  }
}

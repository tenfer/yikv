package io.yikv.client;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import yikv.FieldValue;
import yikv.Row;
import yikv.ValueType;

/**
 * Maps {@link yikv.FieldValue} to Java values and {@link yikv.Row} to column maps. Column keys match
 * {@code tools/yikv_db_sample.py}: prefer {@code field_name} on the wire; otherwise {@code
 * fieldIdToName.get(id)} or {@code field_id_<id>}.
 */
public final class FieldValues {

  private FieldValues() {}

  public static Map<String, Object> rowToMap(
      Row row, Map<Integer, String> fieldIdToName) {
    if (row == null || row.fieldsLength() == 0) {
      return Collections.emptyMap();
    }
    Map<String, Object> out = new HashMap<>();
    FieldValue fv = new FieldValue();
    for (int i = 0; i < row.fieldsLength(); i++) {
      row.fields(fv, i);
      String key = columnKey(fv, fieldIdToName);
      out.put(key, toJavaValue(fv));
    }
    return out;
  }

  static String columnKey(FieldValue fv, Map<Integer, String> fieldIdToName) {
    String name = fv.fieldName();
    if (name != null && !name.isEmpty()) {
      return name;
    }
    int fid = fv.fieldId();
    if (fieldIdToName != null) {
      String mapped = fieldIdToName.get(fid);
      if (mapped != null) {
        return mapped;
      }
    }
    return "field_id_" + fid;
  }

  static Object toJavaValue(FieldValue fv) {
    int vt = fv.vtype();
    return switch (vt) {
      case ValueType.BOOL -> fv.i32() != 0;
      case ValueType.I32 -> fv.i32();
      case ValueType.I64 -> fv.i64();
      case ValueType.F32 -> fv.f32();
      case ValueType.F64 -> fv.f64();
      case ValueType.STRING -> fv.s();
      case ValueType.BYTES -> copyRaw(fv);
      case ValueType.ARR_I32 -> copyAi32(fv);
      case ValueType.ARR_I64 -> copyAi64(fv);
      case ValueType.ARR_F32 -> copyAf32(fv);
      case ValueType.ARR_F64 -> copyAf64(fv);
      case ValueType.ARR_STRING -> copyAs(fv);
      default -> null;
    };
  }

  private static byte[] copyRaw(FieldValue fv) {
    int n = fv.rawLength();
    if (n == 0) {
      return new byte[0];
    }
    byte[] out = new byte[n];
    for (int j = 0; j < n; j++) {
      out[j] = (byte) (fv.raw(j) & 0xFF);
    }
    return out;
  }

  private static List<Integer> copyAi32(FieldValue fv) {
    List<Integer> list = new ArrayList<>(fv.ai32Length());
    for (int j = 0; j < fv.ai32Length(); j++) {
      list.add(fv.ai32(j));
    }
    return list;
  }

  private static List<Long> copyAi64(FieldValue fv) {
    List<Long> list = new ArrayList<>(fv.ai64Length());
    for (int j = 0; j < fv.ai64Length(); j++) {
      list.add(fv.ai64(j));
    }
    return list;
  }

  private static List<Float> copyAf32(FieldValue fv) {
    List<Float> list = new ArrayList<>(fv.af32Length());
    for (int j = 0; j < fv.af32Length(); j++) {
      list.add(fv.af32(j));
    }
    return list;
  }

  private static List<Double> copyAf64(FieldValue fv) {
    List<Double> list = new ArrayList<>(fv.af64Length());
    for (int j = 0; j < fv.af64Length(); j++) {
      list.add(fv.af64(j));
    }
    return list;
  }

  private static List<String> copyAs(FieldValue fv) {
    List<String> list = new ArrayList<>(fv.asLength());
    for (int j = 0; j < fv.asLength(); j++) {
      list.add(fv.as(j));
    }
    return list;
  }

  /** Wrap serialized FlatBuffers root table bytes as a little-endian buffer (position 0). */
  public static ByteBuffer asLittleEndian(byte[] payload) {
    ByteBuffer bb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
    return bb;
  }
}

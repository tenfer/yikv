package io.yikv.client;

import java.util.List;
import java.util.Map;

/** Decoded {@link yikv.BatchGetResponse}. */
public record BatchGetResult(String err, List<Map<String, Object>> rows) {}

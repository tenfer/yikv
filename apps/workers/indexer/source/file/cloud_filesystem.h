#pragma once

#include <arrow/filesystem/filesystem.h>
#include <arrow/result.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace yikv::indexer {

/// Holds optional Arrow filesystem backends for remote object URIs.
struct CloudFileSystems {
    std::shared_ptr<arrow::fs::FileSystem> oss;  ///< oss:// — Aliyun OSS (S3-compatible)
    std::shared_ptr<arrow::fs::FileSystem> s3;   ///< s3:// — AWS S3
    std::shared_ptr<arrow::fs::FileSystem> cos; ///< cos:// — Tencent COS (S3-compatible)
    std::shared_ptr<arrow::fs::FileSystem> obs; ///< obs:// — Huawei OBS (S3-compatible)
    std::shared_ptr<arrow::fs::FileSystem> gcs;  ///< gs:// — Google Cloud Storage

    [[nodiscard]] arrow::fs::FileSystem* FsForScheme(std::string_view scheme) const;
};

[[nodiscard]] bool IsCloudUri(std::string_view uri);

/// Parses oss://, s3://, cos://, obs://, gs://. @p scheme: oss|s3|cos|obs|gs. Key may be empty.
[[nodiscard]] bool ParseCloudUri(std::string_view uri, std::string* scheme, std::string* bucket,
                                 std::string* key);

/// Aliyun OSS (S3-compatible) — same env as before: OSS_ENDPOINT, OSS_ACCESS_KEY_ID,
/// OSS_ACCESS_KEY_SECRET, optional OSS_REGION.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::fs::FileSystem>> MakeOssFileSystem();

/// Native AWS S3 via Arrow (S3Options::Defaults() — standard AWS env / profile chain).
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::fs::FileSystem>> MakeAwsS3FileSystem();

/// Tencent COS (S3-compatible): COS_SECRET_ID, COS_SECRET_KEY;
/// COS_ENDPOINT (e.g. cos.ap-beijing.myqcloud.com) or COS_REGION (builds cos.<region>.myqcloud.com).
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::fs::FileSystem>> MakeTencentCosFileSystem();

/// Huawei OBS (S3-compatible): OBS_ACCESS_KEY_ID, OBS_SECRET_ACCESS_KEY, OBS_ENDPOINT
/// (e.g. obs.cn-south-1.myhuaweicloud.com); optional OBS_REGION for signing.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::fs::FileSystem>> MakeHuaweiObsFileSystem();

/// GCS via Application Default Credentials (e.g. GOOGLE_APPLICATION_CREDENTIALS).
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::fs::FileSystem>> MakeGcsFileSystem();

/// Like ExpandOssInputs but for all schemes; prefix entries must end with '/'.
[[nodiscard]] arrow::Status ExpandCloudInputs(const CloudFileSystems& cfs, std::vector<std::string>* inputs);

}  // namespace yikv::indexer

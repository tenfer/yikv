#include "indexer/source/file/cloud_filesystem.h"

#include <arrow/result.h>
#include <arrow/util/config.h>

#ifdef ARROW_GCS
#include <arrow/filesystem/gcsfs.h>
#endif
#ifdef ARROW_S3
#include <arrow/filesystem/s3fs.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

namespace yikv::indexer {
namespace {

#ifdef ARROW_S3
std::string GetEnvTrimmed(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return {};
    std::string s(v);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i) s.erase(0, i);
    return s;
}

std::string StripSchemeHost(std::string endpoint) {
    if (endpoint.rfind("https://", 0) == 0) endpoint.erase(0, 8);
    else if (endpoint.rfind("http://", 0) == 0) endpoint.erase(0, 7);
    while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
    return endpoint;
}

bool CosRegionFromEndpoint(std::string_view endpoint, std::string* region) {
    constexpr std::string_view pre = "cos.";
    constexpr std::string_view suf = ".myqcloud.com";
    if (endpoint.size() <= pre.size() + suf.size()) return false;
    if (endpoint.rfind(pre, 0) != 0) return false;
    if (endpoint.size() < suf.size() || endpoint.substr(endpoint.size() - suf.size()) != suf) return false;
    *region = std::string(endpoint.substr(pre.size(), endpoint.size() - pre.size() - suf.size()));
    return !region->empty();
}

bool ObsRegionFromEndpoint(std::string_view endpoint, std::string* region) {
    constexpr std::string_view pre = "obs.";
    constexpr std::string_view suf = ".myhuaweicloud.com";
    if (endpoint.size() <= pre.size() + suf.size()) return false;
    if (endpoint.rfind(pre, 0) != 0) return false;
    if (endpoint.size() < suf.size() || endpoint.substr(endpoint.size() - suf.size()) != suf) return false;
    *region = std::string(endpoint.substr(pre.size(), endpoint.size() - pre.size() - suf.size()));
    return !region->empty();
}

#endif  // ARROW_S3

bool EndsWithSlash(std::string_view s) { return !s.empty() && s.back() == '/'; }

bool EndsWithIgnoreCase(std::string_view name, std::string_view ext) {
    if (name.size() < ext.size()) return false;
    for (size_t i = 0; i < ext.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(name[name.size() - ext.size() + i])));
        const char b = ext[i];
        if (a != b) return false;
    }
    return true;
}

bool IsDataObjectPath(std::string_view path) {
    const size_t slash = path.rfind('/');
    const std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
    if (base.empty()) return false;
    return EndsWithIgnoreCase(base, ".parquet") || EndsWithIgnoreCase(base, ".csv");
}

}  // namespace

arrow::fs::FileSystem* CloudFileSystems::FsForScheme(std::string_view scheme) const {
    if (scheme == "oss") return oss.get();
    if (scheme == "s3") return s3.get();
    if (scheme == "cos") return cos.get();
    if (scheme == "obs") return obs.get();
    if (scheme == "gs") return gcs.get();
    return nullptr;
}

bool IsCloudUri(std::string_view uri) {
    return (uri.size() >= 6 && uri.rfind("oss://", 0) == 0) || (uri.size() >= 5 && uri.rfind("s3://", 0) == 0)
        || (uri.size() >= 5 && uri.rfind("gs://", 0) == 0) || (uri.size() >= 6 && uri.rfind("cos://", 0) == 0)
        || (uri.size() >= 6 && uri.rfind("obs://", 0) == 0);
}

bool ParseCloudUri(std::string_view uri, std::string* scheme, std::string* bucket, std::string* key) {
    const size_t p = uri.find("://");
    if (p == std::string_view::npos) return false;
    *scheme = std::string(uri.substr(0, p));
    if (*scheme != "oss" && *scheme != "s3" && *scheme != "gs" && *scheme != "cos" && *scheme != "obs")
        return false;
    std::string_view rest = uri.substr(p + 3);
    if (rest.empty()) return false;
    const size_t slash = rest.find('/');
    if (slash == std::string_view::npos) {
        *bucket = std::string(rest);
        *key    = "";
        return !bucket->empty();
    }
    *bucket = std::string(rest.substr(0, slash));
    *key    = std::string(rest.substr(slash + 1));
    return !bucket->empty();
}

arrow::Result<std::shared_ptr<arrow::fs::FileSystem>> MakeOssFileSystem() {
#ifdef ARROW_S3
    std::string endpoint = StripSchemeHost(GetEnvTrimmed("OSS_ENDPOINT"));
    std::string key_id   = GetEnvTrimmed("OSS_ACCESS_KEY_ID");
    std::string secret   = GetEnvTrimmed("OSS_ACCESS_KEY_SECRET");
    if (endpoint.empty()) {
        return arrow::Status::Invalid("OSS_ENDPOINT is not set (required for oss:// inputs)");
    }
    if (key_id.empty() || secret.empty()) {
        return arrow::Status::Invalid("OSS_ACCESS_KEY_ID and OSS_ACCESS_KEY_SECRET are required for oss:// inputs");
    }
    arrow::fs::S3Options opts = arrow::fs::S3Options::FromAccessKey(key_id, secret);
    opts.endpoint_override        = std::move(endpoint);
    opts.scheme                   = "https";
    opts.force_virtual_addressing = true;
    std::string region = GetEnvTrimmed("OSS_REGION");
    if (region.empty()) region = "cn-hangzhou";
    opts.region = region;
    ARROW_ASSIGN_OR_RAISE(auto s3fs, arrow::fs::S3FileSystem::Make(opts));
    return std::static_pointer_cast<arrow::fs::FileSystem>(std::move(s3fs));
#else
    return arrow::Status::Invalid(
        "oss:// requires Apache Arrow built with S3 support (ARROW_S3); this Arrow install does not include it");
#endif
}

arrow::Result<std::shared_ptr<arrow::fs::FileSystem>> MakeAwsS3FileSystem() {
#ifdef ARROW_S3
    const std::string ak = GetEnvTrimmed("AWS_ACCESS_KEY_ID");
    const std::string sk = GetEnvTrimmed("AWS_SECRET_ACCESS_KEY");
    arrow::fs::S3Options opts =
        (!ak.empty() && !sk.empty()) ? arrow::fs::S3Options::FromAccessKey(ak, sk) : arrow::fs::S3Options::Defaults();

    std::string endpoint = StripSchemeHost(GetEnvTrimmed("AWS_ENDPOINT_URL_S3"));
    if (endpoint.empty()) endpoint = StripSchemeHost(GetEnvTrimmed("AWS_ENDPOINT_URL"));
    if (!endpoint.empty()) {
        opts.endpoint_override = std::move(endpoint);
        const char* ep_raw = std::getenv("AWS_ENDPOINT_URL_S3");
        if (!ep_raw) ep_raw = std::getenv("AWS_ENDPOINT_URL");
        if (ep_raw && std::string_view(ep_raw).rfind("https://", 0) == 0)
            opts.scheme = "https";
        else
            opts.scheme = "http";
        opts.force_virtual_addressing = false;
    }

    std::string region = GetEnvTrimmed("AWS_REGION");
    if (region.empty()) region = GetEnvTrimmed("AWS_DEFAULT_REGION");
    if (!region.empty()) opts.region = std::move(region);

    ARROW_ASSIGN_OR_RAISE(auto s3fs, arrow::fs::S3FileSystem::Make(opts));
    return std::static_pointer_cast<arrow::fs::FileSystem>(std::move(s3fs));
#else
    return arrow::Status::Invalid(
        "s3:// requires Apache Arrow built with S3 support (ARROW_S3); this Arrow install does not include it");
#endif
}

arrow::Result<std::shared_ptr<arrow::fs::FileSystem>> MakeTencentCosFileSystem() {
#ifdef ARROW_S3
    std::string endpoint   = StripSchemeHost(GetEnvTrimmed("COS_ENDPOINT"));
    std::string region_cfg = GetEnvTrimmed("COS_REGION");
    if (endpoint.empty() && !region_cfg.empty()) {
        endpoint = "cos." + region_cfg + ".myqcloud.com";
    }
    if (endpoint.empty()) {
        return arrow::Status::Invalid("COS_ENDPOINT or COS_REGION is required for cos:// inputs");
    }
    std::string region = region_cfg;
    if (region.empty()) {
        if (!CosRegionFromEndpoint(endpoint, &region)) region = "ap-beijing";
    }
    const std::string sid  = GetEnvTrimmed("COS_SECRET_ID");
    const std::string skey = GetEnvTrimmed("COS_SECRET_KEY");
    if (sid.empty() || skey.empty()) {
        return arrow::Status::Invalid("COS_SECRET_ID and COS_SECRET_KEY are required for cos:// inputs");
    }
    arrow::fs::S3Options opts = arrow::fs::S3Options::FromAccessKey(sid, skey);
    opts.endpoint_override        = std::move(endpoint);
    opts.scheme                   = "https";
    opts.force_virtual_addressing = true;
    opts.region                   = std::move(region);
    ARROW_ASSIGN_OR_RAISE(auto s3fs, arrow::fs::S3FileSystem::Make(opts));
    return std::static_pointer_cast<arrow::fs::FileSystem>(std::move(s3fs));
#else
    return arrow::Status::Invalid(
        "cos:// requires Apache Arrow built with S3 support (ARROW_S3); this Arrow install does not include it");
#endif
}

arrow::Result<std::shared_ptr<arrow::fs::FileSystem>> MakeHuaweiObsFileSystem() {
#ifdef ARROW_S3
    std::string endpoint = StripSchemeHost(GetEnvTrimmed("OBS_ENDPOINT"));
    if (endpoint.empty()) {
        return arrow::Status::Invalid("OBS_ENDPOINT is required for obs:// inputs");
    }
    std::string region = GetEnvTrimmed("OBS_REGION");
    if (region.empty()) {
        if (!ObsRegionFromEndpoint(endpoint, &region)) region = "cn-south-1";
    }
    const std::string ak = GetEnvTrimmed("OBS_ACCESS_KEY_ID");
    const std::string sk = GetEnvTrimmed("OBS_SECRET_ACCESS_KEY");
    if (ak.empty() || sk.empty()) {
        return arrow::Status::Invalid(
            "OBS_ACCESS_KEY_ID and OBS_SECRET_ACCESS_KEY are required for obs:// inputs");
    }
    arrow::fs::S3Options opts = arrow::fs::S3Options::FromAccessKey(ak, sk);
    opts.endpoint_override        = std::move(endpoint);
    opts.scheme                   = "https";
    opts.force_virtual_addressing = true;
    opts.region                   = std::move(region);
    ARROW_ASSIGN_OR_RAISE(auto s3fs, arrow::fs::S3FileSystem::Make(opts));
    return std::static_pointer_cast<arrow::fs::FileSystem>(std::move(s3fs));
#else
    return arrow::Status::Invalid(
        "obs:// requires Apache Arrow built with S3 support (ARROW_S3); this Arrow install does not include it");
#endif
}

arrow::Result<std::shared_ptr<arrow::fs::FileSystem>> MakeGcsFileSystem() {
#ifdef ARROW_GCS
    ARROW_ASSIGN_OR_RAISE(auto gcsfs, arrow::fs::GcsFileSystem::Make(arrow::fs::GcsOptions::Defaults()));
    return std::static_pointer_cast<arrow::fs::FileSystem>(std::move(gcsfs));
#else
    return arrow::Status::Invalid(
        "gs:// requires Apache Arrow built with GCS support (ARROW_GCS); this Arrow install does not include it");
#endif
}

arrow::Status ExpandCloudInputs(const CloudFileSystems& cfs, std::vector<std::string>* inputs) {
    std::vector<std::string> in = std::move(*inputs);
    std::vector<std::string> out;
    for (const std::string& e : in) {
        if (!IsCloudUri(e)) {
            out.push_back(e);
            continue;
        }
        std::string scheme;
        std::string bucket;
        std::string key;
        if (!ParseCloudUri(e, &scheme, &bucket, &key)) {
            return arrow::Status::Invalid("invalid cloud URI: ", e);
        }
        arrow::fs::FileSystem* fs = cfs.FsForScheme(scheme);
        if (!fs) {
            return arrow::Status::Invalid("no filesystem configured for scheme ", scheme, " (URI: ", e, ")");
        }
        if (!EndsWithSlash(e)) {
            out.push_back(e);
            continue;
        }
        while (!key.empty() && key.back() == '/') key.pop_back();
        const std::string base_dir = key.empty() ? bucket : (bucket + "/" + key);
        arrow::fs::FileSelector sel;
        sel.base_dir        = base_dir;
        sel.recursive       = true;
        sel.allow_not_found = false;
        ARROW_ASSIGN_OR_RAISE(auto infos, fs->GetFileInfo(sel));
        std::vector<std::string> keys;
        for (const auto& info : infos) {
            if (!info.IsFile()) continue;
            if (!IsDataObjectPath(info.path())) continue;
            keys.push_back(scheme + "://" + info.path());
        }
        if (keys.empty()) {
            return arrow::Status::Invalid("no .parquet or .csv objects under cloud prefix: ", e);
        }
        std::sort(keys.begin(), keys.end());
        for (auto& k : keys) out.push_back(std::move(k));
    }
    *inputs = std::move(out);
    return arrow::Status::OK();
}

}  // namespace yikv::indexer

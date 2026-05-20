#include "indexer/source/file/file_source.h"

#include <arrow/csv/api.h>
#include <arrow/filesystem/filesystem.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>

#include <cctype>
#include <filesystem>
#include <string_view>

namespace yikv::indexer {
namespace {

namespace fs = std::filesystem;

static bool EndsWithIgnoreCase(std::string_view ext_with_dot, const fs::path& p) {
    std::string ext = p.extension().string();
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ext == ext_with_dot;
}

static bool EndsWithIgnoreCaseStr(std::string_view s, std::string_view suf) {
    if (s.size() < suf.size()) return false;
    for (size_t i = 0; i < suf.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - suf.size() + i])));
        if (a != suf[i]) return false;
    }
    return true;
}

static arrow::Status BuildCsvConvertOptions(const yikv::schema::Schema& sch, arrow::csv::ConvertOptions* out) {
    *out                         = arrow::csv::ConvertOptions::Defaults();
    out->include_columns.clear();
    out->column_types.clear();
    for (const auto& fp : sch.fields()) {
        const yikv::schema::FieldDef* def = fp.get();
        if (def->is_array) {
            return arrow::Status::Invalid("CSV import does not support array fields (", def->name, ")");
        }
        out->include_columns.push_back(def->name);
        switch (def->type) {
            case yikv::schema::DataType::Bool:
                out->column_types[def->name] = arrow::boolean();
                break;
            case yikv::schema::DataType::Int32:
                out->column_types[def->name] = arrow::int32();
                break;
            case yikv::schema::DataType::Int64:
                out->column_types[def->name] = arrow::int64();
                break;
            case yikv::schema::DataType::Float32:
                out->column_types[def->name] = arrow::float32();
                break;
            case yikv::schema::DataType::Float64:
                out->column_types[def->name] = arrow::float64();
                break;
            case yikv::schema::DataType::String:
            case yikv::schema::DataType::Bytes:
                out->column_types[def->name] = arrow::utf8();
                break;
            default:
                return arrow::Status::Invalid("CSV import: unsupported type for field: ", def->name);
        }
    }
    return arrow::Status::OK();
}

static arrow::Status ValidateBatchColumns(const arrow::RecordBatch& batch, const yikv::schema::Schema& sch) {
    for (const auto& fp : sch.fields()) {
        const yikv::schema::FieldDef* def = fp.get();
        if (!batch.GetColumnByName(def->name)) {
            return arrow::Status::Invalid("parquet missing column: ", def->name);
        }
    }
    return arrow::Status::OK();
}

arrow::Status ReadParquetFromRandomAccess(const std::string& display_path, size_t file_index_1based,
                                          const std::shared_ptr<arrow::io::RandomAccessFile>& infile,
                                          const yikv::schema::Schema& sch, BoundedParsedBatchQueue& q,
                                          ErrorState& err) {
    auto reader_res = parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
    if (!reader_res.ok()) {
        err.Set(reader_res.status());
        q.Abort();
        return reader_res.status();
    }
    auto rb_reader_res = (*reader_res)->GetRecordBatchReader();
    if (!rb_reader_res.ok()) {
        err.Set(rb_reader_res.status());
        q.Abort();
        return rb_reader_res.status();
    }
    auto rb_reader = std::move(*rb_reader_res);

    while (!err.failed.load()) {
        auto batch_res = rb_reader->Next();
        if (!batch_res.ok()) {
            err.Set(batch_res.status());
            q.Abort();
            return batch_res.status();
        }
        auto batch = *batch_res;
        if (!batch) break;
        err.Set(ValidateBatchColumns(*batch, sch));
        if (err.failed.load()) {
            q.Abort();
            return arrow::Status::Invalid("validation failed");
        }
        ParsedBatch pb;
        pb.batch             = std::move(batch);
        pb.path              = display_path;
        pb.file_index_1based = file_index_1based;
        q.Push(std::move(pb));
    }
    return arrow::Status::OK();
}

arrow::Status ReadParquetFileBatches(const std::string& path, size_t file_index_1based,
                                     const yikv::schema::Schema& sch, BoundedParsedBatchQueue& q,
                                     ErrorState& err) {
    auto infile_res = arrow::io::ReadableFile::Open(path);
    if (!infile_res.ok()) {
        err.Set(infile_res.status());
        q.Abort();
        return infile_res.status();
    }
    return ReadParquetFromRandomAccess(path, file_index_1based, *infile_res, sch, q, err);
}

arrow::Status ReadCsvFromInputStream(const std::string& display_path, size_t file_index_1based,
                                     const std::shared_ptr<arrow::io::InputStream>& input_stream,
                                     const yikv::schema::Schema& sch, BoundedParsedBatchQueue& q,
                                     ErrorState& err) {
    arrow::csv::ConvertOptions convert_options;
    {
        const auto st = BuildCsvConvertOptions(sch, &convert_options);
        if (!st.ok()) {
            err.Set(st);
            q.Abort();
            return st;
        }
    }

    arrow::csv::ReadOptions read_options   = arrow::csv::ReadOptions::Defaults();
    read_options.autogenerate_column_names = false;
    arrow::csv::ParseOptions parse_options = arrow::csv::ParseOptions::Defaults();

    auto reader_res = arrow::csv::StreamingReader::Make(arrow::io::default_io_context(), input_stream, read_options,
                                                          parse_options, convert_options);
    if (!reader_res.ok()) {
        err.Set(reader_res.status());
        q.Abort();
        return reader_res.status();
    }
    auto reader = std::move(*reader_res);

    while (!err.failed.load()) {
        auto batch_res = reader->Next();
        if (!batch_res.ok()) {
            err.Set(batch_res.status());
            q.Abort();
            return batch_res.status();
        }
        auto batch = *batch_res;
        if (!batch) break;
        err.Set(ValidateBatchColumns(*batch, sch));
        if (err.failed.load()) {
            q.Abort();
            return arrow::Status::Invalid("validation failed");
        }
        ParsedBatch pb;
        pb.batch             = std::move(batch);
        pb.path              = display_path;
        pb.file_index_1based = file_index_1based;
        q.Push(std::move(pb));
    }
    return arrow::Status::OK();
}

arrow::Status ReadCsvFileBatches(const std::string& path, size_t file_index_1based,
                                 const yikv::schema::Schema& sch, BoundedParsedBatchQueue& q,
                                 ErrorState& err) {
    auto infile_res = arrow::io::ReadableFile::Open(path);
    if (!infile_res.ok()) {
        err.Set(infile_res.status());
        q.Abort();
        return infile_res.status();
    }
    auto input = std::static_pointer_cast<arrow::io::InputStream>(*infile_res);
    return ReadCsvFromInputStream(path, file_index_1based, input, sch, q, err);
}

}  // namespace

FileSource::FileSource(std::vector<std::string> paths, const yikv::schema::Schema* schema, CloudFileSystems cloud)
    : paths_(std::move(paths)), schema_(schema), cloud_(std::move(cloud)) {}

void FileSource::ProduceLoop(BoundedParsedBatchQueue& q, ErrorState& err) {
    if (!schema_) {
        err.Set(arrow::Status::Invalid("FileSource: null schema"));
        q.Abort();
        return;
    }
    while (!err.failed.load()) {
        const std::size_t fi = next_file_.fetch_add(1, std::memory_order_relaxed);
        if (fi >= paths_.size()) return;
        const std::string& path = paths_[fi];

        if (IsCloudUri(path)) {
            std::string scheme;
            std::string bucket;
            std::string key;
            if (!ParseCloudUri(path, &scheme, &bucket, &key)) {
                err.Set(arrow::Status::Invalid("FileSource: invalid cloud URI: ", path));
                q.Abort();
                return;
            }
            arrow::fs::FileSystem* fs = cloud_.FsForScheme(scheme);
            if (!fs) {
                err.Set(arrow::Status::Invalid("FileSource: no FileSystem for scheme ", scheme, " (URI: ", path,
                                                ")"));
                q.Abort();
                return;
            }
            const std::string fs_path = bucket + "/" + key;
            if (EndsWithIgnoreCaseStr(path, ".parquet")) {
                auto infile_res = fs->OpenInputFile(fs_path);
                if (!infile_res.ok()) {
                    err.Set(infile_res.status());
                    q.Abort();
                    return;
                }
                err.Set(ReadParquetFromRandomAccess(path, fi + 1, *infile_res, *schema_, q, err));
                if (err.failed.load()) return;
                continue;
            }
            if (EndsWithIgnoreCaseStr(path, ".csv")) {
                auto stream_res = fs->OpenInputStream(fs_path);
                if (!stream_res.ok()) {
                    err.Set(stream_res.status());
                    q.Abort();
                    return;
                }
                err.Set(ReadCsvFromInputStream(path, fi + 1, *stream_res, *schema_, q, err));
                if (err.failed.load()) return;
                continue;
            }
            err.Set(arrow::Status::Invalid("FileSource: remote object must be .parquet or .csv: ", path));
            q.Abort();
            return;
        }

        const fs::path p(path);
        if (EndsWithIgnoreCase(".parquet", p)) {
            err.Set(ReadParquetFileBatches(path, fi + 1, *schema_, q, err));
            if (err.failed.load()) return;
            continue;
        }
        if (EndsWithIgnoreCase(".csv", p)) {
            err.Set(ReadCsvFileBatches(path, fi + 1, *schema_, q, err));
            if (err.failed.load()) return;
            continue;
        }
        err.Set(arrow::Status::Invalid("FileSource: unsupported file extension: ", path));
        q.Abort();
        return;
    }
}

}  // namespace yikv::indexer

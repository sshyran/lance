//  Copyright 2022 Lance Authors
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#include "lance/arrow/file_lance.h"

#include <arrow/dataset/file_base.h>
#include <arrow/util/thread_pool.h>
#include <fmt/format.h>

#include <filesystem>
#include <memory>
#include <utility>

#include "lance/arrow/file_lance_ext.h"
#include "lance/format/manifest.h"
#include "lance/format/schema.h"
#include "lance/io/exec/project.h"
#include "lance/io/reader.h"
#include "lance/io/record_batch_reader.h"
#include "lance/io/writer.h"

const char kLanceFormatTypeName[] = "lance";

namespace fs = std::filesystem;

namespace lance::arrow {

class LanceFileFormat::Impl {
 public:
  std::shared_ptr<lance::format::Manifest> manifest;
};

LanceFileFormat::LanceFileFormat() : impl_(std::make_unique<Impl>()) {}

LanceFileFormat::~LanceFileFormat() {}

std::shared_ptr<LanceFileFormat> LanceFileFormat::Make() {
  return std::make_shared<LanceFileFormat>();
}

std::string LanceFileFormat::type_name() const { return kLanceFormatTypeName; }

bool LanceFileFormat::Equals(const FileFormat& other) const {
  return type_name() == other.type_name();
}

::arrow::Result<bool> LanceFileFormat::IsSupported(
    [[maybe_unused]] const ::arrow::dataset::FileSource& source) const {
  return source.path().ends_with(".lance");
}

::arrow::Result<std::shared_ptr<::arrow::Schema>> LanceFileFormat::Inspect(
    const ::arrow::dataset::FileSource& source) const {
  if (impl_->manifest) {
    return impl_->manifest->schema().ToArrow();
  }

  ARROW_ASSIGN_OR_RAISE(auto infile, source.Open());
  auto reader = std::make_shared<lance::io::FileReader>(infile);
  ARROW_RETURN_NOT_OK(reader->Open());
  /// TODO: load dictionary here.
  impl_->manifest = reader->manifest();
  return impl_->manifest->schema().ToArrow();
}

::arrow::Future<::arrow::util::optional<int64_t>> LanceFileFormat::CountRows(
    const std::shared_ptr<::arrow::dataset::FileFragment>& file,
    ::arrow::compute::Expression predicate,
    const std::shared_ptr<::arrow::dataset::ScanOptions>& options) {
  if (predicate.Equals(::arrow::compute::literal(true))) {
    // Fast path.
    auto executor = options->io_context.executor();
    assert(executor != nullptr);
    auto result = executor->Submit(
        [&](const auto& file) -> ::arrow::Result<::arrow::util::optional<int64_t>> {
          ARROW_ASSIGN_OR_RAISE(auto infile, file->source().Open());
          ARROW_ASSIGN_OR_RAISE(auto reader,
                                lance::io::FileReader::Make(infile, this->impl_->manifest));
          return ::arrow::util::make_optional(reader->length());
        },
        file);
    if (!result.ok()) {
      return result.status();
    }
    return result.ValueOrDie();
  }
  // If filter presented, slow path
  return FileFormat::CountRows(file, predicate, options);
}

::arrow::Result<::arrow::RecordBatchGenerator> LanceFileFormat::ScanBatchesAsync(
    const std::shared_ptr<::arrow::dataset::ScanOptions>& options,
    const std::shared_ptr<::arrow::dataset::FileFragment>& file) const {
  ARROW_ASSIGN_OR_RAISE(auto infile, file->source().Open());
  ARROW_ASSIGN_OR_RAISE(auto reader, lance::io::FileReader::Make(infile, impl_->manifest));
  auto batch_reader = lance::io::RecordBatchReader(
      std::move(reader), options, ::arrow::internal::GetCpuThreadPool());
  ARROW_RETURN_NOT_OK(batch_reader.Open());
  return ::arrow::RecordBatchGenerator(std::move(batch_reader));
}

::arrow::Result<std::shared_ptr<::arrow::dataset::FileWriter>> LanceFileFormat::MakeWriter(
    std::shared_ptr<::arrow::io::OutputStream> destination,
    std::shared_ptr<::arrow::Schema> schema,
    std::shared_ptr<::arrow::dataset::FileWriteOptions> options,
    ::arrow::fs::FileLocator destination_locator) const {
  return std::shared_ptr<::arrow::dataset::FileWriter>(
      new io::FileWriter(schema, options, destination, destination_locator));
}

std::shared_ptr<::arrow::dataset::FileWriteOptions> LanceFileFormat::DefaultWriteOptions() {
  return std::make_shared<FileWriteOptions>();
}

FileWriteOptions::FileWriteOptions()
    : ::arrow::dataset::FileWriteOptions(std::make_shared<LanceFileFormat>()) {}

::arrow::Status FileWriteOptions::Validate() const {
  if (batch_size <= 1) {
    return ::arrow::Status::Invalid("Batch size must be greater than 1");
  }
  return ::arrow::Status::OK();
}

std::string LanceFragmentScanOptions::type_name() const { return kLanceFormatTypeName; }

bool IsLanceFragmentScanOptions(const ::arrow::dataset::FragmentScanOptions& fso) {
  return fso.type_name() == kLanceFormatTypeName;
}

LanceFragment::LanceFragment(std::shared_ptr<::arrow::fs::FileSystem> fs,
                             std::string data_dir,
                             std::shared_ptr<lance::format::DataFragment> fragment,
                             const format::Schema& schema)
    : fs_(std::move(fs)),
      data_uri_(std::move(data_dir)),
      fragment_(std::move(fragment)),
      schema_(schema) {}

::arrow::Result<::arrow::RecordBatchGenerator> LanceFragment::ScanBatchesAsync(
    const std::shared_ptr<::arrow::dataset::ScanOptions>& options) {
  // Only support one file for now.
  assert(fragment_->data_files().size() == 1);

  // There will be more than one file when schema evolution happens, will implement it later.
  for (const auto& data_file : fragment_->data_files()) {
    auto full_path = (fs::path(data_uri_) / data_file.path()).string();
    ARROW_ASSIGN_OR_RAISE(auto infile, fs_->OpenInputFile(full_path));
    ARROW_ASSIGN_OR_RAISE(auto reader, lance::io::FileReader::Make(infile));
    auto batch_reader = lance::io::RecordBatchReader(
        std::move(reader), options, ::arrow::internal::GetCpuThreadPool());
    ARROW_RETURN_NOT_OK(batch_reader.Open());
    return ::arrow::RecordBatchGenerator(std::move(batch_reader));
  }
  return ::arrow::Status::IOError("Lance Fragment has zero file");
}

::arrow::Result<std::shared_ptr<::arrow::Schema>> LanceFragment::ReadPhysicalSchemaImpl() {
  return schema_.ToArrow();
}

}  // namespace lance::arrow
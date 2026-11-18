#include "db/manifest.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "db/filenames.h"

namespace lsmkv {

Manifest::Manifest(Directory* dir) : dir_(dir), current_(std::make_shared<const Version>()) {}

Manifest::~Manifest() = default;

void Manifest::MarkFileNumberUsed(uint64_t number) {
  if (number >= next_file_number_) next_file_number_ = number + 1;
}

Status Manifest::MarkExistingFilesUsed(bool* has_current) {
  std::vector<std::string> names;
  Status s = dir_->List(&names);
  if (!s.ok()) return s;
  *has_current = false;
  for (const std::string& name : names) {
    const std::optional<ParsedFileName> parsed = ParseFileName(name);
    if (!parsed) continue;
    switch (parsed->kind) {
      case FileKind::kWal:
      case FileKind::kSst:
      case FileKind::kManifest:
        MarkFileNumberUsed(parsed->number);
        break;
      case FileKind::kCurrent:
        *has_current = true;
        break;
      case FileKind::kLock:
      case FileKind::kCurrentTmp:
        break;
    }
  }
  return Status::OK();
}

Status Manifest::Create() {
  bool has_current = false;
  Status s = MarkExistingFilesUsed(&has_current);
  if (!s.ok()) return s;
  if (has_current) return Status::InvalidArgument(dir_->path(), "database already exists");
  return WriteNewManifest();
}

Status Manifest::Recover(bool* exists) {
  *exists = false;
  std::string current;
  Status s = ReadEntireFile(*dir_, kCurrentFileName, &current);
  if (s.IsNotFound()) return Status::OK();
  if (!s.ok()) return s;
  *exists = true;

  std::optional<ParsedFileName> parsed;
  if (!current.empty() && current.back() == '\n') {
    parsed = ParseFileName(std::string_view(current).substr(0, current.size() - 1));
  }
  if (!parsed || parsed->kind != FileKind::kManifest) {
    return Status::Corruption("CURRENT", "does not name a MANIFEST");
  }
  const std::string name = ManifestFileName(parsed->number);

  std::string contents;
  s = ReadEntireFile(*dir_, name, &contents);
  if (s.IsNotFound()) return Status::Corruption("CURRENT names a missing file", name);
  if (!s.ok()) return s;

  std::shared_ptr<const Version> version = current_;
  std::optional<uint64_t> log_number, next_file_number;
  std::optional<SequenceNumber> last_sequence;
  WalReader reader(std::move(contents));
  std::string_view payload;
  // Stops at the end or at the first bad record. A bad record is an
  // unsynced tail, which never held a committed edit.
  while (reader.Next(&payload) == WalReader::Outcome::kRecord) {
    VersionEdit edit;
    s = edit.DecodeFrom(payload);
    if (!s.ok()) return s;
    std::shared_ptr<const Version> next;
    s = version->Apply(edit, &next);
    if (!s.ok()) return s;
    version = std::move(next);
    if (edit.log_number) log_number = edit.log_number;
    if (edit.next_file_number) next_file_number = edit.next_file_number;
    if (edit.last_sequence) last_sequence = edit.last_sequence;
  }
  if (!log_number || !next_file_number || !last_sequence) {
    return Status::Corruption(name, "missing a counter");
  }

  bool has_current = false;
  s = MarkExistingFilesUsed(&has_current);
  if (!s.ok()) return s;
  next_file_number_ = std::max(next_file_number_, *next_file_number);
  MarkFileNumberUsed(parsed->number);
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& f : version->files(level)) MarkFileNumberUsed(f->number);
  }
  current_ = std::move(version);
  log_number_ = *log_number;
  last_sequence_ = *last_sequence;
  manifest_number_ = parsed->number;
  return Status::OK();
}

Status Manifest::Rewrite() { return WriteNewManifest(); }

Status Manifest::WriteNewManifest() {
  if (!error_.ok()) return error_;
  const uint64_t number = NewFileNumber();

  VersionEdit snapshot;
  snapshot.log_number = log_number_;
  snapshot.next_file_number = next_file_number_;
  snapshot.last_sequence = last_sequence_;
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& f : current_->files(level)) snapshot.AddFile(level, *f);
  }
  std::string record;
  snapshot.EncodeTo(&record);

  std::unique_ptr<WritableFile> file;
  Status s = dir_->Create(ManifestFileName(number), &file);
  std::unique_ptr<WalWriter> writer;
  if (s.ok()) {
    writer = std::make_unique<WalWriter>(std::move(file));
    s = writer->AddRecord({record});
  }
  if (s.ok()) s = writer->Sync();
  if (s.ok()) s = PointCurrentAt(number);
  if (!s.ok()) {
    error_ = s;
    return s;
  }
  writer_ = std::move(writer);  // closes the previous MANIFEST, if any
  manifest_number_ = number;
  return Status::OK();
}

Status Manifest::PointCurrentAt(uint64_t manifest_number) {
  // Directory::Create is exclusive, so a CURRENT.tmp left by a crash must go.
  Status s = dir_->Remove(kCurrentTmpFileName);
  if (s.IsNotFound()) s = Status::OK();
  std::unique_ptr<WritableFile> tmp;
  if (s.ok()) s = dir_->Create(kCurrentTmpFileName, &tmp);
  if (s.ok()) s = tmp->Append(ManifestFileName(manifest_number) + "\n");
  if (s.ok()) s = tmp->Sync();
  if (s.ok()) s = tmp->Close();
  // Makes the new MANIFEST's name durable before CURRENT can name it.
  if (s.ok()) s = dir_->Sync();
  if (s.ok()) s = dir_->Rename(kCurrentTmpFileName, kCurrentFileName);
  if (s.ok()) s = dir_->Sync();
  return s;
}

Status Manifest::Commit(VersionEdit* edit) {
  if (!error_.ok()) return error_;
  if (!writer_) return Status::InvalidArgument("MANIFEST is not open for writing");
  if (edit->log_number && *edit->log_number < log_number_) {
    return Status::InvalidArgument("log number would decrease");
  }
  if (edit->last_sequence && *edit->last_sequence < last_sequence_) {
    return Status::InvalidArgument("last sequence would decrease");
  }
  for (const VersionEdit::AddedFile& a : edit->added) {
    if (a.info.number >= next_file_number_) {
      return Status::InvalidArgument("SST " + std::to_string(a.info.number),
                                     "number was never allocated");
    }
  }
  edit->next_file_number = next_file_number_;

  std::shared_ptr<const Version> next;
  Status s = current_->Apply(*edit, &next);
  if (!s.ok()) return s;

  std::string record;
  edit->EncodeTo(&record);
  s = writer_->AddRecord({record});
  if (s.ok()) s = writer_->Sync();
  if (!s.ok()) {
    error_ = s;
    return s;
  }
  current_ = std::move(next);
  if (edit->log_number) log_number_ = *edit->log_number;
  if (edit->last_sequence) last_sequence_ = *edit->last_sequence;
  return Status::OK();
}

}  // namespace lsmkv

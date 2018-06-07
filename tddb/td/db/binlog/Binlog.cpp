//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2018
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/db/binlog/Binlog.h"

#include "td/db/binlog/detail/BinlogEventsBuffer.h"
#include "td/db/binlog/detail/BinlogEventsProcessor.h"

#include "td/utils/buffer.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/Clocks.h"
#include "td/utils/port/Fd.h"
#include "td/utils/port/path.h"
#include "td/utils/port/Stat.h"
#include "td/utils/Random.h"
#include "td/utils/ScopeGuard.h"
#include "td/utils/Status.h"
#include "td/utils/Time.h"
#include "td/utils/tl_helpers.h"
#include "td/utils/tl_parsers.h"

namespace td {
namespace detail {
struct AesCtrEncryptionEvent {
  static constexpr size_t min_salt_size() {
    return 16;  // 256 bits
  }
  static constexpr size_t default_salt_size() {
    return 32;  // 256 bits
  }
  static constexpr size_t key_size() {
    return 32;  // 256 bits
  }
  static constexpr size_t iv_size() {
    return 16;  // 128 bits
  }
  static constexpr size_t hash_size() {
    return 32;  // 256 bits
  }
  static constexpr size_t kdf_iteration_count() {
    return 60002;
  }
  static constexpr size_t kdf_fast_iteration_count() {
    return 2;
  }

  BufferSlice key_salt_;
  BufferSlice iv_;
  BufferSlice key_hash_;

  BufferSlice generate_key(const DbKey &db_key) {
    CHECK(!db_key.is_empty());
    BufferSlice key(key_size());
    size_t iteration_count = kdf_iteration_count();
    if (db_key.is_raw_key()) {
      iteration_count = kdf_fast_iteration_count();
    }
    pbkdf2_sha256(db_key.data(), key_salt_.as_slice(), narrow_cast<int>(iteration_count), key.as_slice());
    return key;
  }
  BufferSlice generate_hash(Slice key) {
    BufferSlice hash(hash_size());
    hmac_sha256(key, "cucumbers everywhere", hash.as_slice());
    return hash;
  }

  template <class StorerT>
  void store(StorerT &storer) const {
    using td::store;
    BEGIN_STORE_FLAGS();
    END_STORE_FLAGS();
    store(key_salt_, storer);
    store(iv_, storer);
    store(key_hash_, storer);
  }
  template <class ParserT>
  void parse(ParserT &&parser) {
    using td::parse;
    BEGIN_PARSE_FLAGS();
    END_PARSE_FLAGS_GENERIC();
    parse(key_salt_, parser);
    parse(iv_, parser);
    parse(key_hash_, parser);
  }
};

class BinlogReader {
 public:
  BinlogReader() = default;
  explicit BinlogReader(ChainBufferReader *input) : input_(input) {
  }
  void set_input(ChainBufferReader *input) {
    input_ = input;
  }

  int64 offset() {
    return offset_;
  }
  Result<size_t> read_next(BinlogEvent *event) {
    if (state_ == ReadLength) {
      if (input_->size() < 4) {
        return 4;
      }
      auto it = input_->clone();

      char buf[4];
      it.advance(4, MutableSlice(buf, 4));
      size_ = static_cast<size_t>(TlParser(Slice(buf, 4)).fetch_int());

      if (size_ > MAX_EVENT_SIZE) {
        return Status::Error(PSLICE() << "Too big event " << tag("size", size_));
      }
      if (size_ < MIN_EVENT_SIZE) {
        return Status::Error(PSLICE() << "Too small event " << tag("size", size_));
      }
      state_ = ReadEvent;
    }

    if (input_->size() < size_) {
      return size_;
    }

    TRY_STATUS(event->init(input_->cut_head(size_).move_as_buffer_slice()));
    offset_ += size_;
    event->offset_ = offset_;
    state_ = ReadLength;
    return 0;
  }

 private:
  ChainBufferReader *input_;
  enum { ReadLength, ReadEvent } state_ = ReadLength;
  size_t size_{0};
  int64 offset_{0};
};
}  // namespace detail

bool Binlog::IGNORE_ERASE_HACK = false;

Binlog::Binlog() = default;

Binlog::~Binlog() {
  close().ignore();
}

Result<FileFd> Binlog::open_binlog(CSlice path, int32 flags) {
  TRY_RESULT(fd, FileFd::open(path, flags));
  TRY_STATUS(fd.lock(FileFd::LockFlags::Write, 100));
  return std::move(fd);
}

Status Binlog::init(string path, const Callback &callback, DbKey db_key, DbKey old_db_key, int32 dummy,
                    const Callback &debug_callback) {
  close().ignore();

  db_key_ = std::move(db_key);
  old_db_key_ = std::move(old_db_key);

  processor_ = std::make_unique<detail::BinlogEventsProcessor>();
  // Turn off BinlogEventsBuffer
  // events_buffer_ = std::make_unique<detail::BinlogEventsBuffer>();

  // try to restore binlog from regenerated version
  if (stat(path).is_error()) {
    rename(PSLICE() << path << ".new", path).ignore();
  }

  info_ = BinlogInfo();
  info_.was_created = stat(path).is_error();

  TRY_RESULT(fd, open_binlog(path, FileFd::Flags::Read | FileFd::Flags::Write | FileFd::Flags::Create));
  fd_ = BufferedFdBase<FileFd>(std::move(fd));
  fd_size_ = 0;
  path_ = std::move(path);

  auto status = load_binlog(callback, debug_callback);
  if (status.is_error()) {
    close().ignore();
    return status;
  }
  info_.last_id = processor_->last_id();
  last_id_ = processor_->last_id();
  if (info_.wrong_password) {
    close().ignore();
    return Status::Error(Error::WrongPassword, "Wrong password");
  }

  if ((!db_key_.is_empty() && !db_key_used_) || (db_key_.is_empty() && encryption_type_ != EncryptionType::None)) {
    aes_ctr_key_salt_ = BufferSlice();
    do_reindex();
  }

  info_.is_opened = true;
  return Status::OK();
}

void Binlog::add_event(BinlogEvent &&event) {
  if (!events_buffer_) {
    do_add_event(std::move(event));
  } else {
    events_buffer_->add_event(std::move(event));
  }
  lazy_flush();

  if (state_ == State::Run) {
    auto fd_size = fd_size_;
    if (events_buffer_) {
      fd_size += events_buffer_->size();
    }
    auto need_reindex = [&](int64 min_size, int rate) {
      return fd_size > min_size && fd_size / rate > processor_->total_raw_events_size();
    };
    if (need_reindex(100000, 5) || need_reindex(500000, 2)) {
      LOG(INFO) << tag("fd_size", format::as_size(fd_size))
                << tag("total events size", format::as_size(processor_->total_raw_events_size()));
      do_reindex();
    }
  }
}

size_t Binlog::flush_events_buffer(bool force) {
  if (!events_buffer_) {
    return 0;
  }
  if (!force && !events_buffer_->need_flush()) {
    return events_buffer_->size();
  }
  CHECK(!in_flush_events_buffer_);
  in_flush_events_buffer_ = true;
  events_buffer_->flush([&](BinlogEvent &&event) { this->do_add_event(std::move(event)); });
  in_flush_events_buffer_ = false;
  return 0;
}

void Binlog::do_add_event(BinlogEvent &&event) {
  if (event.flags_ & BinlogEvent::Flags::Partial) {
    event.flags_ &= ~BinlogEvent::Flags::Partial;
    pending_events_.emplace_back(std::move(event));
  } else {
    for (auto &pending_event : pending_events_) {
      do_event(std::move(pending_event));
    }
    pending_events_.clear();
    do_event(std::move(event));
  }
}

Status Binlog::close(bool need_sync) {
  if (fd_.empty()) {
    return Status::OK();
  }
  SCOPE_EXIT {
    path_ = "";
    info_.is_opened = false;
    fd_.close();
    need_sync_ = false;
  };
  if (need_sync) {
    sync();
  } else {
    flush();
  }
  return Status::OK();
}

void Binlog::change_key(DbKey new_db_key) {
  db_key_ = std::move(new_db_key);
  aes_ctr_key_salt_ = BufferSlice();
  do_reindex();
}

Status Binlog::close_and_destroy() {
  auto path = path_;
  auto close_status = close(false);
  destroy(path).ignore();
  return close_status;
}
Status Binlog::destroy(Slice path) {
  unlink(PSLICE() << path).ignore();
  unlink(PSLICE() << path << ".new").ignore();
  return Status::OK();
}

void Binlog::do_event(BinlogEvent &&event) {
  fd_events_++;
  fd_size_ += event.raw_event_.size();

  if (state_ == State::Run || state_ == State::Reindex) {
    VLOG(binlog) << "Write binlog event: " << format::cond(state_ == State::Reindex, "[reindex] ") << event;
    switch (encryption_type_) {
      case EncryptionType::None: {
        buffer_writer_.append(event.raw_event_.clone());
        break;
      }
      case EncryptionType::AesCtr: {
        buffer_writer_.append(event.raw_event_.as_slice());
        break;
      }
    }
  }

  if (event.type_ < 0) {
    if (event.type_ == BinlogEvent::ServiceTypes::AesCtrEncryption) {
      detail::AesCtrEncryptionEvent encryption_event;
      encryption_event.parse(TlParser(event.data_));

      BufferSlice key;
      if (aes_ctr_key_salt_.as_slice() == encryption_event.key_salt_.as_slice()) {
        key = BufferSlice(Slice(aes_ctr_key_.raw, sizeof(aes_ctr_key_.raw)));
      } else if (!db_key_.is_empty()) {
        key = encryption_event.generate_key(db_key_);
      }

      if (encryption_event.generate_hash(key.as_slice()).as_slice() != encryption_event.key_hash_.as_slice()) {
        CHECK(state_ == State::Load);
        if (!old_db_key_.is_empty()) {
          key = encryption_event.generate_key(old_db_key_);
          if (encryption_event.generate_hash(key.as_slice()).as_slice() != encryption_event.key_hash_.as_slice()) {
            info_.wrong_password = true;
          }
        } else {
          info_.wrong_password = true;
        }
      } else {
        db_key_used_ = true;
      }

      encryption_type_ = EncryptionType::AesCtr;

      aes_ctr_key_salt_ = encryption_event.key_salt_.copy();
      update_encryption(key.as_slice(), encryption_event.iv_.as_slice());

      if (state_ == State::Load) {
        update_read_encryption();
        LOG(INFO) << "Load: init encryption";
      } else {
        CHECK(state_ == State::Reindex);
        flush();
        update_write_encryption();
        //LOG(INFO) << format::cond(state_ == State::Run, "Run", "Reindex") << ": init encryption";
      }
    }
  }

  if (state_ != State::Reindex) {
    processor_->add_event(std::move(event));
  }
}

void Binlog::sync() {
  flush();
  if (need_sync_) {
    auto status = fd_.sync();
    LOG_IF(FATAL, status.is_error()) << "Failed to sync binlog: " << status;
    need_sync_ = false;
  }
}

void Binlog::flush() {
  if (state_ == State::Load) {
    return;
  }
  flush_events_buffer(true);
  // NB: encryption happens during flush
  if (byte_flow_flag_) {
    byte_flow_source_.wakeup();
  }
  auto r_written = fd_.flush_write();
  r_written.ensure();
  auto written = r_written.ok();
  if (written > 0) {
    need_sync_ = true;
  }
  need_flush_since_ = 0;
  LOG_IF(FATAL, fd_.need_flush_write()) << "Failed to flush binlog";
}

void Binlog::lazy_flush() {
  size_t events_buffer_size = flush_events_buffer(false /*force*/);
  buffer_reader_.sync_with_writer();
  auto size = buffer_reader_.size() + events_buffer_size;
  if (size > (1 << 14)) {
    flush();
  } else if (size > 0 && need_flush_since_ == 0) {
    need_flush_since_ = Time::now_cached();
  }
}

void Binlog::update_read_encryption() {
  CHECK(binlog_reader_ptr_);
  switch (encryption_type_) {
    case EncryptionType::None: {
      binlog_reader_ptr_->set_input(&buffer_reader_);
      byte_flow_flag_ = false;
      break;
    }
    case EncryptionType::AesCtr: {
      byte_flow_source_ = ByteFlowSource(&buffer_reader_);
      aes_xcode_byte_flow_ = AesCtrByteFlow();
      aes_xcode_byte_flow_.init(std::move(aes_ctr_state_));
      byte_flow_sink_ = ByteFlowSink();
      byte_flow_source_ >> aes_xcode_byte_flow_ >> byte_flow_sink_;
      byte_flow_flag_ = true;
      binlog_reader_ptr_->set_input(byte_flow_sink_.get_output());
      break;
    }
  }
}

void Binlog::update_write_encryption() {
  switch (encryption_type_) {
    case EncryptionType::None: {
      fd_.set_output_reader(&buffer_reader_);
      byte_flow_flag_ = false;
      break;
    }
    case EncryptionType::AesCtr: {
      byte_flow_source_ = ByteFlowSource(&buffer_reader_);
      aes_xcode_byte_flow_ = AesCtrByteFlow();
      aes_xcode_byte_flow_.init(std::move(aes_ctr_state_));
      byte_flow_sink_ = ByteFlowSink();
      byte_flow_source_ >> aes_xcode_byte_flow_ >> byte_flow_sink_;
      byte_flow_flag_ = true;
      fd_.set_output_reader(byte_flow_sink_.get_output());
      break;
    }
  }
}

Status Binlog::load_binlog(const Callback &callback, const Callback &debug_callback) {
  state_ = State::Load;

  buffer_writer_ = ChainBufferWriter();
  buffer_reader_ = buffer_writer_.extract_reader();
  fd_.set_input_writer(&buffer_writer_);
  detail::BinlogReader reader;
  binlog_reader_ptr_ = &reader;

  update_read_encryption();

  bool ready_flag = false;
  fd_.update_flags(Fd::Flag::Read);
  info_.wrong_password = false;
  while (true) {
    BinlogEvent event;
    auto r_need_size = reader.read_next(&event);
    if (r_need_size.is_error()) {
      LOG(ERROR) << r_need_size.error();
      break;
    }
    auto need_size = r_need_size.move_as_ok();
    // LOG(ERROR) << "need size = " << need_size;
    if (need_size == 0) {
      if (IGNORE_ERASE_HACK && event.type_ == BinlogEvent::ServiceTypes::Empty &&
          (event.flags_ & BinlogEvent::Flags::Rewrite) != 0) {
        // skip erase
      } else {
        if (debug_callback) {
          debug_callback(event);
        }
        do_add_event(std::move(event));
        if (info_.wrong_password) {
          return Status::OK();
        }
      }
      ready_flag = false;
    } else {
      // TODO(now): fix bug
      if (ready_flag) {
        break;
      }
      TRY_STATUS(fd_.flush_read(max(need_size, static_cast<size_t>(4096))));
      buffer_reader_.sync_with_writer();
      if (byte_flow_flag_) {
        byte_flow_source_.wakeup();
      }
      ready_flag = true;
    }
  }

  auto offset = processor_->offset();
  processor_->for_each([&](BinlogEvent &event) {
    VLOG(binlog) << "Replay binlog event: " << event;
    if (callback) {
      callback(event);
    }
  });

  auto fd_size = fd_.get_size();
  if (offset != fd_size) {
    LOG(ERROR) << "Truncate " << tag("path", path_) << tag("old_size", fd_size) << tag("new_size", offset);
    fd_.seek(offset).ensure();
    fd_.truncate_to_current_position(offset).ensure();
    db_key_used_ = false;  // force reindex
  }
  CHECK(IGNORE_ERASE_HACK || fd_size_ == offset) << fd_size << " " << fd_size_ << " " << offset;
  binlog_reader_ptr_ = nullptr;
  state_ = State::Run;

  buffer_writer_ = ChainBufferWriter();
  buffer_reader_ = buffer_writer_.extract_reader();

  // reuse aes_ctr_state_
  if (encryption_type_ == EncryptionType::AesCtr) {
    aes_ctr_state_ = aes_xcode_byte_flow_.move_aes_ctr_state();
  }
  update_write_encryption();

  return Status::OK();
}

static int64 file_size(CSlice path) {
  auto r_stat = stat(path);
  if (r_stat.is_error()) {
    return 0;
  }
  return r_stat.ok().size_;
}

void Binlog::update_encryption(Slice key, Slice iv) {
  MutableSlice(aes_ctr_key_.raw, sizeof(aes_ctr_key_.raw)).copy_from(key);
  UInt128 aes_ctr_iv;
  MutableSlice(aes_ctr_iv.raw, sizeof(aes_ctr_iv.raw)).copy_from(iv);
  aes_ctr_state_.init(aes_ctr_key_, aes_ctr_iv);
}

void Binlog::reset_encryption() {
  if (db_key_.is_empty()) {
    encryption_type_ = EncryptionType::None;
    return;
  }

  using EncryptionEvent = detail::AesCtrEncryptionEvent;
  EncryptionEvent event;

  if (aes_ctr_key_salt_.empty()) {
    event.key_salt_ = BufferSlice(EncryptionEvent::default_salt_size());
    Random::secure_bytes(event.key_salt_.as_slice());
  } else {
    event.key_salt_ = aes_ctr_key_salt_.clone();
  }
  event.iv_ = BufferSlice(EncryptionEvent::iv_size());
  Random::secure_bytes(event.iv_.as_slice());

  BufferSlice key;
  if (aes_ctr_key_salt_.as_slice() == event.key_salt_.as_slice()) {
    key = BufferSlice(Slice(aes_ctr_key_.raw, sizeof(aes_ctr_key_.raw)));
  } else {
    key = event.generate_key(db_key_);
  }

  event.key_hash_ = event.generate_hash(key.as_slice());

  do_event(BinlogEvent(
      BinlogEvent::create_raw(0, BinlogEvent::ServiceTypes::AesCtrEncryption, 0, create_default_storer(event))));
}

void Binlog::do_reindex() {
  flush_events_buffer(true);
  // start reindex
  CHECK(state_ == State::Run);
  state_ = State::Reindex;
  SCOPE_EXIT {
    state_ = State::Run;
  };

  auto start_time = Clocks::monotonic();
  auto start_size = file_size(path_);
  auto start_events = fd_events_;

  string new_path = path_ + ".new";

  auto r_opened_file = open_binlog(new_path, FileFd::Flags::Write | FileFd::Flags::Create | FileFd::Truncate);
  if (r_opened_file.is_error()) {
    LOG(ERROR) << "Can't open new binlog for regenerate: " << r_opened_file.error();
    return;
  }
  fd_.close();
  fd_ = BufferedFdBase<FileFd>(r_opened_file.move_as_ok());

  buffer_writer_ = ChainBufferWriter();
  buffer_reader_ = buffer_writer_.extract_reader();
  encryption_type_ = EncryptionType::None;
  update_write_encryption();

  // reindex
  fd_size_ = 0;
  fd_events_ = 0;
  reset_encryption();
  processor_->for_each([&](BinlogEvent &event) {
    do_event(std::move(event));  // NB: no move is actually happens
  });
  need_sync_ = true;  // must sync creation of the file
  sync();

  // finish_reindex
  auto status = unlink(path_);
  LOG_IF(FATAL, status.is_error()) << "Failed to unlink old binlog: " << status;
  status = rename(new_path, path_);
  LOG_IF(FATAL, status.is_error()) << "Failed to rename binlog: " << status;

  auto finish_time = Clocks::monotonic();
  auto finish_size = fd_size_;
  auto finish_events = fd_events_;
  CHECK(fd_size_ == file_size(path_));

  // TODO: print warning only if time or ratio is suspicious
  double ratio = static_cast<double>(start_size) / static_cast<double>(finish_size + 1);
  LOG(INFO) << "regenerate index " << tag("name", path_) << tag("time", format::as_time(finish_time - start_time))
            << tag("before_size", format::as_size(start_size)) << tag("after_size", format::as_size(finish_size))
            << tag("ratio", ratio) << tag("before_events", start_events) << tag("after_events", finish_events);

  buffer_writer_ = ChainBufferWriter();
  buffer_reader_ = buffer_writer_.extract_reader();

  // reuse aes_ctr_state_
  if (encryption_type_ == EncryptionType::AesCtr) {
    aes_ctr_state_ = aes_xcode_byte_flow_.move_aes_ctr_state();
  }
  update_write_encryption();
}

}  // namespace td

/*-------------------------------------------------------------------------
 *
 * duckdb_vfs_file_system.cpp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * IDENTIFICATION
 *	  src/paimon_storage/duckdb_vfs_file_system.cpp
 *
 *-------------------------------------------------------------------------
 */

#include "duckdb_vfs_file_system.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"

#include <mutex>

namespace duckdb {

namespace {

paimon::Status IOError(const char *op, const std::string &path, const std::exception &ex) {
	return paimon::Status::IOError(op, " '", path, "' failed via DuckDB file system: ", ex.what());
}

std::string JoinPath(const std::string &dir, const std::string &name) {
	if (dir.empty() || dir.back() == '/') {
		return dir + name;
	}
	return dir + "/" + name;
}

class DuckDBVfsFileStatus : public paimon::FileStatus {
public:
	DuckDBVfsFileStatus(std::string path, int64_t len, bool is_dir, int64_t mtime_ms)
	    : path(std::move(path)), len(len), is_dir(is_dir), mtime_ms(mtime_ms) {
	}
	int64_t GetLen() const override {
		return len;
	}
	bool IsDir() const override {
		return is_dir;
	}
	std::string GetPath() const override {
		return path;
	}
	int64_t GetModificationTime() const override {
		return mtime_ms;
	}

private:
	std::string path;
	int64_t len;
	bool is_dir;
	int64_t mtime_ms;
};

class DuckDBVfsBasicFileStatus : public paimon::BasicFileStatus {
public:
	DuckDBVfsBasicFileStatus(std::string path, bool is_dir) : path(std::move(path)), is_dir(is_dir) {
	}
	bool IsDir() const override {
		return is_dir;
	}
	std::string GetPath() const override {
		return path;
	}

private:
	std::string path;
	bool is_dir;
};

class DuckDBVfsInputStream : public paimon::InputStream {
public:
	DuckDBVfsInputStream(unique_ptr<FileHandle> handle, std::string path, int64_t length)
	    : handle(std::move(handle)), path(std::move(path)), length(length) {
	}

	paimon::Status Close() override {
		std::lock_guard<std::mutex> guard(lock);
		if (!handle) {
			return paimon::Status::OK();
		}
		try {
			handle->Close();
			handle.reset();
			return paimon::Status::OK();
		} catch (std::exception &ex) {
			return IOError("Close", path, ex);
		}
	}

	paimon::Status Seek(int64_t offset, paimon::SeekOrigin origin) override {
		std::lock_guard<std::mutex> guard(lock);
		try {
			int64_t pos;
			switch (origin) {
			case paimon::FS_SEEK_SET:
				pos = offset;
				break;
			case paimon::FS_SEEK_CUR:
				pos = NumericCast<int64_t>(handle->SeekPosition()) + offset;
				break;
			case paimon::FS_SEEK_END:
			default:
				pos = length + offset;
				break;
			}
			if (pos < 0) {
				return paimon::Status::Invalid("seek to negative position ", pos, " in '", path, "'");
			}
			handle->Seek(NumericCast<idx_t>(pos));
			return paimon::Status::OK();
		} catch (std::exception &ex) {
			return IOError("Seek", path, ex);
		}
	}

	paimon::Result<int64_t> GetPos() const override {
		std::lock_guard<std::mutex> guard(lock);
		try {
			return NumericCast<int64_t>(handle->SeekPosition());
		} catch (std::exception &ex) {
			return IOError("GetPos", path, ex);
		}
	}

	paimon::Result<int64_t> Read(char *buffer, int64_t size) override {
		std::lock_guard<std::mutex> guard(lock);
		try {
			return handle->Read(buffer, NumericCast<idx_t>(size));
		} catch (std::exception &ex) {
			return IOError("Read", path, ex);
		}
	}

	// Positional reads run lock-free: FileHandle's positional Read never touches
	// the cursor and the underlying implementations (pread) are concurrency-safe.
	// Only the seek/sequential-read cursor and Close need the mutex; paimon does
	// not close a stream with reads in flight.
	paimon::Result<int64_t> Read(char *buffer, int64_t size, int64_t offset) override {
		return PositionalRead(buffer, size, offset);
	}

	void ReadAsync(char *buffer, int64_t size, int64_t offset,
	               std::function<void(paimon::Status)> &&callback) override {
		callback(PositionalRead(buffer, size, offset).status());
	}

	paimon::Result<std::string> GetUri() const override {
		return path;
	}

	paimon::Result<int64_t> Length() const override {
		return length;
	}

private:
	//! pread contract: FileHandle's positional Read reads exactly n bytes and
	//! leaves the file offset untouched.
	paimon::Result<int64_t> PositionalRead(char *buffer, int64_t size, int64_t offset) {
		if (offset < 0 || size < 0) {
			return paimon::Status::Invalid("negative read offset/size for '", path, "'");
		}
		auto n = MinValue<int64_t>(size, length - offset);
		if (n <= 0) {
			return int64_t(0);
		}
		try {
			handle->Read(buffer, NumericCast<idx_t>(n), NumericCast<idx_t>(offset));
			return n;
		} catch (std::exception &ex) {
			return IOError("Read", path, ex);
		}
	}

	unique_ptr<FileHandle> handle;
	std::string path;
	int64_t length;
	//! Guards the seek cursor shared by Seek/GetPos/sequential Read.
	mutable std::mutex lock;
};

class DuckDBVfsOutputStream : public paimon::OutputStream {
public:
	DuckDBVfsOutputStream(unique_ptr<FileHandle> handle, std::string path)
	    : handle(std::move(handle)), path(std::move(path)) {
	}

	paimon::Status Close() override {
		if (!handle) {
			return paimon::Status::OK();
		}
		try {
			handle->Close();
			handle.reset();
			return paimon::Status::OK();
		} catch (std::exception &ex) {
			return IOError("Close", path, ex);
		}
	}

	paimon::Result<int64_t> Write(const char *buffer, int64_t size) override {
		try {
			int64_t total = 0;
			while (total < size) {
				auto n = handle->Write(const_cast<char *>(buffer) + total, NumericCast<idx_t>(size - total));
				if (n <= 0) {
					return paimon::Status::IOError("short write to '", path, "' via DuckDB file system");
				}
				total += n;
			}
			pos += total;
			return total;
		} catch (std::exception &ex) {
			return IOError("Write", path, ex);
		}
	}

	paimon::Status Flush() override {
		try {
			handle->Sync();
			return paimon::Status::OK();
		} catch (NotImplementedException &) {
			// Object stores buffer internally and flush on close.
			return paimon::Status::OK();
		} catch (std::exception &ex) {
			return IOError("Flush", path, ex);
		}
	}

	paimon::Result<int64_t> GetPos() const override {
		return pos;
	}

	paimon::Result<std::string> GetUri() const override {
		return path;
	}

private:
	unique_ptr<FileHandle> handle;
	std::string path;
	int64_t pos = 0;
};

} // namespace

DuckDBVfsFileSystem::DuckDBVfsFileSystem(DatabaseInstance &db, weak_ptr<ClientContext> context)
    : db(db), context(std::move(context)) {
}

std::shared_ptr<paimon::FileSystem> DuckDBVfsFileSystem::TryWrap(ClientContext &context, const std::string &path) {
	auto scheme_end = path.find("://");
	if (scheme_end == std::string::npos) {
		return nullptr; // no scheme: local path, handled by paimon-cpp
	}
	auto scheme = StringUtil::Lower(path.substr(0, scheme_end));
	if (scheme == "file") {
		return nullptr; // handled by paimon-cpp's local file system
	}
#ifndef DUCKDB_PAIMON_NO_JINDO
	// With JindoSDK compiled out there is no bundled oss file system; route oss://
	// through DuckDB like every other remote scheme.
	if (scheme == "oss") {
		return nullptr; // handled by paimon-cpp's bundled jindo file system
	}
#endif
	// std::make_shared: the paimon API takes std::shared_ptr, which duckdb's
	// shared_ptr does not convert to across a Derived->Base hop.
	return std::make_shared<DuckDBVfsFileSystem>(*context.db, weak_ptr<ClientContext>(context.shared_from_this()));
}

paimon::Result<std::unique_ptr<paimon::InputStream>> DuckDBVfsFileSystem::Open(const std::string &path) const {
	shared_ptr<ClientContext> ctx;
	auto &fs = Fs(ctx);
	try {
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
		auto length = NumericCast<int64_t>(handle->GetFileSize());
		std::unique_ptr<paimon::InputStream> stream = make_uniq<DuckDBVfsInputStream>(std::move(handle), path, length);
		return stream;
	} catch (std::exception &ex) {
		try {
			if (!fs.FileExists(path)) {
				return paimon::Status::NotExist("File '", path, "' not exists");
			}
		} catch (std::exception &) { // fall through to the original error
		}
		return IOError("Open", path, ex);
	}
}

paimon::Result<std::unique_ptr<paimon::OutputStream>> DuckDBVfsFileSystem::Create(const std::string &path,
                                                                                  bool overwrite) const {
	shared_ptr<ClientContext> ctx;
	auto &fs = Fs(ctx);
	try {
		if (!overwrite && fs.FileExists(path)) {
			return paimon::Status::Exist("File '", path, "' already exists");
		}
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		std::unique_ptr<paimon::OutputStream> stream = make_uniq<DuckDBVfsOutputStream>(std::move(handle), path);
		return stream;
	} catch (std::exception &ex) {
		return IOError("Create", path, ex);
	}
}

paimon::Status DuckDBVfsFileSystem::Mkdirs(const std::string &path) const {
	shared_ptr<ClientContext> ctx;
	auto &fs = Fs(ctx);
	try {
		if (fs.DirectoryExists(path)) {
			return paimon::Status::OK();
		}
		// hdfs/ofs create parents in one call; object stores treat this as a no-op
		fs.CreateDirectory(path);
		return paimon::Status::OK();
	} catch (std::exception &ex) {
		return IOError("Mkdirs", path, ex);
	}
}

paimon::Status DuckDBVfsFileSystem::Rename(const std::string &src, const std::string &dst) const {
	shared_ptr<ClientContext> ctx;
	auto &fs = Fs(ctx);
	try {
		if (!fs.FileExists(src) && !fs.DirectoryExists(src)) {
			return paimon::Status::NotExist("rename '", src, "' to '", dst, "' failed: src not exist");
		}
		fs.MoveFile(src, dst);
		return paimon::Status::OK();
	} catch (std::exception &ex) {
		return IOError("Rename", src, ex);
	}
}

paimon::Status DuckDBVfsFileSystem::Delete(const std::string &path, bool recursive) const {
	shared_ptr<ClientContext> ctx;
	auto &fs = Fs(ctx);
	try {
		if (fs.DirectoryExists(path)) {
			if (!recursive) {
				// Non-recursive delete must be atomic or a concurrent commit can
				// slip a file in and be wiped by RemoveDirectory (recursive on
				// hdfs). RemoveFile maps to a non-recursive delete on hdfs/ofs
				// and fails on a non-empty directory; the listing fallback is for
				// file systems where it cannot delete a directory at all.
				try {
					fs.RemoveFile(path);
					return paimon::Status::OK();
				} catch (std::exception &) {
				}
				bool empty = true;
				fs.ListFiles(path, [&](const string &, bool) { empty = false; });
				if (!empty) {
					return paimon::Status::IOError("cannot delete '", path, "', directory is not empty");
				}
			}
			fs.RemoveDirectory(path);
			return paimon::Status::OK();
		}
		if (fs.FileExists(path)) {
			fs.RemoveFile(path);
			return paimon::Status::OK();
		}
		return paimon::Status::NotExist("Path '", path, "' not exists");
	} catch (std::exception &ex) {
		return IOError("Delete", path, ex);
	}
}

paimon::Result<std::unique_ptr<paimon::FileStatus>> DuckDBVfsFileSystem::StatFile(duckdb::FileSystem &fs,
                                                                                  const std::string &path) const {
	try {
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
		auto len = NumericCast<int64_t>(handle->GetFileSize());
		int64_t mtime_ms = 0;
		try {
			// Through the handle's own file system: the virtual file system does
			// not route handle-based calls.
			mtime_ms = Timestamp::GetEpochMs(handle->file_system.GetLastModifiedTime(*handle));
		} catch (std::exception &) { // not every file system tracks mtime
		}
		handle->Close();
		std::unique_ptr<paimon::FileStatus> status = make_uniq<DuckDBVfsFileStatus>(path, len, false, mtime_ms);
		return status;
	} catch (std::exception &ex) {
		try {
			if (!fs.FileExists(path)) {
				return paimon::Status::NotExist("Path '", path, "' not exists");
			}
		} catch (std::exception &) {
		}
		return IOError("Stat", path, ex);
	}
}

paimon::Result<std::unique_ptr<paimon::FileStatus>> DuckDBVfsFileSystem::GetFileStatus(const std::string &path) const {
	shared_ptr<ClientContext> ctx;
	auto &fs = Fs(ctx);
	// Files first: the open carries the stat, so the common case is one round trip.
	auto status = StatFile(fs, path);
	if (status.ok()) {
		return status;
	}
	try {
		if (fs.DirectoryExists(path)) {
			std::unique_ptr<paimon::FileStatus> dir_status = make_uniq<DuckDBVfsFileStatus>(path, 0, true, 0);
			return dir_status;
		}
	} catch (std::exception &ex) {
		return IOError("GetFileStatus", path, ex);
	}
	return status.status(); // NotExist or the original stat error
}

paimon::Status
DuckDBVfsFileSystem::ListDir(const std::string &directory,
                             std::vector<std::unique_ptr<paimon::BasicFileStatus>> *file_status_list) const {
	shared_ptr<ClientContext> ctx;
	auto &fs = Fs(ctx);
	// List first; classify the failure only on the rare miss (missing dir lists
	// as empty per paimon's local-fs semantics, a file is an error).
	std::string list_error;
	try {
		if (fs.ListFiles(directory, [&](const string &name, bool is_dir) {
			    file_status_list->push_back(make_uniq<DuckDBVfsBasicFileStatus>(JoinPath(directory, name), is_dir));
		    })) {
			return paimon::Status::OK();
		}
	} catch (std::exception &ex) {
		list_error = ex.what();
	}
	try {
		if (fs.FileExists(directory)) {
			return paimon::Status::IOError("path '", directory, "' already exists and is not a directory");
		}
		if (!fs.DirectoryExists(directory)) {
			return paimon::Status::OK();
		}
	} catch (std::exception &) {
	}
	return paimon::Status::IOError("ListDir '", directory, "' failed via DuckDB file system: ", list_error);
}

paimon::Status
DuckDBVfsFileSystem::ListFileStatus(const std::string &path,
                                    std::vector<std::unique_ptr<paimon::FileStatus>> *file_status_list) const {
	shared_ptr<ClientContext> ctx;
	auto &fs = Fs(ctx);
	// The OpenFileInfo listing lets file systems with extended listing support
	// return size/mtime inline, saving a per-file stat round trip.
	std::vector<OpenFileInfo> entries;
	std::string list_error;
	bool listed = false;
	try {
		listed = fs.ListFiles(path, [&](OpenFileInfo &info) { entries.push_back(info); });
	} catch (std::exception &ex) {
		list_error = ex.what();
	}
	if (!listed) {
		// A single file lists as itself; a missing path lists as empty.
		auto status = StatFile(fs, path);
		if (status.ok()) {
			file_status_list->push_back(std::move(status).value());
			return paimon::Status::OK();
		}
		if (status.status().IsNotExist()) {
			return paimon::Status::OK();
		}
		return paimon::Status::IOError("ListFileStatus '", path, "' failed via DuckDB file system: ", list_error);
	}
	for (auto &entry : entries) {
		auto full_path = JoinPath(path, entry.path);
		bool is_dir = false;
		int64_t len = 0;
		int64_t mtime_ms = 0;
		bool have_size = false;
		if (entry.extended_info) {
			auto &options = entry.extended_info->options;
			auto type = options.find("type");
			is_dir = type != options.end() && StringValue::Get(type->second) == "directory";
			auto size = options.find("file_size");
			if (size != options.end()) {
				len = size->second.GetValue<int64_t>();
				have_size = true;
			}
			auto modified = options.find("last_modified");
			if (modified != options.end()) {
				mtime_ms = Timestamp::GetEpochMs(modified->second.GetValue<timestamp_t>());
			}
		}
		if (is_dir) {
			file_status_list->push_back(make_uniq<DuckDBVfsFileStatus>(full_path, 0, true, 0));
			continue;
		}
		if (have_size) {
			file_status_list->push_back(make_uniq<DuckDBVfsFileStatus>(full_path, len, false, mtime_ms));
			continue;
		}
		auto status = StatFile(fs, full_path);
		if (!status.ok()) {
			if (status.status().IsNotExist()) {
				continue; // raced with a concurrent delete
			}
			return status.status();
		}
		file_status_list->push_back(std::move(status).value());
	}
	return paimon::Status::OK();
}

paimon::Result<bool> DuckDBVfsFileSystem::Exists(const std::string &path) const {
	shared_ptr<ClientContext> ctx;
	auto &fs = Fs(ctx);
	try {
		return fs.FileExists(path) || fs.DirectoryExists(path);
	} catch (std::exception &ex) {
		return IOError("Exists", path, ex);
	}
}

} // namespace duckdb

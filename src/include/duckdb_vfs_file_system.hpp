/*-------------------------------------------------------------------------
 *
 * duckdb_vfs_file_system.hpp
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
 *	  src/include/duckdb_vfs_file_system.hpp
 *
 *-------------------------------------------------------------------------
 */

#pragma once

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include "paimon/fs/file_system.h"

#include <memory>
#include <string>
#include <vector>

namespace duckdb {

//! paimon::FileSystem adapter over DuckDB's virtual file system. Routes all
//! paimon-cpp I/O for schemes the bundled paimon file systems cannot handle
//! (hdfs://, s3://, ...) through whatever file system is registered in DuckDB.
class DuckDBVfsFileSystem : public paimon::FileSystem {
public:
	DuckDBVfsFileSystem(DatabaseInstance &db, weak_ptr<ClientContext> context);

	//! Returns an adapter for paths whose scheme is not handled by the
	//! bundled paimon-cpp file systems (local, jindo/oss), nullptr otherwise.
	//! The result is meant to be passed to paimon::Catalog::Create or the
	//! context builders' WithFileSystem(); nullptr keeps stock behavior.
	static std::shared_ptr<paimon::FileSystem> TryWrap(ClientContext &context, const std::string &path);

	paimon::Result<std::unique_ptr<paimon::InputStream>> Open(const std::string &path) const override;
	paimon::Result<std::unique_ptr<paimon::OutputStream>> Create(const std::string &path,
	                                                             bool overwrite) const override;
	paimon::Status Mkdirs(const std::string &path) const override;
	paimon::Status Rename(const std::string &src, const std::string &dst) const override;
	paimon::Status Delete(const std::string &path, bool recursive) const override;
	paimon::Result<std::unique_ptr<paimon::FileStatus>> GetFileStatus(const std::string &path) const override;
	paimon::Status ListDir(const std::string &directory,
	                       std::vector<std::unique_ptr<paimon::BasicFileStatus>> *file_status_list) const override;
	paimon::Status ListFileStatus(const std::string &path,
	                              std::vector<std::unique_ptr<paimon::FileStatus>> *file_status_list) const override;
	paimon::Result<bool> Exists(const std::string &path) const override;

private:
	//! Client-level file system while the wrapping statement's context is alive
	//! (session-scoped settings and secrets are visible only there), database-level
	//! afterwards. The holder keeps the context alive for the duration of one call.
	duckdb::FileSystem &Fs(shared_ptr<ClientContext> &context_holder) const {
		context_holder = context.lock();
		return context_holder ? duckdb::FileSystem::GetFileSystem(*context_holder) : db.GetFileSystem();
	}
	//! Stat a single path known to be a regular file.
	paimon::Result<std::unique_ptr<paimon::FileStatus>> StatFile(duckdb::FileSystem &fs, const std::string &path) const;

	//! By reference: the adapter is stored inside attached catalogs and bind data,
	//! all owned by the instance; a shared_ptr here would form an ownership cycle
	//! (DatabaseInstance -> catalog -> adapter -> DatabaseInstance) that leaks the
	//! whole instance on close.
	DatabaseInstance &db;
	weak_ptr<ClientContext> context;
};

} // namespace duckdb

/*-------------------------------------------------------------------------
 *
 * paimon_insert.hpp
 *
 * Copyright (c) 2026, Alibaba Group Holding Limited
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
 *	  src/include/paimon_insert.hpp
 *
 *-------------------------------------------------------------------------
 */

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"

#include <map>
#include <memory>
#include <string>

namespace paimon {
class Schema;
} // namespace paimon

namespace duckdb {

class SchemaCatalogEntry;

//! Partition write info, resolved at plan time so the sink needs no catalog access.
struct PaimonPartitionInfo {
	//! Partition key names, in Paimon schema order.
	vector<string> part_key_names;
	//! Chunk column index of each partition key (chunk order == Paimon field order).
	vector<idx_t> part_col_idxs;
	//! Directory name for NULL partition values; empty keeps the Paimon default.
	string null_part_name;
};

class PhysicalPaimonInsert : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	PhysicalPaimonInsert(PhysicalPlan &physical_plan, LogicalOperator &op, SchemaCatalogEntry &schema,
	                     unique_ptr<BoundCreateTableInfo> info, string table_path, map<string, string> paimon_options,
	                     PaimonPartitionInfo partition_info, idx_t estimated_cardinality);

	//! Resolve partition write info from a loaded Paimon table schema.
	static PaimonPartitionInfo ResolvePartitionInfo(const std::shared_ptr<paimon::Schema> &table_schema);

	SchemaCatalogEntry *schema;
	unique_ptr<BoundCreateTableInfo> info;
	string table_path;
	map<string, string> paimon_options;
	PaimonPartitionInfo partition_info;

public:
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return true;
	}
	bool SinkOrderDependent() const override {
		return false;
	}

	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;

	bool IsSource() const override {
		return true;
	}
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
};

} // namespace duckdb

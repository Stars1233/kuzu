#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "planner/operator/logical_partitioner.h"
#include "planner/operator/persistent/logical_copy_from.h"
#include "processor/expression_mapper.h"
#include "processor/operator/index_lookup.h"
#include "processor/operator/partitioner.h"
#include "processor/operator/persistent/copy_rel_batch_insert.h"
#include "processor/operator/persistent/node_batch_insert.h"
#include "processor/operator/persistent/rel_batch_insert.h"
#include "processor/operator/table_function_call.h"
#include "processor/plan_mapper.h"
#include "processor/result/factorized_table_util.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "storage/table/rel_table.h"

using namespace kuzu::binder;
using namespace kuzu::catalog;
using namespace kuzu::common;
using namespace kuzu::planner;
using namespace kuzu::storage;

namespace kuzu {
namespace processor {

std::unique_ptr<PhysicalOperator> PlanMapper::createRelBatchInsertOp(
    const main::ClientContext* clientContext,
    std::shared_ptr<PartitionerSharedState> partitionerSharedState,
    std::shared_ptr<BatchInsertSharedState> sharedState, const BoundCopyFromInfo& copyFromInfo,
    Schema* outFSchema, RelDataDirection direction, table_id_t fromTableID, table_id_t toTableID,
    std::vector<column_id_t> columnIDs, std::vector<LogicalType> columnTypes, uint32_t operatorID,
    std::unique_ptr<RelBatchInsertImpl> impl) {
    auto partitioningIdx = direction == RelDataDirection::FWD ? 0 : 1;
    auto offsetVectorIdx = direction == RelDataDirection::FWD ? 0 : 1;
    const auto numWarningDataColumns = copyFromInfo.getNumWarningColumns();
    // TODO(Xiyang): rewrite me
    KU_ASSERT(numWarningDataColumns <= copyFromInfo.columnExprs.size());
    for (column_id_t i = numWarningDataColumns; i >= 1; --i) {
        columnTypes.push_back(
            copyFromInfo.columnExprs[copyFromInfo.columnExprs.size() - i]->getDataType().copy());
    }
    auto tableName = copyFromInfo.tableName;
    auto compressionEnabled = clientContext->getStorageManager()->compressionEnabled();
    auto relBatchInsertInfo = std::make_unique<RelBatchInsertInfo>(tableName, compressionEnabled,
        direction, fromTableID, toTableID, partitioningIdx, offsetVectorIdx, std::move(columnIDs),
        std::move(columnTypes), numWarningDataColumns);
    auto printInfo = std::make_unique<RelBatchInsertPrintInfo>(tableName);
    auto batchInsert = std::make_unique<RelBatchInsert>(tableName, std::move(relBatchInsertInfo),
        std::move(partitionerSharedState), std::move(sharedState), operatorID, std::move(printInfo),
        nullptr, std::move(impl));
    batchInsert->setDescriptor(std::make_unique<ResultSetDescriptor>(outFSchema));
    return batchInsert;
}

std::unique_ptr<PhysicalOperator> PlanMapper::mapCopyFrom(const LogicalOperator* logicalOperator) {
    const auto& copyFrom = logicalOperator->constCast<LogicalCopyFrom>();
    clientContext->getWarningContextUnsafe().setIgnoreErrorsForCurrentQuery(
        copyFrom.getInfo()->getIgnoreErrorsOption());
    switch (copyFrom.getInfo()->tableType) {
    case TableType::NODE: {
        return mapCopyNodeFrom(logicalOperator);
    }
    case TableType::REL: {
        return mapCopyRelFrom(logicalOperator);
    }
    default:
        KU_UNREACHABLE;
    }
}

std::unique_ptr<PhysicalOperator> PlanMapper::mapCopyNodeFrom(
    const LogicalOperator* logicalOperator) {
    const auto storageManager = clientContext->getStorageManager();
    auto& copyFrom = logicalOperator->constCast<LogicalCopyFrom>();
    const auto copyFromInfo = copyFrom.getInfo();
    const auto outFSchema = copyFrom.getSchema();

    auto prevOperator = mapOperator(copyFrom.getChild(0).get());
    auto fTable =
        FactorizedTableUtils::getSingleStringColumnFTable(clientContext->getMemoryManager());

    auto sharedState = std::make_shared<NodeBatchInsertSharedState>(fTable,
        &storageManager->getWAL(), clientContext->getMemoryManager());
    if (prevOperator->getOperatorType() == PhysicalOperatorType::TABLE_FUNCTION_CALL) {
        const auto call = prevOperator->ptrCast<TableFunctionCall>();
        sharedState->tableFuncSharedState = call->getSharedState().get();
    }

    std::vector<LogicalType> columnTypes;
    std::vector<std::unique_ptr<evaluator::ExpressionEvaluator>> columnEvaluators;
    auto exprMapper = ExpressionMapper(outFSchema);
    for (auto& expr : copyFromInfo->columnExprs) {
        columnTypes.push_back(expr->getDataType().copy());
        columnEvaluators.push_back(exprMapper.getEvaluator(expr));
    }
    const auto numWarningDataColumns = copyFromInfo->source->getNumWarningDataColumns();
    KU_ASSERT(columnTypes.size() >= numWarningDataColumns);
    auto tableName = copyFromInfo->tableName;
    auto info = std::make_unique<NodeBatchInsertInfo>(tableName,
        storageManager->compressionEnabled(), std::move(columnTypes), std::move(columnEvaluators),
        copyFromInfo->columnEvaluateTypes, numWarningDataColumns);
    auto printInfo = std::make_unique<NodeBatchInsertPrintInfo>(tableName);
    auto batchInsert = std::make_unique<NodeBatchInsert>(tableName, std::move(info),
        std::move(sharedState), std::move(prevOperator), getOperatorID(), std::move(printInfo));
    batchInsert->setDescriptor(std::make_unique<ResultSetDescriptor>(copyFrom.getSchema()));
    return batchInsert;
}

std::unique_ptr<PhysicalOperator> PlanMapper::mapPartitioner(
    const LogicalOperator* logicalOperator) {
    auto& logicalPartitioner = logicalOperator->constCast<LogicalPartitioner>();
    auto prevOperator = mapOperator(logicalPartitioner.getChild(0).get());
    auto outFSchema = logicalPartitioner.getSchema();
    auto& copyFromInfo = logicalPartitioner.copyFromInfo;
    auto& extraInfo = copyFromInfo.extraInfo->constCast<ExtraBoundCopyRelInfo>();
    PartitionerInfo partitionerInfo;
    partitionerInfo.relOffsetDataPos =
        getDataPos(*logicalPartitioner.getInfo().offset, *outFSchema);
    partitionerInfo.infos.reserve(logicalPartitioner.getInfo().getNumInfos());
    for (auto i = 0u; i < logicalPartitioner.getInfo().getNumInfos(); i++) {
        partitionerInfo.infos.emplace_back(logicalPartitioner.getInfo().getInfo(i).keyIdx,
            PartitionerFunctions::partitionRelData);
    }
    std::vector<LogicalType> columnTypes;
    evaluator::evaluator_vector_t columnEvaluators;
    auto exprMapper = ExpressionMapper(outFSchema);
    for (auto& expr : copyFromInfo.columnExprs) {
        columnTypes.push_back(expr->getDataType().copy());
        columnEvaluators.push_back(exprMapper.getEvaluator(expr));
    }
    for (auto idx : extraInfo.internalIDColumnIndices) {
        columnTypes[idx] = LogicalType::INTERNAL_ID();
    }
    auto dataInfo = PartitionerDataInfo(copyFromInfo.tableName, extraInfo.fromTableName,
        extraInfo.toTableName, LogicalType::copy(columnTypes), std::move(columnEvaluators),
        copyFromInfo.columnEvaluateTypes);
    auto sharedState =
        std::make_shared<CopyPartitionerSharedState>(*clientContext->getMemoryManager());
    expression_vector expressions;
    for (auto& info : partitionerInfo.infos) {
        expressions.push_back(copyFromInfo.columnExprs[info.keyIdx]);
    }
    auto printInfo = std::make_unique<PartitionerPrintInfo>(expressions);
    auto partitioner =
        std::make_unique<Partitioner>(std::move(partitionerInfo), std::move(dataInfo),
            std::move(sharedState), std::move(prevOperator), getOperatorID(), std::move(printInfo));
    partitioner->setDescriptor(std::make_unique<ResultSetDescriptor>(outFSchema));
    return partitioner;
}

std::unique_ptr<PhysicalOperator> PlanMapper::mapCopyRelFrom(
    const LogicalOperator* logicalOperator) {
    auto& copyFrom = logicalOperator->constCast<LogicalCopyFrom>();
    const auto copyFromInfo = copyFrom.getInfo();
    auto partitioner = mapOperator(copyFrom.getChild(0).get());
    KU_ASSERT(partitioner->getOperatorType() == PhysicalOperatorType::PARTITIONER);
    const auto sharedState = partitioner->ptrCast<Partitioner>()->getSharedState();
    const auto storageManager = clientContext->getStorageManager();
    const auto catalog = clientContext->getCatalog();
    const auto transaction = clientContext->getTransaction();
    auto extraInfo = copyFromInfo->extraInfo->constCast<ExtraBoundCopyRelInfo>();
    auto fromTableID =
        catalog->getTableCatalogEntry(transaction, extraInfo.fromTableName)->getTableID();
    auto toTableID =
        catalog->getTableCatalogEntry(transaction, extraInfo.toTableName)->getTableID();
    auto fTable =
        FactorizedTableUtils::getSingleStringColumnFTable(clientContext->getMemoryManager());
    const auto batchInsertSharedState = std::make_shared<BatchInsertSharedState>(nullptr, fTable,
        &storageManager->getWAL(), clientContext->getMemoryManager());
    // If the table entry doesn't exist, assume both directions
    std::vector directions = {RelDataDirection::FWD, RelDataDirection::BWD};
    if (catalog->containsTable(transaction, copyFromInfo->tableName)) {
        const auto& relGroupEntry =
            catalog->getTableCatalogEntry(transaction, copyFromInfo->tableName)
                ->constCast<RelGroupCatalogEntry>();
        directions = relGroupEntry.getRelDataDirections();
    }
    auto sink = std::make_unique<DummySimpleSink>(fTable, getOperatorID());
    for (auto direction : directions) {
        auto copyRel = createRelBatchInsertOp(clientContext, sharedState, batchInsertSharedState,
            *copyFrom.getInfo(), copyFrom.getSchema(), direction, fromTableID, toTableID, {}, {},
            getOperatorID(), std::make_unique<CopyRelBatchInsert>());
        sink->addChild(std::move(copyRel));
    }
    sink->addChild(std::move(partitioner));
    return sink;
}

} // namespace processor
} // namespace kuzu

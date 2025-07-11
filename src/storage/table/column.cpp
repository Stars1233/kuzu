#include "storage/table/column.h"

#include <algorithm>
#include <cstdint>

#include "common/assert.h"
#include "common/null_mask.h"
#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/compression/compression.h"
#include "storage/file_handle.h"
#include "storage/page_manager.h"
#include "storage/storage_utils.h"
#include "storage/table/column_chunk.h"
#include "storage/table/column_chunk_data.h"
#include "storage/table/list_column.h"
#include "storage/table/null_column.h"
#include "storage/table/string_column.h"
#include "storage/table/struct_column.h"
#include <bit>

using namespace kuzu::catalog;
using namespace kuzu::common;
using namespace kuzu::transaction;
using namespace kuzu::evaluator;

namespace kuzu {
namespace storage {

struct ReadInternalIDValuesToVector {
    ReadInternalIDValuesToVector() : compressedReader{LogicalType(LogicalTypeID::INTERNAL_ID)} {}
    void operator()(const uint8_t* frame, PageCursor& pageCursor, ValueVector* resultVector,
        uint32_t posInVector, uint32_t numValuesToRead, const CompressionMetadata& metadata) {
        KU_ASSERT(resultVector->dataType.getPhysicalType() == PhysicalTypeID::INTERNAL_ID);

        KU_ASSERT(numValuesToRead <= DEFAULT_VECTOR_CAPACITY);
        offset_t offsetBuffer[DEFAULT_VECTOR_CAPACITY];

        compressedReader(frame, pageCursor, reinterpret_cast<uint8_t*>(offsetBuffer), 0,
            numValuesToRead, metadata);
        auto resultData = reinterpret_cast<internalID_t*>(resultVector->getData());
        for (auto i = 0u; i < numValuesToRead; i++) {
            resultData[posInVector + i].offset = offsetBuffer[i];
        }
    }

private:
    ReadCompressedValuesFromPage compressedReader;
};

struct WriteInternalIDValuesToPage {
    WriteInternalIDValuesToPage() : compressedWriter{LogicalType(LogicalTypeID::INTERNAL_ID)} {}
    void operator()(uint8_t* frame, uint16_t posInFrame, const uint8_t* data, uint32_t dataOffset,
        offset_t numValues, const CompressionMetadata& metadata, const NullMask* nullMask) {
        compressedWriter(frame, posInFrame, data, dataOffset, numValues, metadata, nullMask);
    }
    void operator()(uint8_t* frame, uint16_t posInFrame, ValueVector* vector,
        uint32_t offsetInVector, offset_t numValues, const CompressionMetadata& metadata) {
        KU_ASSERT(vector->dataType.getPhysicalType() == PhysicalTypeID::INTERNAL_ID);
        compressedWriter(frame, posInFrame,
            reinterpret_cast<const uint8_t*>(
                &vector->getValue<internalID_t>(offsetInVector).offset),
            0 /*dataOffset*/, numValues, metadata);
    }

private:
    WriteCompressedValuesToPage compressedWriter;
};

static read_values_to_vector_func_t getReadValuesToVectorFunc(const LogicalType& logicalType) {
    switch (logicalType.getLogicalTypeID()) {
    case LogicalTypeID::INTERNAL_ID:
        return ReadInternalIDValuesToVector();
    default:
        return ReadCompressedValuesFromPageToVector(logicalType);
    }
}

static write_values_func_t getWriteValuesFunc(const LogicalType& logicalType) {
    switch (logicalType.getLogicalTypeID()) {
    case LogicalTypeID::INTERNAL_ID:
        return WriteInternalIDValuesToPage();
    default:
        return WriteCompressedValuesToPage(logicalType);
    }
}

InternalIDColumn::InternalIDColumn(std::string name, FileHandle* dataFH, MemoryManager* mm,
    ShadowFile* shadowFile, bool enableCompression)
    : Column{std::move(name), LogicalType::INTERNAL_ID(), dataFH, mm, shadowFile, enableCompression,
          false /*requireNullColumn*/},
      commonTableID{INVALID_TABLE_ID} {}

void InternalIDColumn::populateCommonTableID(const ValueVector* resultVector) const {
    auto nodeIDs = reinterpret_cast<internalID_t*>(resultVector->getData());
    auto& selVector = resultVector->state->getSelVector();
    for (auto i = 0u; i < selVector.getSelSize(); i++) {
        const auto pos = selVector[i];
        nodeIDs[pos].tableID = commonTableID;
    }
}

Column::Column(std::string name, LogicalType dataType, FileHandle* dataFH, MemoryManager* mm,
    ShadowFile* shadowFile, bool enableCompression, bool requireNullColumn)
    : name{std::move(name)}, dataType{std::move(dataType)}, mm{mm}, dataFH(dataFH),
      shadowFile(shadowFile), enableCompression{enableCompression},
      columnReadWriter(ColumnReadWriterFactory::createColumnReadWriter(
          this->dataType.getPhysicalType(), dataFH, shadowFile)) {
    readToVectorFunc = getReadValuesToVectorFunc(this->dataType);
    readToPageFunc = ReadCompressedValuesFromPage(this->dataType);
    writeFunc = getWriteValuesFunc(this->dataType);
    if (requireNullColumn) {
        auto columnName =
            StorageUtils::getColumnName(this->name, StorageUtils::ColumnType::NULL_MASK, "");
        nullColumn =
            std::make_unique<NullColumn>(columnName, dataFH, mm, shadowFile, enableCompression);
    }
}

Column::Column(std::string name, PhysicalTypeID physicalType, FileHandle* dataFH, MemoryManager* mm,
    ShadowFile* shadowFile, bool enableCompression, bool requireNullColumn)
    : Column(std::move(name), LogicalType::ANY(physicalType), dataFH, mm, shadowFile,
          enableCompression, requireNullColumn) {}

Column::~Column() = default;

Column* Column::getNullColumn() const {
    return nullColumn.get();
}

void Column::populateExtraChunkState(ChunkState& state) const {
    if (state.metadata.compMeta.compression == CompressionType::ALP) {
        if (dataType.getPhysicalType() == PhysicalTypeID::DOUBLE) {
            state.alpExceptionChunk =
                std::make_unique<InMemoryExceptionChunk<double>>(state, dataFH, mm, shadowFile);
        } else if (dataType.getPhysicalType() == PhysicalTypeID::FLOAT) {
            state.alpExceptionChunk =
                std::make_unique<InMemoryExceptionChunk<float>>(state, dataFH, mm, shadowFile);
        }
    }
}

std::unique_ptr<ColumnChunkData> Column::flushChunkData(const ColumnChunkData& chunkData,
    PageAllocator& pageAllocator) {
    switch (chunkData.getDataType().getPhysicalType()) {
    case PhysicalTypeID::STRUCT: {
        return StructColumn::flushChunkData(chunkData, pageAllocator);
    }
    case PhysicalTypeID::STRING: {
        return StringColumn::flushChunkData(chunkData, pageAllocator);
    }
    case PhysicalTypeID::ARRAY:
    case PhysicalTypeID::LIST: {
        return ListColumn::flushChunkData(chunkData, pageAllocator);
    }
    default: {
        return flushNonNestedChunkData(chunkData, pageAllocator);
    }
    }
}

std::unique_ptr<ColumnChunkData> Column::flushNonNestedChunkData(const ColumnChunkData& chunkData,
    PageAllocator& pageAllocator) {
    auto chunkMeta = flushData(chunkData, pageAllocator);
    auto flushedChunk = ColumnChunkFactory::createColumnChunkData(chunkData.getMemoryManager(),
        chunkData.getDataType().copy(), chunkData.isCompressionEnabled(), chunkMeta,
        chunkData.hasNullData(), true);
    if (chunkData.hasNullData()) {
        auto nullChunkMeta = flushData(chunkData.getNullData(), pageAllocator);
        auto nullData = std::make_unique<NullChunkData>(chunkData.getMemoryManager(),
            chunkData.isCompressionEnabled(), nullChunkMeta);
        flushedChunk->setNullData(std::move(nullData));
    }
    return flushedChunk;
}

ColumnChunkMetadata Column::flushData(const ColumnChunkData& chunkData,
    PageAllocator& pageAllocator) {
    KU_ASSERT(chunkData.sanityCheck());
    const auto preScanMetadata = chunkData.getMetadataToFlush();
    auto allocatedBlock = pageAllocator.allocatePageRange(preScanMetadata.getNumPages());
    return chunkData.flushBuffer(pageAllocator, allocatedBlock, preScanMetadata);
}

void Column::scan(const ChunkState& state, offset_t startOffsetInChunk, row_idx_t numValuesToScan,
    ValueVector* resultVector) const {
    if (nullColumn) {
        KU_ASSERT(state.nullState);
        nullColumn->scan(*state.nullState, startOffsetInChunk, numValuesToScan, resultVector);
    }
    scanInternal(state, startOffsetInChunk, numValuesToScan, resultVector);
}

void Column::scan(const ChunkState& state, offset_t startOffsetInGroup, offset_t endOffsetInGroup,
    ValueVector* resultVector, uint64_t offsetInVector) const {
    if (nullColumn) {
        KU_ASSERT(state.nullState);
        nullColumn->scan(*state.nullState, startOffsetInGroup, endOffsetInGroup, resultVector,
            offsetInVector);
    }
    columnReadWriter->readCompressedValuesToVector(state, resultVector, offsetInVector,
        startOffsetInGroup, endOffsetInGroup, readToVectorFunc);
}

void Column::scan(const ChunkState& state, ColumnChunkData* columnChunk, offset_t startOffset,
    offset_t endOffset) const {
    if (nullColumn) {
        nullColumn->scan(*state.nullState, columnChunk->getNullData(), startOffset, endOffset);
    }

    startOffset = std::min(startOffset, state.metadata.numValues);
    endOffset = std::min(endOffset, state.metadata.numValues);
    KU_ASSERT(endOffset >= startOffset);
    const auto numValuesToScan = endOffset - startOffset;
    if (numValuesToScan > columnChunk->getCapacity()) {
        columnChunk->resizeWithoutPreserve(std::bit_ceil(numValuesToScan));
    }
    if (getDataTypeSizeInChunk(dataType) == 0) {
        columnChunk->setNumValues(numValuesToScan);
        return;
    }

    KU_ASSERT((numValuesToScan + startOffset) <= state.metadata.numValues);
    const uint64_t numValuesScanned = columnReadWriter->readCompressedValuesToPage(state,
        columnChunk->getData(), 0, startOffset, endOffset, readToPageFunc);

    columnChunk->setNumValues(numValuesScanned);
}

void Column::scan(const ChunkState& state, offset_t startOffsetInGroup, offset_t endOffsetInGroup,
    uint8_t* result) {
    columnReadWriter->readCompressedValuesToPage(state, result, 0, startOffsetInGroup,
        endOffsetInGroup, readToPageFunc);
}

void Column::scanInternal(const ChunkState& state, offset_t startOffsetInChunk,
    row_idx_t numValuesToScan, ValueVector* resultVector) const {
    if (resultVector->state->getSelVector().isUnfiltered()) {
        columnReadWriter->readCompressedValuesToVector(state, resultVector, 0, startOffsetInChunk,
            startOffsetInChunk + numValuesToScan, readToVectorFunc);
    } else {
        struct Filterer {
            explicit Filterer(const SelectionVector& selVector)
                : selVector(selVector), posInSelVector(0) {}
            bool operator()(offset_t startIdx, offset_t endIdx) {
                while (posInSelVector < selVector.getSelSize() &&
                       selVector[posInSelVector] < startIdx) {
                    posInSelVector++;
                }
                return (posInSelVector < selVector.getSelSize() &&
                        isInRange(selVector[posInSelVector], startIdx, endIdx));
            }

            const SelectionVector& selVector;
            offset_t posInSelVector;
        };

        columnReadWriter->readCompressedValuesToVector(state, resultVector, 0, startOffsetInChunk,
            startOffsetInChunk + numValuesToScan, readToVectorFunc,
            Filterer{resultVector->state->getSelVector()});
    }
}

void Column::lookupValue(const ChunkState& state, offset_t nodeOffset, ValueVector* resultVector,
    uint32_t posInVector) const {
    if (nullColumn) {
        nullColumn->lookupValue(*state.nullState, nodeOffset, resultVector, posInVector);
    }
    if (resultVector->isNull(posInVector)) {
        return;
    }
    lookupInternal(state, nodeOffset, resultVector, posInVector);
}

void Column::lookupInternal(const ChunkState& state, offset_t nodeOffset, ValueVector* resultVector,
    uint32_t posInVector) const {
    columnReadWriter->readCompressedValueToVector(state, nodeOffset, resultVector, posInVector,
        readToVectorFunc);
}

[[maybe_unused]] static bool sanityCheckForWrites(const ColumnChunkMetadata& metadata,
    const LogicalType& dataType) {
    if (metadata.compMeta.compression == CompressionType::ALP) {
        return metadata.compMeta.children.size() != 0;
    }
    if (metadata.compMeta.compression == CompressionType::CONSTANT) {
        return metadata.getNumDataPages(dataType.getPhysicalType()) == 0;
    }
    const auto numValuesPerPage = metadata.compMeta.numValues(KUZU_PAGE_SIZE, dataType);
    if (numValuesPerPage == UINT64_MAX) {
        return metadata.getNumDataPages(dataType.getPhysicalType()) == 0;
    }
    return std::ceil(
               static_cast<double>(metadata.numValues) / static_cast<double>(numValuesPerPage)) <=
           metadata.getNumDataPages(dataType.getPhysicalType());
}

void Column::updateStatistics(ColumnChunkMetadata& metadata, offset_t maxIndex,
    const std::optional<StorageValue>& min, const std::optional<StorageValue>& max) const {
    if (maxIndex >= metadata.numValues) {
        metadata.numValues = maxIndex + 1;
        KU_ASSERT(sanityCheckForWrites(metadata, dataType));
    }
    // Either both or neither should be provided
    KU_ASSERT((!min && !max) || (min && max));
    if (min && max) {
        // FIXME(bmwinger): this is causing test failures
        // If new values are outside of the existing min/max, update them
        if (max->gt(metadata.compMeta.max, dataType.getPhysicalType())) {
            metadata.compMeta.max = *max;
        } else if (metadata.compMeta.min.gt(*min, dataType.getPhysicalType())) {
            metadata.compMeta.min = *min;
        }
    }
}

void Column::write(ColumnChunkData& persistentChunk, ChunkState& state, offset_t dstOffset,
    ColumnChunkData* data, offset_t srcOffset, length_t numValues) {
    std::optional<NullMask> nullMask = data->getNullMask();
    NullMask* nullMaskPtr = nullptr;
    if (nullMask) {
        nullMaskPtr = &*nullMask;
    }
    writeValues(state, dstOffset, data->getData(), nullMaskPtr, srcOffset, numValues);

    if (dataType.getPhysicalType() != PhysicalTypeID::ALP_EXCEPTION_DOUBLE &&
        dataType.getPhysicalType() != PhysicalTypeID::ALP_EXCEPTION_FLOAT) {
        auto [minWritten, maxWritten] = getMinMaxStorageValue(data->getData(), srcOffset, numValues,
            dataType.getPhysicalType(), nullMaskPtr);
        updateStatistics(persistentChunk.getMetadata(), dstOffset + numValues - 1, minWritten,
            maxWritten);
    }
}

void Column::writeValues(ChunkState& state, offset_t dstOffset, const uint8_t* data,
    const NullMask* nullChunkData, offset_t srcOffset, offset_t numValues) const {
    columnReadWriter->writeValuesToPageFromBuffer(state, dstOffset, data, nullChunkData, srcOffset,
        numValues, writeFunc);
}

// Append to the end of the chunk.
offset_t Column::appendValues(ColumnChunkData& persistentChunk, ChunkState& state,
    const uint8_t* data, const NullMask* nullChunkData, offset_t numValues) {
    auto& metadata = persistentChunk.getMetadata();
    const auto startOffset = metadata.numValues;
    writeValues(state, metadata.numValues, data, nullChunkData, 0 /*dataOffset*/, numValues);

    auto [minWritten, maxWritten] = getMinMaxStorageValue(data, 0 /*offset*/, numValues,
        dataType.getPhysicalType(), nullChunkData);
    updateStatistics(metadata, startOffset + numValues - 1, minWritten, maxWritten);
    return startOffset;
}

bool Column::isEndOffsetOutOfPagesCapacity(const ColumnChunkMetadata& metadata,
    offset_t endOffset) const {
    if (metadata.compMeta.compression != CompressionType::CONSTANT &&
        (metadata.compMeta.numValues(KUZU_PAGE_SIZE, dataType) *
            metadata.getNumDataPages(dataType.getPhysicalType())) <= endOffset) {
        // Note that for constant compression, `metadata.numPages` will be equal to 0.
        // Thus, this function will always return true.
        return true;
    }
    return false;
}

void Column::checkpointColumnChunkInPlace(ChunkState& state,
    const ColumnCheckpointState& checkpointState, PageAllocator& pageAllocator) {
    for (auto& chunkCheckpointState : checkpointState.chunkCheckpointStates) {
        KU_ASSERT(chunkCheckpointState.numRows > 0);
        write(checkpointState.persistentData, state, chunkCheckpointState.startRow,
            chunkCheckpointState.chunkData.get(), 0 /*srcOffset*/, chunkCheckpointState.numRows);
    }
    checkpointState.persistentData.resetNumValuesFromMetadata();
    if (nullColumn) {
        checkpointNullData(checkpointState, pageAllocator);
    }
}

void Column::checkpointNullData(const ColumnCheckpointState& checkpointState,
    PageAllocator& pageAllocator) const {
    std::vector<ChunkCheckpointState> nullChunkCheckpointStates;
    for (const auto& chunkCheckpointState : checkpointState.chunkCheckpointStates) {
        KU_ASSERT(chunkCheckpointState.chunkData->hasNullData());
        ChunkCheckpointState nullChunkCheckpointState(
            chunkCheckpointState.chunkData->moveNullData(), chunkCheckpointState.startRow,
            chunkCheckpointState.numRows);
        nullChunkCheckpointStates.push_back(std::move(nullChunkCheckpointState));
    }
    KU_ASSERT(checkpointState.persistentData.hasNullData());
    ColumnCheckpointState nullColumnCheckpointState(*checkpointState.persistentData.getNullData(),
        std::move(nullChunkCheckpointStates));
    nullColumn->checkpointColumnChunk(nullColumnCheckpointState, pageAllocator);
}

void Column::checkpointColumnChunkOutOfPlace(const ChunkState& state,
    const ColumnCheckpointState& checkpointState, PageAllocator& pageAllocator) const {
    const auto numRows = std::max(checkpointState.endRowIdxToWrite, state.metadata.numValues);
    checkpointState.persistentData.setToInMemory();
    checkpointState.persistentData.resize(numRows);
    scan(state, &checkpointState.persistentData);
    state.reclaimAllocatedPages(pageAllocator);
    for (auto& chunkCheckpointState : checkpointState.chunkCheckpointStates) {
        checkpointState.persistentData.write(chunkCheckpointState.chunkData.get(), 0 /*srcOffset*/,
            chunkCheckpointState.startRow, chunkCheckpointState.numRows);
    }
    checkpointState.persistentData.finalize();
    checkpointState.persistentData.flush(pageAllocator);
}

bool Column::canCheckpointInPlace(const ChunkState& state,
    const ColumnCheckpointState& checkpointState) {
    if (isEndOffsetOutOfPagesCapacity(checkpointState.persistentData.getMetadata(),
            checkpointState.endRowIdxToWrite)) {
        return false;
    }
    if (checkpointState.persistentData.getMetadata().compMeta.canAlwaysUpdateInPlace()) {
        return true;
    }

    InPlaceUpdateLocalState localUpdateState{};
    for (auto& chunkCheckpointState : checkpointState.chunkCheckpointStates) {
        auto& chunkData = chunkCheckpointState.chunkData;
        KU_ASSERT(chunkData->getNumValues() == chunkCheckpointState.numRows);
        if (chunkData->getNumValues() != 0 &&
            !state.metadata.compMeta.canUpdateInPlace(chunkData->getData(), 0,
                chunkData->getNumValues(), dataType.getPhysicalType(), localUpdateState,
                chunkData->getNullMask())) {
            return false;
        }
    }
    return true;
}

void Column::checkpointColumnChunk(ColumnCheckpointState& checkpointState,
    PageAllocator& pageAllocator) {
    ChunkState chunkState;
    checkpointState.persistentData.initializeScanState(chunkState, this);
    if (canCheckpointInPlace(chunkState, checkpointState)) {
        checkpointColumnChunkInPlace(chunkState, checkpointState, pageAllocator);

        if (chunkState.metadata.compMeta.compression == CompressionType::ALP) {
            if (dataType.getPhysicalType() == PhysicalTypeID::DOUBLE) {
                chunkState.getExceptionChunk<double>()->finalizeAndFlushToDisk(chunkState);
            } else if (dataType.getPhysicalType() == PhysicalTypeID::FLOAT) {
                chunkState.getExceptionChunk<float>()->finalizeAndFlushToDisk(chunkState);
            } else {
                KU_UNREACHABLE;
            }
            checkpointState.persistentData.getMetadata().compMeta.floatMetadata()->exceptionCount =
                chunkState.metadata.compMeta.floatMetadata()->exceptionCount;
        }
    } else {
        checkpointColumnChunkOutOfPlace(chunkState, checkpointState, pageAllocator);
    }
}

std::unique_ptr<Column> ColumnFactory::createColumn(std::string name, PhysicalTypeID physicalType,
    FileHandle* dataFH, MemoryManager* mm, ShadowFile* shadowFile, bool enableCompression) {
    return std::make_unique<Column>(name, LogicalType::ANY(physicalType), dataFH, mm, shadowFile,
        enableCompression);
}

std::unique_ptr<Column> ColumnFactory::createColumn(std::string name, LogicalType dataType,
    FileHandle* dataFH, MemoryManager* mm, ShadowFile* shadowFile, bool enableCompression) {
    switch (dataType.getPhysicalType()) {
    case PhysicalTypeID::BOOL:
    case PhysicalTypeID::INT64:
    case PhysicalTypeID::INT32:
    case PhysicalTypeID::INT16:
    case PhysicalTypeID::INT8:
    case PhysicalTypeID::UINT64:
    case PhysicalTypeID::UINT32:
    case PhysicalTypeID::UINT16:
    case PhysicalTypeID::UINT8:
    case PhysicalTypeID::INT128:
    case PhysicalTypeID::DOUBLE:
    case PhysicalTypeID::FLOAT:
    case PhysicalTypeID::INTERVAL: {
        return std::make_unique<Column>(name, std::move(dataType), dataFH, mm, shadowFile,
            enableCompression);
    }
    case PhysicalTypeID::INTERNAL_ID: {
        return std::make_unique<InternalIDColumn>(name, dataFH, mm, shadowFile, enableCompression);
    }
    case PhysicalTypeID::STRING: {
        return std::make_unique<StringColumn>(name, std::move(dataType), dataFH, mm, shadowFile,
            enableCompression);
    }
    case PhysicalTypeID::ARRAY:
    case PhysicalTypeID::LIST: {
        return std::make_unique<ListColumn>(name, std::move(dataType), dataFH, mm, shadowFile,
            enableCompression);
    }
    case PhysicalTypeID::STRUCT: {
        return std::make_unique<StructColumn>(name, std::move(dataType), dataFH, mm, shadowFile,
            enableCompression);
    }
    default: {
        KU_UNREACHABLE;
    }
    }
}

} // namespace storage
} // namespace kuzu

#pragma once

#include "storage/table/column.h"

namespace kuzu {
namespace storage {
class MemoryManager;

class StructColumn final : public Column {
public:
    StructColumn(std::string name, common::LogicalType dataType, FileHandle* dataFH,
        MemoryManager* mm, ShadowFile* shadowFile, bool enableCompression);

    static std::unique_ptr<ColumnChunkData> flushChunkData(const ColumnChunkData& chunk,
        PageAllocator& pageAllocator);

    void scan(const ChunkState& state, ColumnChunkData* columnChunk,
        common::offset_t startOffset = 0,
        common::offset_t endOffset = common::INVALID_OFFSET) const override;
    void scan(const ChunkState& state, common::offset_t startOffsetInGroup,
        common::offset_t endOffsetInGroup, common::ValueVector* resultVector,
        uint64_t offsetInVector) const override;

    Column* getChild(common::idx_t childIdx) const {
        KU_ASSERT(childIdx < childColumns.size());
        return childColumns[childIdx].get();
    }
    void write(ColumnChunkData& persistentChunk, ChunkState& state, common::offset_t offsetInChunk,
        ColumnChunkData* data, common::offset_t dataOffset, common::length_t numValues) override;

    void checkpointColumnChunk(ColumnCheckpointState& checkpointState,
        PageAllocator& pageAllocator) override;

protected:
    void scanInternal(const ChunkState& state, common::offset_t startOffsetInChunk,
        common::row_idx_t numValuesToScan, common::ValueVector* resultVector) const override;

    void lookupInternal(const ChunkState& state, common::offset_t nodeOffset,
        common::ValueVector* resultVector, uint32_t posInVector) const override;

private:
    std::vector<std::unique_ptr<Column>> childColumns;
};

} // namespace storage
} // namespace kuzu

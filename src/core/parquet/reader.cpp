#include "zetta/parquet/reader.hpp"

#include <cstdint>
#include <utility>

#include "zetta/parquet/column_decoder.hpp"
#include "zetta/parquet/metadata.hpp"

namespace zetta::parquet {

ParquetRowsResult read_parquet_rows(std::string_view file_bytes) {
    ParquetRowsResult result;

    const ParquetMetadataResult metadata = parse_parquet_metadata(file_bytes);
    if (!metadata.ok()) {
        result.error = metadata.error;
        return result;
    }

    const std::int64_t num_rows = metadata.metadata.num_rows;
    if (num_rows < 0) {
        result.error = "parquet reader: negative row count in footer";
        return result;
    }
    result.rows.assign(static_cast<std::size_t>(num_rows), nlohmann::json::object());

    const std::vector<LeafColumn> leaves = metadata.metadata.leaf_columns();
    for (std::size_t index = 0; index < leaves.size(); ++index) {
        const DecodedColumnResult decoded =
            decode_column(file_bytes, metadata.metadata, index);
        if (!decoded.ok()) {
            result.rows.clear();
            result.error = leaves[index].path + ": " + decoded.error;
            return result;
        }
        const DecodedColumn& column = decoded.column;
        if (column.num_rows != num_rows) {
            result.rows.clear();
            result.error = column.path + ": column has " +
                           std::to_string(column.num_rows) + " rows, footer says " +
                           std::to_string(num_rows);
            return result;
        }
        // The transpose below indexes the dense values by a running counter, so
        // it needs one value per present row in the vector `type` selects.
        if (std::string shape = validate_decoded_column(column); !shape.empty()) {
            result.rows.clear();
            result.error = std::move(shape);
            return result;
        }

        // Walk logical rows, popping the next dense value for a present row and
        // emitting JSON null for a null row — the per-row transpose of
        // to_json(DecodedColumn).
        std::size_t next = 0;
        const ColumnValues& source = column.values;
        for (std::int64_t row = 0; row < num_rows; ++row) {
            nlohmann::json& cell = result.rows[static_cast<std::size_t>(row)][column.path];
            if (!column.row_is_valid(row)) {
                cell = nullptr;
                continue;
            }
            switch (column.type) {
                case PhysicalType::Boolean:
                    cell = source.bool_values[next] != 0;
                    break;
                case PhysicalType::Int32:
                    cell = source.int32_values[next];
                    break;
                case PhysicalType::Int64:
                    cell = source.int64_values[next];
                    break;
                case PhysicalType::Float:
                    cell = source.float_values[next];
                    break;
                case PhysicalType::Double:
                    cell = source.double_values[next];
                    break;
                case PhysicalType::ByteArray:
                case PhysicalType::FixedLenByteArray:
                    cell = source.byte_array_values[next];
                    break;
                default:
                    result.rows.clear();
                    result.error = column.path + ": unsupported physical type";
                    return result;
            }
            ++next;
        }
    }

    return result;
}

}  // namespace zetta::parquet

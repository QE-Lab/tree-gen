#include "tree-cbor.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstring>

namespace tree {
namespace cbor {

/**
 * Turns the given std::string that consists of an RFC7049 CBOR object into
 * a Reader representation that may be used to parse it.
 */
Reader::Reader(const std::string &data) : Reader(std::string(data)) {}

/**
 * Turns the given std::string that consists of an RFC7049 CBOR object into
 * a Reader representation that may be used to parse it.
 */
Reader::Reader(std::string &&data) :
    data(std::make_shared<std::string>(std::forward<std::string>(data))),
    slice_offset(0),
    slice_length(this->data->size())
{
    if (!slice_length) {
        throw std::runtime_error("invalid CBOR: zero-size object");
    }
    check();
}

/**
 * Loads the given CBOR file and turns it into a CborReader for parsing.
 */
Reader Reader::from_file(const std::string &&filename) {
    std::ostringstream ss;
    ss << std::ifstream(filename).rdbuf();
    return Reader(ss.str());
}

/**
 * Constructs a subslice of this slice.
 */
Reader::Reader(const Reader &parent, size_t offs, size_t len) :
    data(parent.data),
    slice_offset(parent.slice_offset + offs),
    slice_length(len)
{
    if (slice_offset + slice_length > parent.slice_offset + parent.slice_length) {
        throw std::runtime_error("invalid CBOR: trying to slice past extents of current slice");
    }
    if (this->slice_length == 0) {
        throw std::runtime_error("invalid CBOR: trying to make an empty slice");
    }

    // Seek past semantic tags.
    uint8_t initial = read_at(0);
    uint8_t type = initial >> 5u;
    if (type == 6) {
        size_t end = slice_offset + slice_length;
        slice_offset++;
        read_intlike(initial & 0x1Fu, slice_offset);
        slice_length = end - slice_offset;
        if (this->slice_length == 0) {
            throw std::runtime_error("invalid CBOR: semantic tag has no value");
        }
    }
}

/**
 * Returns a subslice of this slice.
 */
Reader Reader::slice(size_t offset, size_t length) const {
    return Reader(*this, offset, length);
}

/**
 * Returns the byte at the given offset after range-checking.
 */
uint8_t Reader::read_at(size_t offset) const {
    if (offset >= slice_length) {
        throw std::runtime_error("invalid CBOR: trying to read past extents of current slice");
    }
    return data->at(this->slice_offset + offset);
}

/**
 * Parses the additional information and reads any additional bytes it specifies
 * the existence of, and returns the encoded integer. offset should point to the
 * byte immediately following the initial byte. offset is moved forward past the
 * integer data.
 */
uint64_t Reader::read_intlike(uint8_t info, size_t &offset) const {

    // Info less than 24 is a shorthand for the integer itself.
    if (info < 24u) return info;

    // Info greater than or equal to 24 is either illegal or a big-endian
    // integer immediately following the initial byte. So read said integer.
    uint64_t value = read_at(offset++);
    if (info < 25u) return value;
    value <<= 8u; value |= read_at(offset++);
    if (info < 26u) return value;
    value <<= 8u; value |= read_at(offset++);
    value <<= 8u; value |= read_at(offset++);
    if (info < 27u) return value;
    value <<= 8u; value |= read_at(offset++);
    value <<= 8u; value |= read_at(offset++);
    value <<= 8u; value |= read_at(offset++);
    value <<= 8u; value |= read_at(offset++);
    if (info < 28u) return value;

    // Info greater than or equal to 28 is illegal. Note that 31 is used for
    // indefinite lengths, so this must be checked prior to calling this method.
    throw std::runtime_error("invalid CBOR: illegal additional info for integer or object length");

}

/**
 * Reads the string representation of this slice for both binary and UTF8
 * strings alike. Assumes that the slice is actually a string. offset must start
 * at 0, and is moved to the end of the string. The string is written to the
 * output stream.
 */
void Reader::read_stringlike(size_t &offset, std::ostream &s) const {
    uint8_t info = read_at(offset++) & 0x1Fu;
    if (info == 31) {

        // Handle indefinite length strings. These are just a break-terminated
        // list of definite-length strings, so we can just call ourselves for
        // the child objects.
        while (read_at(offset) != 0xFF) {
            read_stringlike(offset, s);
        }
        offset++;

    } else {

        // Handle definite-length strings.
        uint64_t length = read_intlike(info, offset);
        if (length + offset > this->slice_length) {
            throw std::runtime_error("Invalid CBOR: string read past end of slice");
        }
        s.write(data->data() + this->slice_offset + offset, length);
        offset += length;

    }
}

/**
 * Checks validity of the object at the given offset and seeks past it by
 * moving offset to the byte immediately following the object.
 */
void Reader::check_and_seek(size_t &offset) const {

    // Read the initial byte.
    uint8_t initial = read_at(offset++);
    uint8_t type = initial >> 5u;
    uint8_t info = initial & 0x1Fu;

    // Handle major types 0 through 6.
    switch (type) {
        case 0: // unsigned integer
        case 1: // negative integer
            read_intlike(info, offset);
            return;

        case 2: // byte string
        case 3: // UTF8 string

            // Handle indefinite length strings.
            if (info == 31) {

                // Indefinite strings consist of a break-terminated (0xFF) list
                // of definite-length strings of the same type.
                uint8_t sub_initial;
                while ((sub_initial = read_at(offset++)) != 0xFF) {
                    uint8_t sub_major_type = sub_initial >> 5u;
                    uint8_t sub_info = sub_initial & 0x1Fu;
                    if (sub_major_type != type) {
                        throw std::runtime_error("invalid CBOR: illegal indefinite-length string component");
                    }
                    // Seek past definite-length string component. The size in
                    // bytes is encoded as an integer.
                    offset += read_intlike(sub_info, offset);
                }

                return;
            }

            // Seek past definite-length string. The size in bytes is
            // encoded as an integer.
            offset += read_intlike(info, offset);
            return;

        case 4: // array
        case 5: // map

            // Handle indefinite length arrays and maps.
            if (info == 31) {

                // Read objects/object pairs until we encounter a break.
                while (read_at(offset) != 0xFF) {
                    if (type == 5) check_and_seek(offset);
                    check_and_seek(offset);
                }

                // Seek past the break.
                offset++;

                return;
            }

            // Handle definite-length arrays and maps. The amount of
            // objects/object pairs is encoded as an integer.
            for (uint64_t size = read_intlike(info, offset); size--;) {
                if (type == 5) check_and_seek(offset);
                check_and_seek(offset);
            }
            return;

        case 6: // semantic tag

            // We don't use semantic tags for anything, but ignoring them is
            // legal and reading past them is easy enough.
            read_intlike(info, offset);
            check_and_seek(offset);
            return;

        default:
            break;
    }

    // Handle major type 7. Here, the type is defined by the additional info.
    // Additional info 24 is reserved for having the type specified by the next
    // byte, but all such values are unassigned.
    switch (info) {
        case 20: // false
        case 21: // true
        case 22: // null
            // Simple value with no additional data, we're already done.
            return;

        case 23: // undefined
            throw std::runtime_error("invalid CBOR: undefined value is not supported");

        case 25: // half-precision float
            throw std::runtime_error("invalid CBOR: half-precision float is not supported");

        case 26: // single-precision float
            throw std::runtime_error("invalid CBOR: single-precision float is not supported");

        case 27: // double-precision float
            offset += 8;
            return;

        case 31: // break
            throw std::runtime_error("invalid CBOR: unexpected break");

        default:
            break;
    }

    throw std::runtime_error("invalid CBOR: unknown type code");
}

/**
 * Tests whether the structure is valid CBOR for as far as we know about
 * it. Throws a std::runtime_error with an appropriate message if not.
 */
void Reader::check() const {
    size_t offset = 0u;
    check_and_seek(offset);
    if (offset != this->slice_length) {
        throw std::runtime_error("invalid CBOR: garbage at end of outer object or multiple objects");
    }
}

/**
 * Returns the name of the type corresponding to this CBOR object slice. This
 * returns one of:
 *  - "null"
 *  - "boolean"
 *  - "integer"
 *  - "float"
 *  - "binary string"
 *  - "UTF8 string"
 *  - "array"
 *  - "map"
 *  - "unknown type"
 */
const char *Reader::get_type_name() const {
    uint8_t initial = read_at(0);
    uint8_t type = initial >> 5u;
    uint8_t info = initial & 0x1Fu;
    switch (type) {
        case 0: case 1: return "integer";
        case 2: return "binary string";
        case 3: return "UTF8 string";
        case 4: return "array";
        case 5: return "map";
        case 7:
            switch (info) {
                case 20: case 21: return "boolean";
                case 22: return "null";
                case 27: return "float";
                default: break;
            }
        default: break;
    }
    return "unknown type";
}

/**
 * Checks whether the object represented by this slice is null.
 */
bool Reader::is_null() const {
    return read_at(0) == 0xF6;
}

/**
 * Throws an unexpected value type error through a std::runtime_error if
 * the object represented by this slice is not null.
 */
void Reader::as_null() const {
    if (!is_null()) {
        throw std::runtime_error(
            "unexpected CBOR structure: expected null but found "
            + std::string(get_type_name()));
    }
}

/**
 * Checks whether the object represented by this slice is a boolean.
 */
bool Reader::is_bool() const {
    return (read_at(0) & 0xFEu) == 0xF4;
}

/**
 * Returns the boolean representation of this slice. If it's not a boolean,
 * an unexpected value type error is thrown through a std::runtime_error.
 */
bool Reader::as_bool() const {
    switch (read_at(0)) {
        case 0xF4: return false;
        case 0xF5: return true;
    }
    throw std::runtime_error(
        "unexpected CBOR structure: expected boolean but found "
        + std::string(get_type_name()));
}

/**
 * Checks whether the object represented by this slice is an integer.
 */
bool Reader::is_int() const {
    return (read_at(0) & 0xC0u) == 0;
}

/**
 * Returns the integer representation of this slice. If it's not an
 * integer, an unexpected value type error is thrown through a
 * std::runtime_error.
 */
int64_t Reader::as_int() const {
    uint8_t initial = read_at(0);
    uint8_t type = initial >> 5u;
    if (type >= 2) {
        throw std::runtime_error(
            "unexpected CBOR structure: expected integer but found "
            + std::string(get_type_name()));
    }
    uint8_t info = initial & 0x1Fu;
    uint64_t offset = 1;
    uint64_t value = read_intlike(info, offset);
    if (value >= 0x8000000000000000ull) {
        throw std::runtime_error("CBOR integer out of int64 range");
    }
    if (type == 0) {
        return (int64_t)value;
    } else {
        return -1 - (int64_t)value;
    }
}

/**
 * Checks whether the object represented by this slice is a float. Only
 * double precision is supported.
 */
bool Reader::is_float() const {
    return read_at(0) == 0xFBu;
}

/**
 * Returns the floating point representation of this slice. If it's not a
 * float, an unexpected value type error is thrown through a
 * std::runtime_error. Only double precision is supported.
 */
double Reader::as_float() const {
    if (!is_float()) {
        throw std::runtime_error(
            "unexpected CBOR structure: expected float but found "
            + std::string(get_type_name()));
    }
    size_t offset = 1;
    uint64_t value = read_intlike(27, offset);
    return *(double*)(&value);
}

/**
 * Checks whether the object represented by this slice is a Unicode string.
 */
bool Reader::is_string() const {
    return (read_at(0) & 0xE0u) == 0x60u;
}

/**
 * Returns the string representation of this slice. If it's not a Unicode
 * string, an unexpected value type error is thrown through a
 * std::runtime_error.
 */
std::string Reader::as_string() const {
    if (!is_string()) {
        throw std::runtime_error(
            "unexpected CBOR structure: expected UTF8 string but found "
            + std::string(get_type_name()));
    }
    std::ostringstream ss;
    uint64_t offset = 0;
    read_stringlike(offset, ss);
    return ss.str();
}

/**
 * Checks whether the object represented by this slice is a binary string.
 */
bool Reader::is_binary() const {
    return (read_at(0) & 0xE0u) == 0x40u;
}

/**
 * Returns the string representation of this slice. If it's not a binary
 * string, an unexpected value type error is thrown through a
 * std::runtime_error.
 */
std::string Reader::as_binary() const {
    if (!is_binary()) {
        throw std::runtime_error(
            "unexpected CBOR structure: expected binary string but found "
            + std::string(get_type_name()));
    }
    std::ostringstream ss;
    uint64_t offset = 0;
    read_stringlike(offset, ss);
    return ss.str();
}

/**
 * Checks whether the object represented by this slice is an array.
 */
bool Reader::is_array() const {
    return (read_at(0) & 0xE0u) == 0x80u;
}

/**
 * Reads the array item at the given offset into the array and advances the
 * offset past the item data.
 */
void Reader::read_array_item(size_t &offset, ArrayReader &ar) const {
    size_t start = offset;
    check_and_seek(offset);
    ar.push_back(slice(start, offset - start));
}

/**
 * Returns the array representation of this slice. If it's not an array,
 * an unexpected value type error is thrown through a std::runtime_error.
 */
ArrayReader Reader::as_array() const {
    if (!is_array()) {
        throw std::runtime_error(
            "unexpected CBOR structure: expected array but found "
            + std::string(get_type_name()));
    }

    uint8_t info = read_at(0) & 0x1Fu;
    size_t offset = 1;
    ArrayReader ar;

    if (info == 31) {

        // Handle indefinite length arrays.
        while (read_at(offset) != 0xFF) {
            read_array_item(offset, ar);
        }

    } else {

        // Handle definite-length arrays and maps. The amount of
        // objects/object pairs is encoded as an integer.
        for (uint64_t size = read_intlike(info, offset); size--;) {
            read_array_item(offset, ar);
        }

    }

    return ar;
}

/**
 * Checks whether the object represented by this slice is a map/object.
 */
bool Reader::is_map() const {
    return (read_at(0) & 0xE0u) == 0xA0u;
}

/**
 * Reads the map item at the given offset into the array and advances the
 * offset past the item data.
 */
void Reader::read_map_item(size_t &offset, MapReader &map) const {
    size_t key_start = offset;
    check_and_seek(offset);
    size_t data_start = offset;
    check_and_seek(offset);
    map.insert(std::make_pair(
        slice(key_start, data_start - key_start).as_string(),
        slice(data_start, offset - data_start)));
}

/**
 * Returns the map/object representation of this slice. If it's not a map,
 * an unexpected value type error is thrown through a std::runtime_error.
 */
MapReader Reader::as_map() const {
    if (!is_map()) {
        throw std::runtime_error(
            "unexpected CBOR structure: expected map but found "
            + std::string(get_type_name()));
    }

    uint8_t info = read_at(0) & 0x1Fu;
    size_t offset = 1;
    MapReader map;

    if (info == 31) {

        // Handle indefinite length arrays.
        while (read_at(offset) != 0xFFu) {
            read_map_item(offset, map);
        }

    } else {

        // Handle definite-length arrays and maps. The amount of
        // objects/object pairs is encoded as an integer.
        for (uint64_t size = read_intlike(info, offset); size--;) {
            read_map_item(offset, map);
        }

    }

    return map;
}

/**
 * Returns a copy of the CBOR slice in the form of a binary string.
 */
std::string Reader::get_contents() const {
    return data->substr(slice_offset, slice_length);
}

/**
 * Constructs a structure writer and makes it the active writer.
 */
StructureWriter::StructureWriter(Writer &writer) :
    writer(writer),
    id(writer.id_counter)
{
    writer.stack.push(id);
    writer.id_counter++;
}

/**
 * Returns a reference to the underlying output stream if and only if we're
 * the active writer. Otherwise an exception is thrown.
 */
std::ostream &StructureWriter::stream() {
    if (writer.stack.empty() || writer.stack.top() != id) {
        throw std::runtime_error("Attempt to write to CBOR object using inactive writer");
    }
    return writer.stream;
}

/**
 * Writes a null value to the structure.
 */
void StructureWriter::write_null() {
    uint8_t data = 0xF6;
    stream().write(reinterpret_cast<char*>(&data), 1);
}

/**
 * Writes a boolean value to the structure.
 */
void StructureWriter::write_bool(bool value) {
    uint8_t data = value ? 0xF5 : 0xF4;
    stream().write(reinterpret_cast<char*>(&data), 1);
}

/**
 * Writes an integer value to the structure. The major code can be
 * overridden from 0/1 to something else to specify a length, but in that
 * case value should be positive.
 */
void StructureWriter::write_int(int64_t int_value, uint8_t major) {
    uint64_t value;
    if (int_value < 0) {
        major = 1;
        value = -1 - int_value;
    } else {
        value = int_value;
    }
    uint8_t data[9];
    data[0] = major << 5u;
    if (value < 24) {
        data[0] |= value;
        stream().write(reinterpret_cast<char*>(&data), 1);
    } else if (value < 0x100ll) {
        data[0] |= 24u;
        data[1] = value;
        stream().write(reinterpret_cast<char*>(&data), 2);
    } else if (value < 0x10000ll) {
        data[0] |= 25u;
        data[1] = value >> 8u;
        data[2] = value;
        stream().write(reinterpret_cast<char*>(&data), 3);
    } else if (value < 0x100000000ll) {
        data[0] |= 26u;
        data[1] = value >> 24u;
        data[2] = value >> 16u;
        data[3] = value >> 8u;
        data[4] = value;
        stream().write(reinterpret_cast<char*>(&data), 5);
    } else {
        data[0] |= 27u;
        data[1] = value >> 56u;
        data[2] = value >> 48u;
        data[3] = value >> 40u;
        data[4] = value >> 32u;
        data[5] = value >> 24u;
        data[6] = value >> 16u;
        data[7] = value >> 8u;
        data[8] = value;
        stream().write(reinterpret_cast<char*>(&data), 9);
    }
}

/**
 * Writes a float value to the structure. Only doubles are supported.
 */
void StructureWriter::write_float(double value) {
    uint8_t data[9];
    data[0] = 0xFB;
    std::memcpy(data + 1, &value, 8);
    std::swap(data[1], data[8]);
    std::swap(data[2], data[7]);
    std::swap(data[3], data[6]);
    std::swap(data[4], data[5]);
    stream().write(reinterpret_cast<char*>(&data), 9);
}

/**
 * Writes a Unicode string value to the structure.
 */
void StructureWriter::write_string(const std::string &value) {
    write_int(value.size(), 3);
    stream().write(value.data(), value.size());
}

/**
 * Writes a binary string value to the structure.
 */
void StructureWriter::write_binary(const std::string &value) {
    write_int(value.size(), 2);
    stream().write(value.data(), value.size());
}

/**
 * Starts writing an array to the structure. The array is constructed in a
 * streaming fashion using the return value. It must be close()d or go out
 * of scope before the next value can be written to this structure.
 */
ArrayWriter StructureWriter::write_array() {
    // Ensure that we're allowed to write.
    stream();
    return ArrayWriter(writer);
}

/**
 * Starts writing a map to the structure. The map is constructed in a
 * streaming fashion using the return value. It must be close()d or go out
 * of scope before the next value can be written to this structure.
 */
MapWriter StructureWriter::write_map() {
    // Ensure that we're allowed to write.
    stream();
    return MapWriter(writer);
}

/**
 * Virtual destructor. This calls close() if we're the active writer, but
 * assumes close() was called manually if not.
 */
StructureWriter::~StructureWriter() {
    if (!writer.stack.empty() && writer.stack.top() == id) {
        close();
    }
}

/**
 * Terminates the structure that we were writing with a break code, and
 * hands over control to the parent writer (if any).
 */
void StructureWriter::close() {
    uint8_t data = 0xFF;
    stream().write(reinterpret_cast<char*>(&data), 1);
    writer.stack.pop();
}

/**
 * Constructs a new array writer, makes it the active writer, and writes the
 * array header.
 */
ArrayWriter::ArrayWriter(Writer &writer) : StructureWriter(writer) {
    uint8_t data = 0x9F;
    stream().write(reinterpret_cast<char*>(&data), 1);
}

/**
 * Writes a null value to the array.
 */
void ArrayWriter::append_null() {
    write_null();
}

/**
 * Writes a boolean value to the array.
 */
void ArrayWriter::append_bool(bool value) {
    write_bool(value);
}

/**
 * Writes an integer value to the array.
 */
void ArrayWriter::append_int(int64_t value) {
    write_int(value);
}

/**
 * Writes a float value to the array. Only doubles are supported.
 */
void ArrayWriter::append_float(double value) {
    write_float(value);
}

/**
 * Writes a Unicode string value to the array.
 */
void ArrayWriter::append_string(const std::string &value) {
    write_string(value);
}

/**
 * Writes a binary string value to the array.
 */
void ArrayWriter::append_binary(const std::string &value) {
    write_binary(value);
}

/**
 * Starts writing a nested array to the array. The array is constructed in a
 * streaming fashion using the return value. It must be close()d or go out
 * of scope before the next value can be written to this array.
 */
ArrayWriter ArrayWriter::append_array() {
    return write_array();
}

/**
 * Starts writing a map to the array. The map is constructed in a streaming
 * fashion using the return value. It must be close()d or go out of scope
 * before the next value can be written to this array.
 */
MapWriter ArrayWriter::append_map() {
    return write_map();
}

/**
 * Constructs a new map writer, makes it the active writer, and writes the
 * map header.
 */
MapWriter::MapWriter(Writer &writer) : StructureWriter(writer) {
    uint8_t data = 0xBF;
    stream().write(reinterpret_cast<char*>(&data), 1);
}

/**
 * Writes a null value to the map with the given key.
 */
void MapWriter::append_null(const std::string &key) {
    write_string(key);
    write_null();
}

/**
 * Writes a boolean value to the map with the given key.
 */
void MapWriter::append_bool(const std::string &key, bool value) {
    write_string(key);
    write_bool(value);
}

/**
 * Writes an integer value to the map with the given key.
 */
void MapWriter::append_int(const std::string &key, int64_t value) {
    write_string(key);
    write_int(value);
}

/**
 * Writes a float value to the map with the given key. Only doubles are
 * supported.
 */
void MapWriter::append_float(const std::string &key, double value) {
    write_string(key);
    write_float(value);
}

/**
 * Writes a Unicode string value to the map with the given key.
 */
void MapWriter::append_string(const std::string &key, const std::string &value) {
    write_string(key);
    write_string(value);
}

/**
 * Writes a binary string value to the map with the given key.
 */
void MapWriter::append_binary(const std::string &key, const std::string &value) {
    write_string(key);
    write_binary(value);
}

/**
 * Starts writing an array to the map with the given key. The array is
 * constructed in a streaming fashion using the return value. It must be
 * close()d or go out of scope before the next value can be written to this
 * map.
 */
ArrayWriter MapWriter::append_array(const std::string &key) {
    write_string(key);
    return write_array();
}

/**
 * Starts writing a nested map to the map with the given key. The map is
 * constructed in a streaming fashion using the return value. It must be
 * close()d or go out of scope before the next value can be written to this
 * map.
 */
MapWriter MapWriter::append_map(const std::string &key) {
    write_string(key);
    return write_map();
}

/**
 * Creates a CBOR writer that writes to the given stream.
 */
Writer::Writer(std::ostream &stream) : stream(stream), id_counter(0) {
}

/**
 * Returns the toplevel map writer. This can only be done when no other
 * writer is active. It is technically legal to call this multiple times to
 * write multiple structures back-to-back, but this is not used for
 * serializing trees.
 */
MapWriter Writer::start() {
    if (!stack.empty()) {
        throw std::runtime_error("Writing of this CBOR object has already started");
    }
    return MapWriter(*this);
}

}
}

#include <sstream>
#include <cstdio>
#include "tree-cbor.hpp"
#include "assert.hpp"

// Some valid CBOR to test decoding with:
const uint8_t TEST_CBOR[] = {
    0x89,                                                           // array(9)
        0xF6,                                                       // primitive(22)
        0xF4,                                                       // primitive(20)
        0xF5,                                                       // primitive(21)
        0x8B,                                                       // array(11)
            0x00,                                                   // unsigned(0)
            0x01,                                                   // unsigned(1)
            0x17,                                                   // unsigned(23)
            0x18, 0x18,                                             // unsigned(24)
            0x18, 0xFF,                                             // unsigned(255)
            0x19, 0x01, 0x00,                                       // unsigned(256)
            0x19, 0xFF, 0xFF,                                       // unsigned(65535)
            0x1A, 0x00, 0x01, 0x00, 0x00,                           // unsigned(65536)
            0x1A, 0xFF, 0xFF, 0xFF, 0xFF,                           // unsigned(4294967295)
            0x1B, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,   // unsigned(4294967296)
            0x1B, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // unsigned(9223372036854775807)
        0x9F,                                                       // array(*)
            0x20,                                                   // negative(0)
            0x37,                                                   // negative(23)
            0x38, 0x18,                                             // negative(24)
            0x38, 0xFF,                                             // negative(255)
            0x39, 0x01, 0x00,                                       // negative(256)
            0x39, 0xFF, 0xFF,                                       // negative(65535)
            0x3A, 0x00, 0x01, 0x00, 0x00,                           // negative(65536)
            0x3A, 0xFF, 0xFF, 0xFF, 0xFF,                           // negative(4294967295)
            0x3B, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,   // negative(4294967296)
            0x3B, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // negative(9223372036854775807)
            0xFF,                                                   // primitive(*)
        0xFB, 0x40, 0x09, 0x21, 0xFB, 0x54, 0x44, 0x2E, 0xEA,       // primitive(4614256656552046314)
        0x65,                                                       // text(5)
            0x68, 0x65, 0x6C, 0x6C, 0x6F,                           // "hello"
        0x45,                                                       // bytes(5)
            0x77, 0x6F, 0x72, 0x6C, 0x64,                           // "world"
        0xA2,                                                       // map(2)
            0x61,                                                   // text(1)
                0x61,                                               // "a"
            0x61,                                                   // text(1)
                0x62,                                               // "b"
            0x61,                                                   // text(1)
                0x63,                                               // "c"
            0x61,                                                   // text(1)
                0x64                                                // "d"
};

int main() {

    // Basic test for the reader using known-good CBOR.
    auto reader = tree::cbor::Reader(std::string((const char*)TEST_CBOR, sizeof(TEST_CBOR)));
    CHECK(reader.is_array());
    auto ar = reader.as_array();
    CHECK_EQ(ar.size(), 9);
    CHECK(ar[0].is_null());
    ar[0].as_null();
    CHECK(ar[1].is_bool());
    CHECK(!ar[1].as_bool());
    CHECK(ar[2].is_bool());
    CHECK(ar[2].as_bool());
    CHECK(ar[3].is_array());
    auto ar2 = ar[3].as_array();
    CHECK_EQ(ar2.size(), 11);
    CHECK(ar2[0].is_int());
    CHECK_EQ(ar2[0].as_int(), 0);
    CHECK_EQ(ar2[1].as_int(), 1);
    CHECK_EQ(ar2[2].as_int(), 23);
    CHECK_EQ(ar2[3].as_int(), 24);
    CHECK_EQ(ar2[4].as_int(), 255);
    CHECK_EQ(ar2[5].as_int(), 256);
    CHECK_EQ(ar2[6].as_int(), 65535);
    CHECK_EQ(ar2[7].as_int(), 65536);
    CHECK_EQ(ar2[8].as_int(), 4294967295);
    CHECK_EQ(ar2[9].as_int(), 4294967296);
    CHECK_EQ(ar2[10].as_int(), 9223372036854775807);
    auto ar3 = ar[4].as_array();
    CHECK_EQ(ar3.size(), 10);
    CHECK(ar3[0].is_int());
    CHECK_EQ(ar3[0].as_int(), -1);
    CHECK_EQ(ar3[1].as_int(), -24);
    CHECK_EQ(ar3[2].as_int(), -25);
    CHECK_EQ(ar3[3].as_int(), -256);
    CHECK_EQ(ar3[4].as_int(), -257);
    CHECK_EQ(ar3[5].as_int(), -65536);
    CHECK_EQ(ar3[6].as_int(), -65537);
    CHECK_EQ(ar3[7].as_int(), -4294967296);
    CHECK_EQ(ar3[8].as_int(), -4294967297);
    CHECK_EQ(ar3[9].as_int(), -9223372036854775807 - 1);
    CHECK(ar[5].is_float());
    CHECK_EQ(ar[5].as_float(), 3.14159265359);
    CHECK(ar[6].is_string());
    CHECK_EQ(ar[6].as_string(), "hello");
    CHECK(ar[7].is_binary());
    CHECK_EQ(ar[7].as_binary(), "world");
    CHECK(ar[8].is_map());
    auto map = ar[8].as_map();
    CHECK_EQ(map.size(), 2);
    CHECK_EQ(map.find("a")->second.as_string(), "b");
    CHECK_EQ(map.find("c")->second.as_string(), "d");

    // Basic test for the writer.
    std::ostringstream ss;
    auto writer = tree::cbor::Writer(ss);
    auto outer = writer.start();
    outer.append_null("null");
    outer.append_bool("false", false);
    outer.append_bool("true", true);
    auto int_array = outer.append_array("int-array");
    int_array.append_int(0x3);
    int_array.append_int(0x34);
    int_array.append_int(0x3456);
    int_array.append_int(0x3456789A);
    int_array.append_int(0x3456789ABCDEF012);
    int_array.append_int(-0x3);
    int_array.append_int(-0x34);
    int_array.append_int(-0x3456);
    int_array.append_int(-0x3456789A);
    int_array.append_int(-0x3456789ABCDEF012);
    int_array.close();
    outer.append_float("pi", 3.14159265359);
    outer.append_string("string", "hello");
    outer.append_binary("binary", "world");
    outer.close();
    std::string encoded = ss.str();

    /*for (auto c : encoded) {
        std::printf("%02X ", (uint8_t)c);
    }
    std::printf("\n");*/

    // Test the writer using our own reader.
    auto reader2 = tree::cbor::Reader(encoded);
    auto map2 = reader2.as_map();
    CHECK_EQ(map2.size(), 7);
    map2.find("null")->second.as_null();
    CHECK_EQ(map2.find("false")->second.as_bool(), false);
    CHECK_EQ(map2.find("true")->second.as_bool(), true);
    auto ar4 = map2.find("int-array")->second.as_array();
    CHECK_EQ(ar4.size(), 10);
    CHECK_EQ(ar4[0].as_int(), 0x3);
    CHECK_EQ(ar4[1].as_int(), 0x34);
    CHECK_EQ(ar4[2].as_int(), 0x3456);
    CHECK_EQ(ar4[3].as_int(), 0x3456789A);
    CHECK_EQ(ar4[4].as_int(), 0x3456789ABCDEF012);
    CHECK_EQ(ar4[5].as_int(), -0x3);
    CHECK_EQ(ar4[6].as_int(), -0x34);
    CHECK_EQ(ar4[7].as_int(), -0x3456);
    CHECK_EQ(ar4[8].as_int(), -0x3456789A);
    CHECK_EQ(ar4[9].as_int(), -0x3456789ABCDEF012);
    CHECK_EQ(map2.find("pi")->second.as_float(), 3.14159265359);
    CHECK_EQ(map2.find("string")->second.as_string(), "hello");
    CHECK_EQ(map2.find("binary")->second.as_binary(), "world");

    std::cout << "Test passed" << std::endl;
    return 0;
}

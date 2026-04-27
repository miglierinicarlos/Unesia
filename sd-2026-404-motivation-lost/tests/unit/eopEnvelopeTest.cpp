extern "C"
{
#include "eop_client.h"
#include "eop_envelope.h"
}

#include <arpa/inet.h>
#include <cstring>
#include <gtest/gtest.h>

// =============================================================================
// EnvelopeTest.SerializeRoundTrip
// =============================================================================
TEST(EnvelopeTest, SerializeRoundTripProducesIdenticalFields)
{
    // Arrange
    const uint8_t payload[] = "{\"node_id\":\"vault-13\"}";
    size_t payload_len = sizeof(payload) - 1;
    uint8_t buf[256];

    // Act — serialize
    int rc = eop_envelope_serialize(EOP_REGISTER, 42, payload, payload_len, buf, sizeof(buf));
    ASSERT_EQ(rc, EOP_OK);

    // Act — deserialize
    eop_message_type_t out_type;
    uint32_t out_id;
    const uint8_t* out_payload;
    size_t out_payload_len;
    rc = eop_envelope_deserialize(
        buf, EOP_ENVELOPE_HEADER_SIZE + payload_len, &out_type, &out_id, &out_payload, &out_payload_len);
    ASSERT_EQ(rc, EOP_OK);

    // Assert
    EXPECT_EQ(out_type, EOP_REGISTER);
    EXPECT_EQ(out_id, 42u);
    EXPECT_EQ(out_payload_len, payload_len);
    EXPECT_EQ(std::memcmp(out_payload, payload, payload_len), 0);
}

// =============================================================================
// EnvelopeTest.ByteOrderCorrect
// =============================================================================
TEST(EnvelopeTest, ByteOrderCorrectOnWire)
{
    // Arrange
    const uint8_t payload[] = "test";
    size_t payload_len = 4;
    uint8_t buf[64];
    uint32_t msg_id = 0x01020304;

    // Act
    int rc = eop_envelope_serialize(EOP_ACK, msg_id, payload, payload_len, buf, sizeof(buf));
    ASSERT_EQ(rc, EOP_OK);

    // Assert — verify raw bytes are big-endian
    EXPECT_EQ(buf[0], EOP_PROTOCOL_VERSION);
    EXPECT_EQ(buf[1], 0x02);

    // message_id at offset 2: 0x01020304 in big-endian
    EXPECT_EQ(buf[2], 0x01);
    EXPECT_EQ(buf[3], 0x02);
    EXPECT_EQ(buf[4], 0x03);
    EXPECT_EQ(buf[5], 0x04);

    // payload_length at offset 6: 4 in big-endian
    EXPECT_EQ(buf[6], 0x00);
    EXPECT_EQ(buf[7], 0x00);
    EXPECT_EQ(buf[8], 0x00);
    EXPECT_EQ(buf[9], 0x04);
}

// =============================================================================
// EnvelopeTest.TruncatedHeader
// =============================================================================
TEST(EnvelopeTest, TruncatedHeaderReturnsError)
{
    // Arrange — buffer shorter than header
    uint8_t buf[5] = {EOP_PROTOCOL_VERSION, EOP_REGISTER, 0, 0, 0};
    eop_message_type_t out_type;
    uint32_t out_id;
    const uint8_t* out_payload;
    size_t out_payload_len;

    // Act
    int rc = eop_envelope_deserialize(buf, sizeof(buf), &out_type, &out_id, &out_payload, &out_payload_len);

    // Assert
    EXPECT_EQ(rc, EOP_ERR_INVALID_ARGUMENT);
}

// =============================================================================
// EnvelopeTest.PayloadLengthMismatch
// =============================================================================
TEST(EnvelopeTest, PayloadLengthMismatchReturnsError)
{
    // Arrange — serialize with 10-byte payload, then lie about buffer size
    const uint8_t payload[] = "0123456789";
    size_t payload_len = 10;
    uint8_t buf[64];
    int rc = eop_envelope_serialize(EOP_HEARTBEAT, 1, payload, payload_len, buf, sizeof(buf));
    ASSERT_EQ(rc, EOP_OK);

    // Act — pass truncated length (header only, no room for payload)
    eop_message_type_t out_type;
    uint32_t out_id;
    const uint8_t* out_payload;
    size_t out_payload_len;
    rc = eop_envelope_deserialize(buf, EOP_ENVELOPE_HEADER_SIZE, &out_type, &out_id, &out_payload, &out_payload_len);

    // Assert
    EXPECT_EQ(rc, EOP_ERR_INVALID_ARGUMENT);
}

// =============================================================================
// EnvelopeTest.AllMessageTypes
// =============================================================================
TEST(EnvelopeTest, AllMessageTypesSerializeCorrectly)
{
    eop_message_type_t types[] = {
        EOP_REGISTER,
        EOP_ACK,
        EOP_ERROR,
        EOP_QUERY_NODE,
        EOP_LIST_NODES,
        EOP_HEARTBEAT,
        EOP_ANALYZE_GRAPH,
        EOP_ANALYZE_RESULT,
        EOP_EDGE_TELEMETRY,
    };

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
    {
        // Arrange
        uint8_t buf[64];

        // Act
        int rc = eop_envelope_serialize(types[i], 1, nullptr, 0, buf, sizeof(buf));
        ASSERT_EQ(rc, EOP_OK) << "Failed to serialize message type 0x" << std::hex << static_cast<int>(types[i]);

        // Assert — message_type byte matches
        EXPECT_EQ(buf[1], static_cast<uint8_t>(types[i]));
    }
}

// =============================================================================
// EnvelopeTest — additional edge cases
// =============================================================================
TEST(EnvelopeTest, SerializeWithNullPayloadAndZeroLengthSucceeds)
{
    // Arrange
    uint8_t buf[16];

    // Act
    int rc = eop_envelope_serialize(EOP_HEARTBEAT, 1, nullptr, 0, buf, sizeof(buf));

    // Assert
    EXPECT_EQ(rc, EOP_OK);
}

TEST(EnvelopeTest, SerializeWithNullPayloadAndNonZeroLengthFails)
{
    // Arrange
    uint8_t buf[64];

    // Act
    int rc = eop_envelope_serialize(EOP_REGISTER, 1, nullptr, 10, buf, sizeof(buf));

    // Assert
    EXPECT_EQ(rc, EOP_ERR_INVALID_ARGUMENT);
}

TEST(EnvelopeTest, SerializeWithBufferTooSmallFails)
{
    // Arrange
    const uint8_t payload[] = "hello";
    uint8_t buf[4];

    // Act
    int rc = eop_envelope_serialize(EOP_REGISTER, 1, payload, 5, buf, sizeof(buf));

    // Assert
    EXPECT_EQ(rc, EOP_ERR_NOT_ENOUGH_SPACE);
}

TEST(EnvelopeTest, SerializeWithInvalidMessageTypeFails)
{
    // Arrange
    uint8_t buf[64];

    // Act
    int rc = eop_envelope_serialize(static_cast<eop_message_type_t>(0x00), 1, nullptr, 0, buf, sizeof(buf));

    // Assert
    EXPECT_EQ(rc, EOP_ERR_INVALID_MESSAGE_TYPE);
}

TEST(EnvelopeTest, DeserializeWithWrongProtocolVersionFails)
{
    // Arrange — manually craft a buffer with version 99
    uint8_t buf[EOP_ENVELOPE_HEADER_SIZE];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 99;
    buf[1] = EOP_REGISTER;

    eop_message_type_t out_type;
    uint32_t out_id;
    const uint8_t* out_payload;
    size_t out_payload_len;

    // Act
    int rc = eop_envelope_deserialize(buf, sizeof(buf), &out_type, &out_id, &out_payload, &out_payload_len);

    // Assert
    EXPECT_EQ(rc, EOP_ERR_INVALID_ARGUMENT);
}

TEST(EnvelopeTest, DeserializeWithNullOutputPointersFails)
{
    // Arrange
    uint8_t buf[EOP_ENVELOPE_HEADER_SIZE];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = EOP_PROTOCOL_VERSION;
    buf[1] = EOP_REGISTER;

    // Act & Assert
    EXPECT_EQ(eop_envelope_deserialize(buf, sizeof(buf), nullptr, nullptr, nullptr, nullptr), EOP_ERR_INVALID_ARGUMENT);
}

TEST(EnvelopeTest, DeserializeWithInvalidMessageTypeFails)
{
    // Arrange — valid header but message_type = 0xFF
    uint8_t buf[EOP_ENVELOPE_HEADER_SIZE];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = EOP_PROTOCOL_VERSION;
    buf[1] = 0xFF;

    eop_message_type_t out_type;
    uint32_t out_id;
    const uint8_t* out_payload;
    size_t out_payload_len;

    // Act
    int rc = eop_envelope_deserialize(buf, sizeof(buf), &out_type, &out_id, &out_payload, &out_payload_len);

    // Assert
    EXPECT_EQ(rc, EOP_ERR_INVALID_MESSAGE_TYPE);
}

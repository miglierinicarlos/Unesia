#include "messageSerializer.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <cstring>

// Helpers

/// Extracts a uint32_t in big-endian from a raw byte pointer.
static uint32_t readBE32(const uint8_t* ptr)
{
    uint32_t val = 0;
    std::memcpy(&val, ptr, sizeof(val));
    return ntohl(val);
}

// Frame structure tests

TEST(messageSerializerTest, welcomeFrameIsWellFormed)
{
    const std::string payload = MessageSerializer::buildWelcomePayload(
        {.serverVersion = "0.1.0", .timestamp = "2026-03-30T12:00:00Z", .sessionId = "0000000a"});

    const uint32_t messageId = 1;
    const auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::ACK, messageId, payload);

    const uint32_t expectedFrameLen = MessageSerializer::ENVELOPE_SIZE + static_cast<uint32_t>(payload.size());

    // 4B prefix + envelope + payload
    ASSERT_EQ(frame.size(), MessageSerializer::FRAME_LENGTH_PREFIX_SIZE + expectedFrameLen);

    // Verify frame_length prefix
    const uint32_t frameLen = readBE32(frame.data());
    EXPECT_EQ(frameLen, expectedFrameLen);

    // Envelope starts at offset 4
    const uint8_t* env = frame.data() + MessageSerializer::FRAME_LENGTH_PREFIX_SIZE;

    // protocol_version = 0x01
    EXPECT_EQ(env[0], MessageSerializer::PROTOCOL_VERSION);

    // message_type = ACK (0x02)
    EXPECT_EQ(env[1], static_cast<uint8_t>(MessageSerializer::MessageType::ACK));

    // message_id
    EXPECT_EQ(readBE32(env + 2), messageId);

    // payload_length
    EXPECT_EQ(readBE32(env + 6), static_cast<uint32_t>(payload.size()));
}

TEST(messageSerializerTest, welcomeContainsRequiredFields)
{
    const std::string payload = MessageSerializer::buildWelcomePayload(
        {.serverVersion = "0.1.0", .timestamp = "2026-03-30T12:00:00Z", .sessionId = "0000000a"});

    const auto j = nlohmann::json::parse(payload);

    EXPECT_TRUE(j.contains("server_version"));
    EXPECT_TRUE(j.contains("timestamp"));
    EXPECT_TRUE(j.contains("session_id"));

    EXPECT_EQ(j["server_version"].get<std::string>(), "0.1.0");
    EXPECT_EQ(j["timestamp"].get<std::string>(), "2026-03-30T12:00:00Z");
    EXPECT_EQ(j["session_id"].get<std::string>(), "0000000a");
}

TEST(messageSerializerTest, emptyPayloadProducesValidFrame)
{
    const auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::HEARTBEAT, 42, "");

    // 4B prefix + 10B envelope + 0B payload
    ASSERT_EQ(frame.size(), MessageSerializer::FRAME_LENGTH_PREFIX_SIZE + MessageSerializer::ENVELOPE_SIZE);

    const uint32_t frameLen = readBE32(frame.data());
    EXPECT_EQ(frameLen, MessageSerializer::ENVELOPE_SIZE);

    // payload_length field should be 0
    const uint8_t* env = frame.data() + MessageSerializer::FRAME_LENGTH_PREFIX_SIZE;
    EXPECT_EQ(readBE32(env + 6), 0U);
}

TEST(messageSerializerTest, frameEnvelopeFieldsAreInCorrectOrder)
{
    const std::string payload = R"({"test":"data"})";
    const uint32_t msgId = 0x12345678;

    const auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::REGISTER, msgId, payload);
    const uint8_t* env = frame.data() + MessageSerializer::FRAME_LENGTH_PREFIX_SIZE;

    EXPECT_EQ(env[0], 0x01);                                             // protocol_version at offset 0
    EXPECT_EQ(env[1], 0x01);                                             // REGISTER = 0x01 at offset 1
    EXPECT_EQ(readBE32(env + 2), msgId);                                 // message_id at offset 2
    EXPECT_EQ(readBE32(env + 6), static_cast<uint32_t>(payload.size())); // payload_length at offset 6
}

TEST(messageSerializerTest, sendAllReturnsTrueForZeroSize)
{
    const uint8_t dummy = 0xAA;
    EXPECT_TRUE(MessageSerializer::sendAll(-1, &dummy, 0U));
}

TEST(messageSerializerTest, sendAllFailsWithInvalidFd)
{
    const std::array<uint8_t, 3> data = {0x01, 0x02, 0x03};
    EXPECT_FALSE(MessageSerializer::sendAll(-1, data.data(), data.size()));
}

TEST(messageSerializerTest, sendAllWritesAllBytesThroughSocketPair)
{
    std::array<int, 2> sv = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()), 0);

    const std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    std::vector<uint8_t> received(payload.size(), 0);

    std::thread reader(
        [&]()
        {
            std::size_t total = 0;
            while (total < received.size())
            {
                const ssize_t n = ::recv(sv[1], received.data() + total, received.size() - total, 0);
                ASSERT_GT(n, 0);
                total += static_cast<std::size_t>(n);
            }
        });

    EXPECT_TRUE(MessageSerializer::sendAll(sv[0], payload.data(), payload.size()));
    reader.join();

    EXPECT_EQ(received, payload);
    ::close(sv[0]);
    ::close(sv[1]);
}

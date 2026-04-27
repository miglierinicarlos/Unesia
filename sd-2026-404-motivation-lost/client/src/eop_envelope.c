#include "eop_envelope.h"
#include "eop_client.h"

#include <arpa/inet.h>
#include <string.h>

int eop_envelope_serialize(eop_message_type_t msg_type,
                           uint32_t msg_id,
                           const uint8_t* payload,
                           size_t length,
                           uint8_t* out_buf,
                           size_t out_len)
{
    if (out_len < EOP_ENVELOPE_HEADER_SIZE + length)
    {
        return (int)EOP_ERR_NOT_ENOUGH_SPACE; // Not enough space in output buffer
    }
    if (msg_type < EOP_REGISTER || msg_type > EOP_EDGE_TELEMETRY)
    {
        return (int)EOP_ERR_INVALID_MESSAGE_TYPE; // Invalid message type
    }
    if (payload == NULL && length > 0)
    {
        return (int)EOP_ERR_INVALID_ARGUMENT; // Payload is NULL but length is greater than 0
    }

    out_buf[0] = EOP_PROTOCOL_VERSION;
    out_buf[1] = (uint8_t)msg_type;
    uint32_t net_val = htonl(msg_id);
    memcpy(out_buf + 2, &net_val, sizeof(net_val));
    uint32_t len_val = htonl((uint32_t)length);
    memcpy(out_buf + 6, &len_val, sizeof(len_val));
    if (payload && length > 0)
    {
        memcpy(out_buf + EOP_ENVELOPE_HEADER_SIZE, payload, length);
    }
    return (int)EOP_OK;
}

int eop_envelope_deserialize(const uint8_t* buf,
                             size_t len,
                             eop_message_type_t* out_msg_type,
                             uint32_t* out_msg_id,
                             const uint8_t** out_payload,
                             size_t* out_payload_len)
{
    if (len < EOP_ENVELOPE_HEADER_SIZE)
    {
        return (int)EOP_ERR_INVALID_ARGUMENT; // Buffer too small to contain header
    }
    if (buf[0] != EOP_PROTOCOL_VERSION)
    {
        return (int)EOP_ERR_INVALID_ARGUMENT; // Unsupported protocol version
    }
    if (out_msg_type == NULL || out_msg_id == NULL || out_payload == NULL || out_payload_len == NULL)
    {
        return (int)EOP_ERR_INVALID_ARGUMENT; // Output pointers must not be NULL
    }
    uint8_t msg_type = buf[1];
    if (msg_type < EOP_REGISTER || msg_type > EOP_EDGE_TELEMETRY)
    {
        return (int)EOP_ERR_INVALID_MESSAGE_TYPE; // Invalid message type
    }
    uint32_t msg_id;
    memcpy(&msg_id, buf + 2, sizeof(msg_id));
    msg_id = ntohl(msg_id);
    uint32_t payload_len;
    memcpy(&payload_len, buf + 6, sizeof(payload_len));
    payload_len = ntohl(payload_len);
    if (len < EOP_ENVELOPE_HEADER_SIZE + payload_len)
    {
        return (int)EOP_ERR_INVALID_ARGUMENT; // Buffer too small for declared payload length
    }

    *out_msg_type = (eop_message_type_t)msg_type;
    *out_msg_id = msg_id;
    *out_payload = buf + EOP_ENVELOPE_HEADER_SIZE;
    *out_payload_len = payload_len;

    return (int)EOP_OK;
}

#ifndef EOP_ENVELOPE_H
#define EOP_ENVELOPE_H

#include "eop_client.h"

#define EOP_PROTOCOL_VERSION     1
#define EOP_ENVELOPE_HEADER_SIZE 10

struct eop_envelope
{
    uint8_t version;
    eop_message_type_t msg_type;
    uint32_t msg_id;
    uint32_t payload_length;
    const uint8_t* payload;
};

int eop_envelope_serialize(eop_message_type_t msg_type,
                           uint32_t msg_id,
                           const uint8_t* payload,
                           size_t length,
                           uint8_t* out_buf,
                           size_t out_len);

int eop_envelope_deserialize(const uint8_t* buf,
                             size_t len,
                             eop_message_type_t* out_msg_type,
                             uint32_t* out_msg_id,
                             const uint8_t** out_payload,
                             size_t* out_payload_len);

#endif // EOP_ENVELOPE_H

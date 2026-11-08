#pragma once
#include "wardrive_upload_reply.h"
#define CERBERUS_UPLOAD_RECEIPT_BYTES 128U
bool cerberus_upload_receipt_encode(const cerberus_upload_reply_t *reply,uint8_t bytes[128]);
bool cerberus_upload_receipt_decode(const uint8_t bytes[128],cerberus_upload_reply_t *reply);

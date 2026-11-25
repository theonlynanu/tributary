/**
 * Sending messages over a Socket and the client side of request/response
 * exchanges.
 *
 * Connections carry one request at a time. Client sends a request and waits for
 * its response before sending the next. Correlation id in the response must
 * match the request's.
 */
#pragma once

#include "net/frame.h"
#include "net/socket.h"
#include "protocol/messages.h"

namespace tributary {

/**
 * Encodes msg with its header and sends it as one frame.
 *
 * Anything but kOk means the connection should be closed.
 */
template <typename Msg>
IoStatus SendMessage(Socket& socket, int32_t correlation_id, const Msg& msg) {
    return SendFrame(socket, EncodeMessage(correlation_id, msg));
}

/**
 * Outcome of Call.
 */
enum class CallStatus {
    kOk,          // '*response' holds the decoded response
    kIoError,     // sending or receiving failed (check IoStatus)
    kBadResponse  // reply was wrong type, didn't decode, or had wrong
                  // correlation id
};

/**
 * Sends request and waits for its response.
 *
 * Uses the socket's receive timeout if one is set.
 *
 * Any result but kOk means the connection should be closed.
 */
template <typename Req, typename Resp>
CallStatus Call(Socket& socket, int32_t correlation_id, const Req& request,
                Resp* response, IoStatus* io = nullptr) {
    IoStatus status = SendMessage(socket, correlation_id, request);
    if (status == IoStatus::kOk) {
        std::vector<uint8_t> payload;
        status = RecvFrame(socket, &payload);
        if (status == IoStatus::kOk) {
            int32_t got_id = 0;
            if (!DecodeMessage(payload, response, &got_id) ||
                got_id != correlation_id) {
                if (io) {
                    *io = IoStatus::kOk;
                }
                return CallStatus::kBadResponse;
            }
            if (io) {
                *io = IoStatus::kOk;
            }
            return CallStatus::kOk;
        }
    }
    if (io) {
        *io = status;
    }
    return CallStatus::kIoError;
}

}  // namespace tributary

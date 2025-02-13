#include "h266.hh"

#include "../rtp.hh"
#include "../queue.hh"
#include "frame.hh"
#include "debug.hh"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <queue>


#ifndef _WIN32
#include <sys/socket.h>
#endif



uvgrtp::formats::h266::h266(uvgrtp::socket* socket, uvgrtp::rtp* rtp, int flags) :
    h26x(socket, rtp, flags)
{}

uvgrtp::formats::h266::~h266()
{
}

uint8_t uvgrtp::formats::h266::get_nal_header_size() const
{
    return uvgrtp::frame::HEADER_SIZE_H266_NAL;
}

uint8_t uvgrtp::formats::h266::get_fu_header_size() const
{
    return uvgrtp::frame::HEADER_SIZE_H266_FU;
}

uint8_t uvgrtp::formats::h266::get_nal_type(uint8_t* data) const
{
    return (data[1] >> 3) & 0x1f;
}

int uvgrtp::formats::h266::get_fragment_type(uvgrtp::frame::rtp_frame* frame) const
{
    bool first_frag = frame->payload[2] & 0x80;
    bool last_frag = frame->payload[2] & 0x40;

    if ((frame->payload[1] >> 3) != uvgrtp::formats::H266_PKT_FRAG)
        return uvgrtp::formats::FT_NOT_FRAG;

    if (first_frag && last_frag)
        return uvgrtp::formats::FT_INVALID;

    if (first_frag)
        return uvgrtp::formats::FT_START;

    if (last_frag)
        return uvgrtp::formats::FT_END;

    return uvgrtp::formats::FT_MIDDLE;
}

uvgrtp::formats::NAL_TYPES uvgrtp::formats::h266::get_nal_type(uvgrtp::frame::rtp_frame* frame) const
{
    switch (frame->payload[2] & 0x3f) {
    case 19: return uvgrtp::formats::NT_INTRA;
    case 1:  return uvgrtp::formats::NT_INTER;
    default: break;
    }

    return uvgrtp::formats::NT_OTHER;
}

rtp_error_t uvgrtp::formats::h266::handle_small_packet(uint8_t* data, size_t data_len, bool more)
{
    rtp_error_t ret = RTP_OK;

    if ((ret = fqueue_->enqueue_message(data, data_len)) != RTP_OK) {
        LOG_ERROR("enqeueu failed for small packet");
        return ret;
    }

    if (more)
        return RTP_NOT_READY;
    return fqueue_->flush_queue();
}

rtp_error_t uvgrtp::formats::h266::construct_format_header_divide_fus(uint8_t* data, size_t& data_left,
    size_t& data_pos, size_t payload_size, uvgrtp::buf_vec& buffers)
{
    auto headers = (uvgrtp::formats::h266_headers*)fqueue_->get_media_headers();

    headers->nal_header[0] = data[0];
    headers->nal_header[1] = (29 << 3) | (data[1] & 0x7);

    initialize_fu_headers(get_nal_type(data), headers->fu_headers);

    buffers.push_back(std::make_pair(sizeof(headers->nal_header), headers->nal_header));
    buffers.push_back(std::make_pair(sizeof(uint8_t), &headers->fu_headers[0]));
    buffers.push_back(std::make_pair(payload_size, nullptr));

    data_pos = uvgrtp::frame::HEADER_SIZE_H266_NAL;
    data_left -= uvgrtp::frame::HEADER_SIZE_H266_NAL;

    return divide_frame_to_fus(data, data_left, data_pos, payload_size, buffers, headers->fu_headers);
}
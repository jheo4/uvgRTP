#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2def.h>
#else
#include <netinet/in.h>
#endif

#include <string>
#include <vector>

#include "util.hh"

#define RTP_HEADER_LENGTH   12
#define RTCP_HEADER_LENGTH  12

namespace uvgrtp {
    namespace frame {
        enum HEADER_SIZES {
            HEADER_SIZE_RTP      = 12,
            HEADER_SIZE_OPUS     =  1,
            HEADER_SIZE_H264_NAL =  1,
            HEADER_SIZE_H264_FU  =  1,
            HEADER_SIZE_H265_NAL =  2,
            HEADER_SIZE_H265_FU  =  1,
            HEADER_SIZE_H266_NAL =  2,
            HEADER_SIZE_H266_FU  =  1,
        };

        enum RTP_FRAME_TYPE {
            RTP_FT_GENERIC = 0, /* payload length + RTP Header size (N + 12) */
            RTP_FT_OPUS    = 1, /* payload length + RTP Header size + Opus header (N + 12 + 0 [for now]) */
            RTP_FT_H265_FU = 2, /* payload length + RTP Header size + HEVC NAL Header + FU Header (N + 12 + 2 + 1) */
            RTP_FT_H266_FU = 2, /* payload length + RTP Header size + HEVC NAL Header + FU Header (N + 12 + 2 + 1) */
        };

        enum RTCP_FRAME_TYPE {
            RTCP_FT_SR   = 200, /* Sender report */
            RTCP_FT_RR   = 201, /* Receiver report */
            RTCP_FT_SDES = 202, /* Source description */
            RTCP_FT_BYE  = 203, /* Goodbye */
            RTCP_FT_APP  = 204  /* Application-specific message */
        };

        PACK(struct rtp_header {
            uint8_t version:2;
            uint8_t padding:1;
            uint8_t ext:1;
            uint8_t cc:4;
            uint8_t marker:1;
            uint8_t payload:7;
            uint16_t seq;
            uint32_t timestamp;
            uint32_t ssrc;
        });

        PACK(struct ext_header {
            uint16_t type;
            uint16_t len;
            uint8_t *data;
        });

        struct rtp_frame {
            struct rtp_header header;
            uint32_t *csrc;
            struct ext_header *ext;

            size_t padding_len; /* non-zero if frame is padded */
            size_t payload_len; /* payload_len: total_len - header_len - padding length (if padded) */

            /* Probation zone is a small area of free-to-use memory for the frame receiver
             * when handling fragments. For example HEVC fragments that belong to future frames
             * but cannot be relocated there (start sequence missing) are copied to probation
             * zone and when the frame becomes active, all fragments in the probation are relocated
             *
             * NOTE 1: Probation zone will increase the memory usage and will increase
             * the internal fragmentation as this memory is not usable for anything else
             *
             * NOTE 2: This is a Linux-only optimization */
            size_t probation_len;
            size_t probation_off;
            uint8_t *probation;
            uint8_t *payload;

            uint8_t *dgram;      /* pointer to the UDP datagram (for internal use only) */
            size_t   dgram_size; /* size of the UDP datagram */

            rtp_format_t format;
            int  type;
            sockaddr_in src_addr;
        };

        struct rtcp_header {
            uint8_t version;
            uint8_t padding;
            union {
                uint8_t count;
                uint8_t pkt_subtype; /* for app packets */
            };
            uint8_t pkt_type;
            uint16_t length;
        };

        struct rtcp_sender_info {
            uint32_t ntp_msw; /* NTP timestamp, most significant word */
            uint32_t ntp_lsw; /* NTP timestamp, least significant word */
            uint32_t rtp_ts;  /* RTP timestamp corresponding to same time as NTP */
            uint32_t pkt_cnt;
            uint32_t byte_cnt;
        };

        struct rtcp_report_block {
            uint32_t ssrc;
            uint8_t  fraction;
            int32_t  lost;
            uint32_t last_seq;
            uint32_t jitter;
            uint32_t lsr;  /* last Sender Report */
            uint32_t dlsr; /* delay since last Sender Report */
        };

        struct rtcp_receiver_report {
            struct rtcp_header header;
            uint32_t ssrc;
            std::vector<rtcp_report_block> report_blocks;
        };

        struct rtcp_sender_report {
            struct rtcp_header header;
            uint32_t ssrc;
            struct rtcp_sender_info sender_info;
            std::vector<rtcp_report_block> report_blocks;
        };

        struct rtcp_sdes_item {
            uint8_t type;
            uint8_t length;
            void *data;
        };

        struct rtcp_sdes_packet {
            struct rtcp_header header;
            uint32_t ssrc;
            std::vector<rtcp_sdes_item> items;
        };

        struct rtcp_app_packet {
            struct rtcp_header header;
            uint32_t ssrc;
            uint8_t name[4];
            uint8_t *payload;
        };

        PACK(struct zrtp_frame {
            uint8_t version:4;
            uint16_t unused:12;
            uint16_t seq;
            uint32_t magic;
            uint32_t ssrc;
            uint8_t payload[1];
        });

        /* Allocate an RTP frame
         *
         * First function allocates an empty RTP frame (no payload)
         *
         * Second allocates an RTP frame with payload of size "payload_len",
         *
         * Third allocate an RTP frame with payload of size "payload_len"
         * + probation zone of size "pz_size" * MAX_PAYLOAD
         *
         * Return pointer to frame on success
         * Return nullptr on error and set rtp_errno to:
         *    RTP_MEMORY_ERROR if allocation of memory failed */
        rtp_frame *alloc_rtp_frame();
        rtp_frame *alloc_rtp_frame(size_t payload_len);
        rtp_frame *alloc_rtp_frame(size_t payload_len, size_t pz_size);

        /* Allocate ZRTP frame
         * Parameter "payload_size" defines the length of the frame 
         *
         * Return pointer to frame on success
         * Return nullptr on error and set rtp_errno to:
         *    RTP_MEMORY_ERROR if allocation of memory failed
         *    RTP_INVALID_VALUE if "payload_size" is 0 */
        zrtp_frame *alloc_zrtp_frame(size_t payload_size);

        /* Deallocate RTP frame
         *
         * Return RTP_OK on successs
         * Return RTP_INVALID_VALUE if "frame" is nullptr */
        rtp_error_t dealloc_frame(uvgrtp::frame::rtp_frame *frame);

        /* Deallocate ZRTP frame
         *
         * Return RTP_OK on successs
         * Return RTP_INVALID_VALUE if "frame" is nullptr */
        rtp_error_t dealloc_frame(uvgrtp::frame::zrtp_frame *frame);
    };
};

namespace uvg_rtp = uvgrtp;

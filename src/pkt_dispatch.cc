#ifdef __linux__
#include <errno.h>

#else
#define MSG_DONTWAIT 0
#endif

#include <cstring>

#include "debug.hh"
#include "pkt_dispatch.hh"
#include "util.hh"

uvg_rtp::pkt_dispatcher::pkt_dispatcher():
    recv_hook_arg_(nullptr),
    recv_hook_(nullptr)
{
}

uvg_rtp::pkt_dispatcher::~pkt_dispatcher()
{
}

rtp_error_t uvg_rtp::pkt_dispatcher::start(uvg_rtp::socket *socket, int flags)
{
    if (!(runner_ = new std::thread(runner, this, socket, flags, &exit_mtx_)))
        return RTP_MEMORY_ERROR;

    runner_->detach();
    return uvg_rtp::runner::start();
}

rtp_error_t uvg_rtp::pkt_dispatcher::stop()
{
    active_ = false;

    while (!exit_mtx_.try_lock())
        ;

    return RTP_OK;
}

rtp_error_t uvg_rtp::pkt_dispatcher::install_receive_hook(
    void *arg,
    void (*hook)(void *, uvg_rtp::frame::rtp_frame *)
)
{
    if (!hook)
        return RTP_INVALID_VALUE;

    recv_hook_     = hook;
    recv_hook_arg_ = arg;

    return RTP_OK;
}

uvg_rtp::frame::rtp_frame *uvg_rtp::pkt_dispatcher::pull_frame()
{
    while (frames_.empty() && this->active())
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    if (!this->active())
        return nullptr;

    frames_mtx_.lock();
    auto frame = frames_.front();
    frames_.erase(frames_.begin());
    frames_mtx_.unlock();

    return frame;
}

uvg_rtp::frame::rtp_frame *uvg_rtp::pkt_dispatcher::pull_frame(size_t timeout)
{
    while (frames_.empty() && this->active() && timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        --timeout;
    }

    if (!this->active() && frames_.empty())
        return nullptr;

    frames_mtx_.lock();
    auto frame = frames_.front();
    frames_.erase(frames_.begin());
    frames_mtx_.unlock();

    return frame;
}

rtp_error_t uvg_rtp::pkt_dispatcher::install_handler(uvg_rtp::packet_handler handler)
{
    if (!handler)
        return RTP_INVALID_VALUE;

    packet_handlers_.push_back(handler);
    return RTP_OK;
}

std::vector<uvg_rtp::packet_handler>& uvg_rtp::pkt_dispatcher::get_handlers()
{
    return packet_handlers_;
}

void uvg_rtp::pkt_dispatcher::return_frame(uvg_rtp::frame::rtp_frame *frame)
{
    if (recv_hook_) {
        recv_hook_(recv_hook_arg_, frame);
    } else {
        frames_mtx_.lock();
        frames_.push_back(frame);
        frames_mtx_.unlock();
    }
}

/* The point of packet dispatcher is to provide much-needed isolation between different layers
 * of uvgRTP. For example, HEVC handler should not concern itself with RTP packet validation
 * because that should be a global operation done for all packets.
 *
 * Neither should Opus handler take SRTP-provided authentication tag into account when it is
 * performing operations on the packet.
 * And ZRTP packets should not be relayed from media handler to ZRTP handler et cetera.
 *
 * This can be achieved by having a global UDP packet handler for any packet type that validates
 * all common stuff it can and then dispatches the validated packet to the correct layer using
 * one of the installed handlers.
 *
 * If it's unclear as to which handler should be called, the packet is dispatched to all relevant
 * handlers and a handler then returns RTP_OK/RTP_PKT_NOT_HANDLED based on whether the packet was handled.
 *
 * For example, if runner detects an incoming ZRTP packet, that packet is immediately dispatched to the
 * installed ZRTP handler if ZRTP has been enabled.
 * Likewise, if RTP packet authentication has been enabled, runner validates the packet before passing
 * it onto any other layer so all future work on the packet is not done in vain due to invalid data
 *
 * One piece of design choice that complicates the design of packet dispatcher a little is that the order
 * of handlers is important. First handler must be ZRTP and then follows SRTP, RTP and finally media handlers.
 * This requirement gives packet handler a clean and generic interface while giving a possibility to modify
 * the packet in each of the called handlers if needed. For example SRTP handler verifies RTP authentication
 * tag and decrypts the packet and RTP handler verifies the fields of the RTP packet and processes it into
 * a more easily modifiable format for the media handler.
 *
 * If packet is modified by the handler but the frame is not ready to be returned to user,
 * handler returns RTP_PKT_MODIFIED to indicate that it has modified the input buffer and that
 * the packet should be passed onto other handlers.
 *
 * When packet is ready to be returned to user, "out" parameter of packet handler is set to point to
 * the allocated frame that can be returned and return value of the packet handler is RTP_PKT_READY.
 *
 * If a handler receives a non-null "out", it can safely ignore "packet" and operate just on
 * the "out" parameter because at that point it already contains all needed information. */
void uvg_rtp::pkt_dispatcher::runner(
    uvg_rtp::pkt_dispatcher *dispatcher,
    uvg_rtp::socket *socket,
    int flags,
    std::mutex *exit_mtx
)
{
    int nread;
    fd_set read_fds;
    rtp_error_t ret;
    struct timeval t_val;
    uvg_rtp::frame::rtp_frame *frame;

    FD_ZERO(&read_fds);

    t_val.tv_sec  = 0;
    t_val.tv_usec = 1500;

    const size_t recv_buffer_len = 8192;
    uint8_t recv_buffer[recv_buffer_len] = { 0 };

    while (!dispatcher->active())
        ;

    exit_mtx->lock();

    while (dispatcher->active()) {
        FD_SET(socket->get_raw_socket(), &read_fds);
        int sret = ::select(socket->get_raw_socket() + 1, &read_fds, nullptr, nullptr, &t_val);

        if (sret < 0) {
            log_platform_error("select(2) failed");
            break;
        }

        do {
            if ((ret = socket->recvfrom(recv_buffer, recv_buffer_len, MSG_DONTWAIT, &nread)) == RTP_INTERRUPTED)
                break;

            if (ret != RTP_OK) {
                LOG_ERROR("recvfrom(2) failed! Packet dispatcher cannot continue %d!", ret);
                break;
            }

            for (auto& handler : dispatcher->get_handlers()) {
                switch ((ret = (*handler)(nread, recv_buffer, flags, &frame))) {
                    /* packet was handled successfully */
                    case RTP_OK:
                        break;

                    /* "out" contains an RTP packet that can be returned to the user */
                    case RTP_PKT_READY:
                        dispatcher->return_frame(frame);
                        break;

                    /* the received packet is not handled at all or only partially by the called handler
                     * proceed to the next handler */
                    case RTP_PKT_NOT_HANDLED:
                    case RTP_PKT_MODIFIED:
                        continue;

                    case RTP_GENERIC_ERROR:
                        LOG_DEBUG("Received a corrupted packet!");
                        break;

                    default:
                        LOG_ERROR("Unknown error code from packet handler: %d", ret);
                        break;
                }

            }
        } while (ret == RTP_OK);
    }

    exit_mtx->unlock();
}

#include "cylinder_loader.h"

#include "rdata_stream.h"

CylinderLoader::Step CylinderLoader::service() {
    if (loading_ >= 0 && loading_ != wanted_) {
        // The head moved: start again for where it is now.
        ++restarts_;
        loading_ = -1;
    }
    if (loading_ < 0) {
        if (ready_.load(std::memory_order_relaxed) == wanted_) return Step::Idle;
        loading_ = wanted_;
        next_ = Step::ReadHead0;
    }

    const ibm::Format& fmt = source_.format();
    uint8_t cyl = uint8_t(loading_);
    Step done = next_;
    switch (next_) {
        case Step::ReadHead0:
        case Step::ReadHead1: {
            uint8_t head = next_ == Step::ReadHead0 ? 0 : 1;
            present_[head] = source_.readTrack(cyl, head, data_[head]);
            next_ = head == 0 ? Step::ReadHead1 : Step::EncodeHead0;
            break;
        }
        case Step::EncodeHead0:
        case Step::EncodeHead1: {
            uint8_t head = next_ == Step::EncodeHead0 ? 0 : 1;
            ready_.store(-1, std::memory_order_release);  // before the first write into the live buffer
            if (!present_[head] || !ibm::buildTrack(fmt, cyl, head, data_[head], scratch_))
                ibm::buildBlankTrack(fmt, scratch_);
            rdata::setHead(head, scratch_, fmt.track_bytes, slots_);
            if (head == 0) {
                next_ = Step::EncodeHead1;
            } else {
                ready_.store(loading_, std::memory_order_release);  // after the last
                loading_ = -1;
                next_ = Step::Idle;
                ++loads_;
            }
            break;
        }
        case Step::Idle:
            break;
    }
    return done;
}

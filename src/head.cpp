#include "head.h"

namespace head {

const char* stepName(Step s) {
    switch (s) {
        case Step::In: return "IN";
        case Step::Out: return "OUT";
        case Step::AtOuterStop: return "OUTER_STOP";
        case Step::AtInnerStop: return "INNER_STOP";
    }
    return "?";
}

Step Head::step(bool inwards) {
    if (inwards) {
        if (cylinder_ >= last_) return Step::AtInnerStop;
        ++cylinder_;
        return Step::In;
    }
    if (cylinder_ == 0) return Step::AtOuterStop;
    --cylinder_;
    return Step::Out;
}

}  // namespace head

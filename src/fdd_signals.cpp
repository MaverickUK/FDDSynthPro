#include "fdd_signals.h"

namespace fdd {

const char* signalName(unsigned pin) {
    switch (pin) {
        case cfg::PIN_DENSEL: return "DENSEL";
        case cfg::PIN_MOTEA: return "MOTEA";
        case cfg::PIN_DRVSB: return "DRVSB";
        case cfg::PIN_DRVSA: return "DRVSA";
        case cfg::PIN_MOTEB: return "MOTEB";
        case cfg::PIN_DIR: return "DIR";
        case cfg::PIN_STEP: return "STEP";
        case cfg::PIN_WDATA: return "WDATA";
        case cfg::PIN_WGATE: return "WGATE";
        case cfg::PIN_SIDE1: return "SIDE1";
        case cfg::PIN_INDEX: return "INDEX";
        case cfg::PIN_TRK00: return "TRK00";
        case cfg::PIN_WPT: return "WPT";
        case cfg::PIN_RDATA: return "RDATA";
        case cfg::PIN_DSKCHG: return "DSKCHG";
        case cfg::PIN_CHNG: return "CHNG";
        default: return "?";
    }
}

}  // namespace fdd

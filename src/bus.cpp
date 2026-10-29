#include "bus.h"

#include <cstring>

namespace bus {

bool parseInterface(const char* name, Interface& out) {
    if (std::strcmp(name, "ibmpc") == 0) out = Interface::IbmPc;
    else if (std::strcmp(name, "shugart") == 0) out = Interface::Shugart;
    else return false;
    return true;
}

bool parseCable(const char* name, Cable& out) {
    if (std::strcmp(name, "twisted") == 0) out = Cable::Twisted;
    else if (std::strcmp(name, "straight") == 0) out = Cable::Straight;
    else return false;
    return true;
}

bool parseUnit(const char* name, uint8_t& out) {
    if (std::strcmp(name, "a") == 0 || std::strcmp(name, "A") == 0) out = 0;
    else if (std::strcmp(name, "b") == 0 || std::strcmp(name, "B") == 0) out = 1;
    else if (std::strncmp(name, "ds", 2) == 0 && name[2] >= '0' && name[2] < char('0' + SHUGART_UNITS) && !name[3])
        out = uint8_t(name[2] - '0');
    else return false;
    return true;
}

const char* interfaceName(Interface i) { return i == Interface::Shugart ? "shugart" : "ibmpc"; }

const char* cableName(Cable c) { return c == Cable::Straight ? "straight" : "twisted"; }

const char* unitName(const Setup& s) {
    if (s.interface == Interface::Shugart) {
        static const char* const NAMES[SHUGART_UNITS] = {"ds0", "ds1", "ds2"};
        return NAMES[s.unit < SHUGART_UNITS ? s.unit : 0];
    }
    return s.unit == 1 ? "b" : "a";
}

}  // namespace bus

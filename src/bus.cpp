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

// One unit's name, `len` characters of it.
static bool parseUnit(const char* name, size_t len, uint8_t& out) {
    if (len == 1 && (name[0] == 'a' || name[0] == 'A')) out = 0;
    else if (len == 1 && (name[0] == 'b' || name[0] == 'B')) out = 1;
    else if (len == 3 && std::strncmp(name, "ds", 2) == 0 && name[2] >= '0' && name[2] < char('0' + SHUGART_UNITS))
        out = uint8_t(name[2] - '0');
    else return false;
    return true;
}

bool parseUnits(const char* names, uint8_t& out) {
    uint8_t units = 0;
    unsigned count = 0;
    for (const char* at = names;;) {
        const char* end = std::strchr(at, ',');
        size_t len = end ? size_t(end - at) : std::strlen(at);
        uint8_t unit;
        if (!parseUnit(at, len, unit) || (units >> unit) & 1 || ++count > MAX_DRIVES) return false;
        units |= uint8_t(1u << unit);
        if (!end) break;
        at = end + 1;
    }
    out = units;
    return true;
}

const char* interfaceName(Interface i) { return i == Interface::Shugart ? "shugart" : "ibmpc"; }

const char* cableName(Cable c) { return c == Cable::Straight ? "straight" : "twisted"; }

const char* unitName(const Setup& s, uint8_t unit) {
    if (s.interface == Interface::Shugart) {
        static const char* const NAMES[SHUGART_UNITS] = {"ds0", "ds1", "ds2"};
        return NAMES[unit < SHUGART_UNITS ? unit : 0];
    }
    return unit == 1 ? "b" : "a";
}

const char* unitsName(const Setup& s, char (&out)[12]) {
    out[0] = 0;
    for (unsigned i = 0; i < s.drives(); ++i) {
        if (i) std::strcat(out, ",");
        std::strcat(out, unitName(s, s.unit(i)));
    }
    return out;
}

}  // namespace bus

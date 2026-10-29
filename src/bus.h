// How the drive sits on the cable, GoTek style: which lines select it and
// run its motor (a real drive's DS jumpers), and what pin 34 carries
// (FlashFloppy's `interface`). Pure C++, host-tested; read from
// FDDSYNTH.CFG (settings.h), described in docs/config.md.
//
// IBM PC: drives use the lines in pairs, select with motor. The host drives
// A on pins 14/10 (DRVSA, MOTEA) and B on 12/16 (DRVSB, MOTEB); a twisted
// cable swaps the pairs on its end connector, so a drive past the twist
// sees A on 12/16. That's where a PC drive answers, which is why it never
// needs its jumper moved. Pin 34 is DISK CHANGE; pin 2 is the host's density
// select.
//
// Shugart (the Atari ST, Amstrad, synths, most non-PC machines): one select
// line per drive, DS0-DS2 on pins 10, 12 and 14 (DS3, pin 6, isn't wired
// on the carrier), and one MOTOR ON line, pin 16, for all of them. Pin 34
// is READY: a disk in and the motor at speed. Pin 2 would be DISK CHANGE,
// which the carrier can't drive yet (it's an input), so Shugart hosts that
// want it have to watch READY instead.
#pragma once

#include <cstdint>

#include "config.h"

namespace bus {

enum class Interface : uint8_t { IbmPc, Shugart };
enum class Cable : uint8_t { Twisted, Straight };

// The lines one drive answers on.
struct Lines {
    unsigned select;
    unsigned motor;
    bool operator==(const Lines& o) const { return select == o.select && motor == o.motor; }
};

struct Setup {
    Interface interface = Interface::IbmPc;
    Cable cable = Cable::Twisted;  // IBM PC only
    // IBM PC: 0 = A, 1 = B. Shugart: DS0-DS2.
    uint8_t unit = 0;

    bool operator==(const Setup& o) const {
        return interface == o.interface && cable == o.cable && unit == o.unit;
    }
};

constexpr uint8_t PC_UNITS = 2;
constexpr uint8_t SHUGART_UNITS = 3;

// The default, a PC drive past the twist (the host's A), answers on 12/16:
// the Feather host's drive A, and a Greaseweazle's B on a straight cable.
inline Lines lines(const Setup& s) {
    if (s.interface == Interface::Shugart) {
        constexpr unsigned SELECT[SHUGART_UNITS] = {cfg::PIN_MOTEA, cfg::PIN_DRVSB, cfg::PIN_DRVSA};  // pins 10, 12, 14
        return {SELECT[s.unit < SHUGART_UNITS ? s.unit : 0], cfg::PIN_MOTEB};                       // MOTOR ON, pin 16
    }
    constexpr Lines PAIR_12_16{cfg::PIN_DRVSB, cfg::PIN_MOTEB};
    constexpr Lines PAIR_14_10{cfg::PIN_DRVSA, cfg::PIN_MOTEA};
    bool a = s.unit == 0 || s.unit >= PC_UNITS;
    bool twisted = s.cable == Cable::Twisted;
    return a == twisted ? PAIR_12_16 : PAIR_14_10;
}

// For FDDSYNTH.CFG and the console. Each returns false, and leaves `out`
// alone, for a name it doesn't know.
bool parseInterface(const char* name, Interface& out);
bool parseCable(const char* name, Cable& out);
// "a" or "b" (IBM PC), "ds0" to "ds2" (Shugart). Either kind is taken
// whatever the interface (it may come later in the file); a unit the
// interface doesn't have is A or DS0.
bool parseUnit(const char* name, uint8_t& out);

const char* interfaceName(Interface i);  // "ibmpc", "shugart"
const char* cableName(Cable c);          // "twisted", "straight"
const char* unitName(const Setup& s);    // "a", "b", "ds0" ...

}  // namespace bus

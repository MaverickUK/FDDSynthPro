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
// is READY: a disk in and the motor at speed. Pin 2 is DISK CHANGE, which
// the carrier drives only after a rework (config.h: PIN_CHNG) and with
// `pin02=chg`; without it, Shugart hosts have to watch READY instead.
//
// Amiga: Shugart-like. DF0 selects on pin 10, DF1 on pin 12; the motor
// line (pin 16) is latched as the drive is selected, so each drive keeps
// its own motor state. Pin 34 is /RDY: the motor at speed, and while the
// motor is off, the drive's ID bits (all asserted: a standard 3.5-inch DD
// drive). Pin 2 is /CHNG, as Shugart's: without it, the Amiga never sees
// a disk change.
#pragma once

#include <cstdint>

#include "config.h"

namespace bus {

enum class Interface : uint8_t { IbmPc, Shugart, Amiga };
enum class Cable : uint8_t { Twisted, Straight };

// The lines one drive answers on.
struct Lines {
    unsigned select;
    unsigned motor;
    bool operator==(const Lines& o) const { return select == o.select && motor == o.motor; }
};

constexpr uint8_t PC_UNITS = 2;       // A, B
constexpr uint8_t SHUGART_UNITS = 3;  // DS0-DS2
constexpr uint8_t AMIGA_UNITS = 2;    // DF0, DF1 (the 34-way cable's DS0 and DS1)
// The emulator can be one drive or two: each has its own image in PSRAM
// and its own cylinder buffer in RAM.
constexpr unsigned MAX_DRIVES = 2;

struct Setup {
    Interface interface = Interface::IbmPc;
    Cable cable = Cable::Twisted;  // IBM PC only
    // The units it answers as, bit n for unit n: IBM PC A (0) and B (1);
    // Shugart DS0-DS2; Amiga DF0, DF1. One or two bits.
    uint8_t units = 1;
    // Floppy pin 2 driven as DISK CHANGE (`pin02=chg`): Shugart and Amiga
    // only, and only on a reworked carrier (config.h: PIN_CHNG).
    bool pin2_chng = false;

    bool operator==(const Setup& o) const {
        return interface == o.interface && cable == o.cable && units == o.units && pin2_chng == o.pin2_chng;
    }

    unsigned drives() const { return (units & 1) + ((units >> 1) & 1) + ((units >> 2) & 1); }
    // The unit number of drive `i` (0 or 1), lowest first: A before B.
    uint8_t unit(unsigned i) const {
        for (uint8_t u = 0; u < 8; ++u)
            if ((units >> u) & 1 && i-- == 0) return u;
        return 0;
    }
};

// Keeps only the units the interface has (A and B on a PC), and at least
// one: a Shugart setup's DS2 would otherwise be nothing on a PC.
inline Setup normalised(Setup s) {
    unsigned units = s.interface == Interface::IbmPc ? PC_UNITS
                     : s.interface == Interface::Amiga ? AMIGA_UNITS
                                                       : SHUGART_UNITS;
    s.units &= (1u << units) - 1;
    if (!s.units) s.units = 1;
    if (s.interface == Interface::IbmPc) s.pin2_chng = false;  // pin 2 is the PC's: its density select
    return s;
}

// The lines `unit` answers on. The default, a PC drive past the twist (the
// host's A), answers on 12/16: the Feather host's drive A, and a
// Greaseweazle's B on a straight cable.
inline Lines unitLines(const Setup& s, uint8_t unit) {
    if (s.interface != Interface::IbmPc) {
        constexpr unsigned SELECT[SHUGART_UNITS] = {cfg::PIN_MOTEA, cfg::PIN_DRVSB, cfg::PIN_DRVSA};  // pins 10, 12, 14
        return {SELECT[unit < SHUGART_UNITS ? unit : 0], cfg::PIN_MOTEB};                           // MOTOR ON, pin 16
    }
    constexpr Lines PAIR_12_16{cfg::PIN_DRVSB, cfg::PIN_MOTEB};
    constexpr Lines PAIR_14_10{cfg::PIN_DRVSA, cfg::PIN_MOTEA};
    bool a = unit != 1;
    bool twisted = s.cable == Cable::Twisted;
    return a == twisted ? PAIR_12_16 : PAIR_14_10;
}

// The lines drive `i` answers on.
inline Lines lines(const Setup& s, unsigned i = 0) { return unitLines(s, s.unit(i)); }

// For FDDSYNTH.CFG and the console. Each returns false, and leaves `out`
// alone, for a name it doesn't know.
bool parseInterface(const char* name, Interface& out);
bool parseCable(const char* name, Cable& out);
// One unit or two, comma-separated: "a", "b", "a,b" (IBM PC); "ds0" to
// "ds2", "ds0,ds1" (Shugart); "df0", "df1", "df0,df1" (Amiga). Any kind is
// taken whatever the interface (it may come later in the file); a unit the
// interface doesn't have is A, DS0 or DF0. Refuses more than MAX_DRIVES, or the same unit twice.
bool parseUnits(const char* names, uint8_t& out);

const char* interfaceName(Interface i);  // "ibmpc", "shugart", "amiga"
const char* cableName(Cable c);          // "twisted", "straight"
const char* unitName(const Setup& s, uint8_t unit);  // "a", "b", "ds0", "df0" ...
// "a", "a,b", "ds0,ds2": as parseUnits takes them. Returns `out`.
const char* unitsName(const Setup& s, char (&out)[12]);

}  // namespace bus

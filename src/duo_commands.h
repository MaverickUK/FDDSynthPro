// What the drive answers through SynthMinder's mailbox (mailbox.h): the
// `$FD…` sentences. Pure C++, host-tested (test_mailbox.cpp) against a
// pretend drive; main.cpp's `Duo` is the real one.
//
// Requests (`$FDQ` asks, `$FDC` changes) and their replies:
//   FDQ,1,STATUS          one $FDV,1,<key>,<value> per value
//   FDQ,1,FILES[,<from>]  $FDF,1,<index>,<count>,<name>,<KB>,<kind> per image
//                         from image <from> (0 if left out), as many as fit;
//                         SynthMinder asks again from the next. Kind: I a
//                         sector image, H a flux image (HFE), A an Amiga disk
//   FDQ,1,PACKS           $FDP,1,<index>,<count>,<name> per sample pack
//   FDQ,1,SETTINGS        $FDS,1,<key>,<next>,<now> per setting SET changes
//   FDC,1,INSERT,<A|B>,<name>  the disk into that drive, as the front panel
//   FDC,1,EJECT,<A|B>
//   FDC,1,PACK,<index>
//   FDC,1,VOL,<0-100>
//   FDC,1,SET,<key>,<value>    into FDDSYNTH.CFG, from the next power-on (a
//                              value may hold commas: `drive`'s `a,b`)
// Each change answers $FDK,1,<what>,OK, or $FDK,1,<what>,ERR,<reason>.
// A request that isn't a sentence: $FDK,1,REQUEST,ERR,invalid; one it
// doesn't know: $FDK,1,<its first word>,ERR,unknown. Values never hold `,`,
// `*` or `$` (they become `_`).
#pragma once

#include <cstddef>
#include <cstdint>

#include "ibm_track.h"
#include "mailbox.h"

namespace duo {

struct DriveInfo {
    char letter = 'A';            // the PC's name for it
    bool disk_in = false;
    const char* disk = "";        // the image's name, or "welcome" / "generated"
    uint32_t kb = 0;
    char format[32] = "";         // describe()'s
    unsigned cylinder = 0;
    bool motor = false;
    bool write_protected = false;
};

struct Setting {
    const char* key;
    const char* next;  // FDDSYNTH.CFG's
    const char* now;   // what the drive runs with
};

// The drive SynthMinder talks to. Changes return nullptr when done (or
// started: a swap takes a second), or the reason they weren't.
class Duo {
public:
    virtual const char* firmware() const = 0;
    virtual unsigned drives() const = 0;
    virtual void driveInfo(unsigned drive, DriveInfo& out) const = 0;
    virtual int volume() const = 0;
    virtual size_t packCount() const = 0;
    virtual size_t packIndex() const = 0;
    virtual const char* packName(size_t index) const = 0;
    virtual size_t imageCount() const = 0;
    virtual const char* imageName(size_t index) const = 0;
    virtual uint32_t imageKB(size_t index) const = 0;
    virtual size_t settingCount() const = 0;
    virtual Setting setting(size_t index) const = 0;

    // `asked_on`: the drive the request came through. A swap of that drive
    // waits until the head leaves the mailbox, so the PC can read the reply.
    virtual const char* insert(unsigned drive, size_t image, unsigned asked_on) = 0;
    virtual const char* eject(unsigned drive, unsigned asked_on) = 0;
    virtual const char* setPack(size_t index) = 0;
    virtual const char* setVolume(int volume) = 0;
    virtual const char* set(const char* key, const char* value) = 0;

protected:
    ~Duo() = default;
};

// The reply to one request (`$...*CS`, CR LF or not), as sentences each
// ending in a line feed; returns their length.
uint32_t answer(Duo& duo, unsigned drive, const char* request, uint32_t n, char* out, uint32_t cap);

// The mailbox's handler over a Duo.
class MailboxHandler final : public mailbox::Handler {
public:
    explicit MailboxHandler(Duo& duo) : duo_(duo) {}
    uint32_t answer(unsigned drive, const char* request, uint32_t n, char* reply, uint32_t cap) override {
        return duo::answer(duo_, drive, request, n, reply, cap);
    }

private:
    Duo& duo_;
};

// A disk's format in words: "1440 KB MFM 300 rpm", "flux 300 rpm", "Amiga DD".
void describe(const ibm::Format& fmt, char* out, size_t cap);

// An image's kind from its name: 'H' a flux image, 'A' an Amiga disk, 'I' a sector image.
char imageKind(const char* name);

}  // namespace duo

// SynthMinder's mailbox: a cylinder past the end of the disk where the PC
// and the drive exchange sectors. Pure C++, host-tested (test_mailbox.cpp).
//
// The floppy cable carries no commands, so SynthMinder (a DOS program) talks
// to the drive in sectors, through the BIOS (INT 13h):
//   1. It writes a request to cylinder 82, head 0, sector 1: the same
//      512-byte SMND sector as its IDE command (SynthMinder's
//      docs/protocol.md), holding one sentence (`$FDQ,1,STATUS*CS`).
//   2. The drive decodes the write as it decodes any write, answers it at
//      once (duo_commands.h), and puts the reply in sectors 5-9 of the same
//      track, with the request's sequence number.
//   3. SynthMinder reads sectors 5-9. If they still carry the last sequence
//      number, the drive hadn't answered yet: it reads them again on the
//      next revolution.
// Before its first request SynthMinder only reads: sectors 5-9 hold a hello
// from power-on, so a real drive (no track 82: sector not found) is never
// written to.
//
// Sector 5 is four sector slots after sector 1, at least 44 ms at 300 rpm:
// time for the write's decode and the head's re-encode (about 4 and 3 ms on
// the board), so the reply is normally there on the first revolution.
//
// Cylinder 82 is past the last of an 80-track disk and inside the head's
// travel (head::DEFAULT_LAST_CYLINDER is 83). MailboxSource serves it, and
// never passes it to the disk: the user's image is never touched. It sits
// between the drive's DiskSlot and its loader and writer, so neither knows.
// Cylinders between the disk's end and 82 stay as before: blank, and writes
// to them dropped.
//
// It works for the PC's own disks (IBM MFM, single-stepped, at most 82
// cylinders, at least 9 sectors of 512 bytes numbered from 1: 720 KB,
// 1.2 MB, 1.44 MB, 1.68 MB DMF). The track is laid out as the disk's, at
// its data rate, which is the rate the BIOS has set the controller to for
// the disk in. Other disks (flux images, Amiga disks, FM, 40-track) have no
// mailbox, and nor does an empty drive.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ibm_track.h"
#include "track_source.h"

class CylinderLoader;

namespace mailbox {

constexpr uint8_t CYLINDER = 82;
constexpr uint8_t HEAD = 0;
constexpr uint8_t REQUEST = 0;        // sector index: R = 1
constexpr uint8_t FIRST_REPLY = 4;    // R = 5
constexpr uint8_t REPLY_SECTORS = 5;  // R = 5-9
constexpr uint32_t SECTOR = 512;
constexpr uint32_t HEADER = 16;
constexpr uint32_t TEXT_MAX = SECTOR - HEADER;  // 496
constexpr uint32_t REPLY_TEXT_MAX = REPLY_SECTORS * TEXT_MAX;
constexpr uint8_t VERSION = 1;
constexpr uint8_t FLAG_MORE = 1;  // the reply continues in the next sector

// Whether `fmt` can have the mailbox (see the top).
bool eligible(const ibm::Format& fmt);

struct Request {
    uint16_t seq = 0;
    uint32_t len = 0;
    char text[TEXT_MAX + 1] = {};
};

// False unless `sector` is a version 1 SMND sector with sensible lengths.
bool parseRequest(const uint8_t* sector, Request& out);

// Packs `text` (sentences, each ending in a line feed) into REPLY_SECTORS
// sectors at `out`, each with its header: the sequence number, the text's
// length in that sector, and FLAG_MORE on every one but the last used. A
// sentence is never split; one that doesn't fit is left out, with every
// one after it. Returns the bytes of text placed.
uint32_t buildReply(uint16_t seq, const char* text, uint32_t len, uint8_t* out);

// What answers a request: duo_commands.h's, over the firmware's state.
class Handler {
public:
    // `drive`: which of the emulator's drives the request came on. Writes
    // the reply's sentences to `reply` (room `cap`) and returns their length.
    virtual uint32_t answer(unsigned drive, const char* request, uint32_t n, char* reply, uint32_t cap) = 0;

protected:
    ~Handler() = default;
};

class MailboxSource : public TrackSource {
public:
    // From the start, the reply sectors hold `$FDK,1,HELLO,OK,` with
    // sequence 0, so SynthMinder can find the drive by reading alone: it
    // never writes to a drive that isn't one.
    MailboxSource(TrackSource& disk, unsigned drive);

    void setHandler(Handler* handler) { handler_ = handler; }
    // The loader that reads this source: a reply goes into its track data
    // at once, so the write that brought the request shows it.
    void setLoader(CylinderLoader* loader) { loader_ = loader; }

    // The disk in has a mailbox.
    bool active() const { return eligible(disk_.format()); }

    const ibm::Format& format() const override;
    bool readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) override;
    void readCylinder(uint8_t cylinder, uint8_t* out0, uint8_t* out1, bool ok[2]) override;
    bool writeSector(uint8_t cylinder, uint8_t head, uint8_t sector, const uint8_t* data) override;
    bool writeTrackBytes(uint8_t c, uint8_t h, uint32_t at, const uint8_t* data, uint32_t n) override {
        return disk_.writeTrackBytes(c, h, at, data, n);
    }
    bool flush() override { return disk_.flush(); }
    bool writable() const override { return disk_.writable(); }

    // Requests answered, repeats of the last one (the PC wrote it again: not
    // run twice), and sectors at cylinder 82 that weren't a request.
    uint32_t exchanges() const { return exchanges_; }
    uint32_t repeats() const { return repeats_; }
    uint32_t rejected() const { return rejected_; }
    uint16_t lastSeq() const { return last_seq_; }

private:
    bool mailboxCylinder(uint8_t cylinder) const { return cylinder == CYLINDER && active(); }
    void fillTrack(uint8_t head, uint8_t* out) const;
    void take(const uint8_t* sector);

    TrackSource& disk_;
    unsigned drive_;
    Handler* handler_ = nullptr;
    CylinderLoader* loader_ = nullptr;
    // The disk's format with cylinder 82 added. Rebuilt on every call: the
    // disk's format changes in place when an image is swapped in.
    mutable ibm::Format extended_{};
    uint8_t request_[SECTOR] = {};
    uint8_t reply_[REPLY_SECTORS * SECTOR] = {};
    char text_[REPLY_TEXT_MAX] = {};
    bool answered_ = false;  // a request has been answered (the hello isn't one)
    uint16_t last_seq_ = 0;
    uint32_t exchanges_ = 0, repeats_ = 0, rejected_ = 0;
};

}  // namespace mailbox

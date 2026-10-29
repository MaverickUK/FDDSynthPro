// The disk images on the SD card, in name order, for choosing between them
// GoTek style, and the sample packs. Pure C++, host-tested; sd_image.cpp
// fills them from the card.
#pragma once

#include <strings.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "track_source.h"

class ImageList {
public:
    static constexpr size_t MAX = 64;
    static constexpr size_t NAME = 64;

    void clear() { count_ = 0; }

    // A raw image's name: .img, .ima or .dsk (FlashFloppy's), or .adf (an
    // Amiga disk), any case.
    static bool imageName(const char* name) {
        size_t n = std::strlen(name);
        return n > 4 && n < NAME &&
               (strcasecmp(name + n - 4, ".img") == 0 || strcasecmp(name + n - 4, ".ima") == 0 ||
                strcasecmp(name + n - 4, ".dsk") == 0 || adfName(name));
    }
    static bool adfName(const char* name) {
        size_t n = std::strlen(name);
        return n > 4 && strcasecmp(name + n - 4, ".adf") == 0;
    }

    // Adds an image (the caller has found its format). Keeps the list in
    // name order, ignoring case. Returns whether it was added.
    bool add(const char* name, uint32_t size) {
        if (!imageName(name) || count_ == MAX) return false;
        size_t at = count_;
        while (at > 0 && strcasecmp(name, entries_[at - 1].name) < 0) {
            entries_[at] = entries_[at - 1];
            --at;
        }
        std::strcpy(entries_[at].name, name);
        entries_[at].size = size;
        ++count_;
        return true;
    }

    size_t count() const { return count_; }
    const char* name(size_t i) const { return entries_[i].name; }
    // Where `name` is (ignoring case), or count() if it isn't.
    size_t find(const char* name) const {
        for (size_t i = 0; i < count_; ++i)
            if (strcasecmp(entries_[i].name, name) == 0) return i;
        return count_;
    }
    uint32_t size(size_t i) const { return entries_[i].size; }
    // The next and previous image, wrapping round.
    size_t next(size_t i) const { return count_ ? (i + 1) % count_ : 0; }
    size_t prev(size_t i) const { return count_ ? (i + count_ - 1) % count_ : 0; }

private:
    struct Entry {
        char name[NAME];
        uint32_t size;
    };
    Entry entries_[MAX];
    size_t count_ = 0;
};

// The FDD Synth's sample packs on the card: the folders under /samples, in
// name order, ignoring case. Pure C++, host-tested; sd_image.cpp fills it.
class PackList {
public:
    static constexpr size_t MAX = 16;
    static constexpr size_t NAME = ImageList::NAME;

    void clear() { count_ = 0; }

    bool add(const char* name) {
        size_t n = std::strlen(name);
        if (!n || n >= NAME || name[0] == '.' || count_ == MAX) return false;  // not hidden folders
        size_t at = count_;
        while (at > 0 && strcasecmp(name, names_[at - 1]) < 0) {
            std::strcpy(names_[at], names_[at - 1]);
            --at;
        }
        std::strcpy(names_[at], name);
        ++count_;
        return true;
    }

    size_t count() const { return count_; }
    const char* name(size_t i) const { return names_[i]; }
    // Where `name` is (ignoring case), or count() if it isn't.
    size_t find(const char* name) const {
        for (size_t i = 0; i < count_; ++i)
            if (strcasecmp(names_[i], name) == 0) return i;
        return count_;
    }
    size_t next(size_t i) const { return count_ ? (i + 1) % count_ : 0; }
    size_t prev(size_t i) const { return count_ ? (i + count_ - 1) % count_ : 0; }

private:
    char names_[MAX][NAME];
    size_t count_ = 0;
};

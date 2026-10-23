// The disk images on the SD card, in name order, for choosing between them
// GoTek style. Pure C++, host-tested; sd_image.cpp fills it from the card.
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

    // Adds a file if it's an image we serve: .img or .ima (any case), 720 KB
    // or 1.44 MB. Keeps the list in name order, ignoring case. Returns
    // whether it was added.
    bool add(const char* name, uint32_t size) {
        size_t n = std::strlen(name);
        bool image_name = n > 4 && n < NAME && (strcasecmp(name + n - 4, ".img") == 0 ||
                                                strcasecmp(name + n - 4, ".ima") == 0);
        if (!image_name || !formatForImageSize(size) || count_ == MAX) return false;
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

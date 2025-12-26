#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <furi.h>
#include <storage/storage.h>

class EEPROMClass {
public:
    static constexpr int kSize = 1024;

    EEPROMClass();

    // autosave_interval_ms: 0 = только ручной commit()
    void begin(const char* path = "/ext/eeprom.bin", uint32_t autosave_interval_ms = 0);

    int length() const { return kSize; }

    uint8_t read(int addr) const;
    void write(int addr, uint8_t value);
    void update(int addr, uint8_t value);

    template<typename T>
    T& get(int addr, T& out) const {
        ensureLoaded_();
        if(addr < 0 || addr + (int)sizeof(T) > kSize) return out;
        memcpy(&out, mem_ + addr, sizeof(T));
        return out;
    }

    template<typename T>
    const T& put(int addr, const T& in) {
        ensureLoaded_();
        if(addr < 0 || addr + (int)sizeof(T) > kSize) return in;

        bool changed = false;
        const uint8_t* src = reinterpret_cast<const uint8_t*>(&in);
        for(size_t i = 0; i < sizeof(T); i++) {
            int a = addr + (int)i;
            if(mem_[a] != src[i]) {
                mem_[a] = src[i];
                changed = true;
            }
        }
        if(changed) dirty_ = true;
        return in;
    }

    void clear(uint8_t value = 0);
    void tick();
    bool commit();
    bool isDirty() const { return dirty_; }

private:
    void ensureLoaded_() const;
    bool writeFile_() const;
    static uint32_t now_ms_();

private:
    mutable uint8_t mem_[kSize];
    mutable bool loaded_;
    mutable bool dirty_;

    mutable char file_path_[64];

    mutable uint32_t autosave_interval_ms_;
    mutable uint32_t last_autosave_ms_;
};

// только объявление!
extern EEPROMClass EEPROM;

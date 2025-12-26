#include "lib/EEPROM.h"

EEPROMClass EEPROM;

EEPROMClass::EEPROMClass()
    : loaded_(false)
    , dirty_(false)
    , autosave_interval_ms_(0)
    , last_autosave_ms_(0) {
    memset(mem_, 0x00, kSize);
    file_path_[0] = '\0';
}

void EEPROMClass::begin(const char* path, uint32_t autosave_interval_ms) {
    if(path && *path) {
        strncpy(file_path_, path, sizeof(file_path_) - 1);
        file_path_[sizeof(file_path_) - 1] = '\0';
    } else if(file_path_[0] == '\0') {
        strncpy(file_path_, "/ext/eeprom.bin", sizeof(file_path_) - 1);
        file_path_[sizeof(file_path_) - 1] = '\0';
    }

    autosave_interval_ms_ = autosave_interval_ms;
    last_autosave_ms_ = now_ms_();
    ensureLoaded_();
}

uint8_t EEPROMClass::read(int addr) const {
    ensureLoaded_();
    if(addr < 0 || addr >= kSize) return 0;
    return mem_[addr];
}

void EEPROMClass::write(int addr, uint8_t value) {
    ensureLoaded_();
    if(addr < 0 || addr >= kSize) return;
    mem_[addr] = value;
    dirty_ = true;
}

void EEPROMClass::update(int addr, uint8_t value) {
    ensureLoaded_();
    if(addr < 0 || addr >= kSize) return;
    if(mem_[addr] != value) {
        mem_[addr] = value;
        dirty_ = true;
    }
}

void EEPROMClass::clear(uint8_t value) {
    ensureLoaded_();
    memset(mem_, value, kSize);
    dirty_ = true;
}

void EEPROMClass::tick() {
    if(autosave_interval_ms_ == 0) return;
    if(!dirty_) return;

    const uint32_t now = now_ms_();
    if((uint32_t)(now - last_autosave_ms_) >= autosave_interval_ms_) {
        (void)commit();
        last_autosave_ms_ = now;
    }
}

bool EEPROMClass::commit() {
    ensureLoaded_();
    if(!dirty_) return true;

    const bool ok = writeFile_();
    if(ok) dirty_ = false;
    return ok;
}

void EEPROMClass::ensureLoaded_() const {
    if(loaded_) return;

    if(file_path_[0] == '\0') {
        strncpy(file_path_, "/ext/eeprom.bin", sizeof(file_path_) - 1);
        file_path_[sizeof(file_path_) - 1] = '\0';
    }

    Storage* storage = (Storage*)furi_record_open(RECORD_STORAGE);
    if(!storage) {
        // storage ещё не доступен — не ставим loaded_
        return;
    }

    File* file = storage_file_alloc(storage);
    if(!file) {
        furi_record_close(RECORD_STORAGE);
        return;
    }

    if(storage_file_exists(storage, file_path_)) {
        bool ok = storage_file_open(file, file_path_, FSAM_READ, FSOM_OPEN_EXISTING);
        if(ok) {
            memset(mem_, 0x00, kSize);
            (void)storage_file_read(file, mem_, kSize);
        }
        (void)storage_file_close(file);
    } else {
        memset(mem_, 0x00, kSize);
        bool ok = storage_file_open(file, file_path_, FSAM_WRITE, FSOM_CREATE_ALWAYS);
        if(ok) {
            (void)storage_file_write(file, mem_, kSize);
            (void)storage_file_sync(file);
        }
        (void)storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    loaded_ = true;
    dirty_ = false;
}

bool EEPROMClass::writeFile_() const {
    Storage* storage = (Storage*)furi_record_open(RECORD_STORAGE);
    if(!storage) return false;

    File* file = storage_file_alloc(storage);
    if(!file) {
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    bool ok = storage_file_open(file, file_path_, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    bool success = false;

    if(ok) {
        size_t wr = storage_file_write(file, mem_, kSize);
        (void)storage_file_sync(file);
        (void)storage_file_close(file);
        success = (wr == (size_t)kSize);
    } else {
        (void)storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return success;
}

uint32_t EEPROMClass::now_ms_() {
    const uint32_t tick = furi_get_tick();
    const uint32_t hz = furi_kernel_get_tick_frequency();
    if(hz == 0) return tick;
    return (tick * 1000u) / hz;
}

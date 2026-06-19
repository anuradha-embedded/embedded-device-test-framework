// Mock hardware interfaces for host-side embedded testing.
//
// These are header-only simulations of common peripherals. They let a driver
// under test run unmodified against an in-memory model of the hardware, while
// the test scripts the expected bus traffic and later verifies it.
//
//   MockRegisterBank : memory-mapped register file with read/write hooks.
//   MockI2CBus       : multi-device I2C bus with scripted register maps and a
//                      recorded transaction log plus an expectation queue.
//   MockSerialPort   : byte-oriented UART with an injectable RX buffer and a
//                      captured TX buffer.
#ifndef EDTF_MOCK_HARDWARE_HPP
#define EDTF_MOCK_HARDWARE_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace edtf {
namespace mock {

// Raised by mocks when a driver does something the model considers illegal
// (unknown device address, out-of-range register, unmet expectation, ...).
struct HardwareError : std::runtime_error {
    explicit HardwareError(const std::string& what) : std::runtime_error(what) {}
};

// ---------------------------------------------------------------------------
// MockRegisterBank: a flat, word-addressable register file. Useful for
// modeling a memory-mapped peripheral. Optional per-register hooks allow
// simulating read-clears-on-read status bits, write-one-to-clear, etc.
// ---------------------------------------------------------------------------
class MockRegisterBank {
public:
    using ReadHook = std::function<uint32_t(uint32_t addr, uint32_t stored)>;
    using WriteHook = std::function<uint32_t(uint32_t addr, uint32_t current,
                                             uint32_t incoming)>;

    explicit MockRegisterBank(uint32_t size_words) : regs_(size_words, 0) {}

    uint32_t read(uint32_t addr) {
        check_addr(addr);
        uint32_t stored = regs_[addr];
        uint32_t value = read_hook_ ? read_hook_(addr, stored) : stored;
        ++reads_;
        return value;
    }

    void write(uint32_t addr, uint32_t value) {
        check_addr(addr);
        uint32_t current = regs_[addr];
        regs_[addr] = write_hook_ ? write_hook_(addr, current, value) : value;
        ++writes_;
    }

    // Direct backdoor access for test setup, bypassing hooks and counters.
    uint32_t peek(uint32_t addr) const {
        check_addr(addr);
        return regs_[addr];
    }
    void poke(uint32_t addr, uint32_t value) {
        check_addr(addr);
        regs_[addr] = value;
    }

    void set_read_hook(ReadHook h) { read_hook_ = std::move(h); }
    void set_write_hook(WriteHook h) { write_hook_ = std::move(h); }

    std::size_t size() const { return regs_.size(); }
    std::size_t read_count() const { return reads_; }
    std::size_t write_count() const { return writes_; }

private:
    void check_addr(uint32_t addr) const {
        if (addr >= regs_.size()) {
            throw HardwareError("register address out of range: " +
                                std::to_string(addr));
        }
    }

    std::vector<uint32_t> regs_;
    ReadHook read_hook_;
    WriteHook write_hook_;
    std::size_t reads_ = 0;
    std::size_t writes_ = 0;
};

// ---------------------------------------------------------------------------
// MockI2CBus: a bus carrying one or more 7-bit-addressed devices. Each device
// owns an 8-bit-indexed register map. Every read/write is appended to a
// transaction log, and an ordered expectation queue can be used to assert
// that the driver issued exactly the traffic the test author intended.
// ---------------------------------------------------------------------------
class MockI2CBus {
public:
    enum class Op { Read, Write };

    struct Transaction {
        Op op;
        uint8_t addr;  // 7-bit device address
        uint8_t reg;   // register / command code
        uint8_t value; // byte read or written
    };

    // Register a device and seed its register map.
    void attach_device(uint8_t addr,
                       std::map<uint8_t, uint8_t> initial = {}) {
        devices_[addr] = std::move(initial);
    }

    bool has_device(uint8_t addr) const {
        return devices_.find(addr) != devices_.end();
    }

    // Backdoor: set a device register without logging a transaction. Used by
    // tests to model sensor state.
    void set_register(uint8_t addr, uint8_t reg, uint8_t value) {
        device(addr)[reg] = value;
    }
    uint8_t get_register(uint8_t addr, uint8_t reg) {
        auto& m = device(addr);
        return m.count(reg) ? m[reg] : 0;
    }

    // Driver-facing API. read_register pops the matching expectation (if any
    // expectations were queued) and logs the transaction.
    uint8_t read_register(uint8_t addr, uint8_t reg) {
        auto& m = device(addr);
        uint8_t value = m.count(reg) ? m[reg] : 0;
        match_expectation(Op::Read, addr, reg, value);
        log_.push_back({Op::Read, addr, reg, value});
        return value;
    }

    void write_register(uint8_t addr, uint8_t reg, uint8_t value) {
        auto& m = device(addr);
        match_expectation(Op::Write, addr, reg, value);
        m[reg] = value;
        log_.push_back({Op::Write, addr, reg, value});
    }

    // Expectation API. Queue the transactions the test expects, in order,
    // then call verify() after exercising the driver.
    MockI2CBus& expect_read(uint8_t addr, uint8_t reg, uint8_t returns) {
        // Seed the value so the read returns what the script promises.
        device(addr)[reg] = returns;
        expectations_.push_back({Op::Read, addr, reg, returns});
        return *this;
    }
    MockI2CBus& expect_write(uint8_t addr, uint8_t reg, uint8_t value) {
        expectations_.push_back({Op::Write, addr, reg, value});
        return *this;
    }

    // Returns empty string when all expectations were met in order and fully
    // consumed; otherwise a human-readable description of the mismatch.
    std::string verify() const {
        if (!expectations_.empty() && expect_cursor_ < expectations_.size()) {
            return std::to_string(expectations_.size() - expect_cursor_) +
                   " expected transaction(s) never occurred (next expected: " +
                   describe(expectations_[expect_cursor_]) + ")";
        }
        return std::string();
    }

    const std::vector<Transaction>& log() const { return log_; }
    void clear_log() { log_.clear(); }

    static std::string describe(const Transaction& t) {
        std::string s = (t.op == Op::Read ? "R" : "W");
        return s + " dev=0x" + hex2(t.addr) + " reg=0x" + hex2(t.reg) +
               " val=0x" + hex2(t.value);
    }

private:
    std::map<uint8_t, uint8_t>& device(uint8_t addr) {
        auto it = devices_.find(addr);
        if (it == devices_.end()) {
            throw HardwareError("no I2C device at address 0x" + hex2(addr));
        }
        return it->second;
    }

    void match_expectation(Op op, uint8_t addr, uint8_t reg, uint8_t value) {
        if (expectations_.empty()) {
            return;  // no script installed: free-running mode
        }
        if (expect_cursor_ >= expectations_.size()) {
            throw HardwareError("unexpected I2C transaction (script exhausted): " +
                                describe({op, addr, reg, value}));
        }
        const Transaction& e = expectations_[expect_cursor_];
        if (e.op != op || e.addr != addr || e.reg != reg ||
            (op == Op::Write && e.value != value)) {
            throw HardwareError("I2C expectation mismatch:\n    expected " +
                                describe(e) + "\n    actual   " +
                                describe({op, addr, reg, value}));
        }
        ++expect_cursor_;
    }

    static std::string hex2(uint8_t b) {
        const char* d = "0123456789abcdef";
        std::string s;
        s += d[(b >> 4) & 0xF];
        s += d[b & 0xF];
        return s;
    }

    std::map<uint8_t, std::map<uint8_t, uint8_t>> devices_;
    std::vector<Transaction> log_;
    std::vector<Transaction> expectations_;
    std::size_t expect_cursor_ = 0;
};

// ---------------------------------------------------------------------------
// MockSerialPort: a simple byte stream. Tests inject bytes the device would
// send (inject_rx) and inspect bytes the driver transmitted (tx_data).
// ---------------------------------------------------------------------------
class MockSerialPort {
public:
    // Bytes the driver writes are captured here.
    void write(const std::vector<uint8_t>& bytes) {
        tx_.insert(tx_.end(), bytes.begin(), bytes.end());
    }
    void write_byte(uint8_t b) { tx_.push_back(b); }

    // Driver reads consume from the injected RX buffer. Returns the number of
    // bytes actually read (may be fewer than requested when RX is drained).
    std::size_t read(std::vector<uint8_t>& out, std::size_t max) {
        std::size_t n = 0;
        while (n < max && rx_pos_ < rx_.size()) {
            out.push_back(rx_[rx_pos_++]);
            ++n;
        }
        return n;
    }

    int read_byte() {
        if (rx_pos_ >= rx_.size()) {
            return -1;  // RX empty
        }
        return rx_[rx_pos_++];
    }

    // Test-side helpers.
    void inject_rx(const std::vector<uint8_t>& bytes) {
        rx_.insert(rx_.end(), bytes.begin(), bytes.end());
    }
    void inject_rx_string(const std::string& s) {
        rx_.insert(rx_.end(), s.begin(), s.end());
    }

    const std::vector<uint8_t>& tx_data() const { return tx_; }
    std::string tx_string() const { return std::string(tx_.begin(), tx_.end()); }
    std::size_t rx_remaining() const { return rx_.size() - rx_pos_; }
    void clear() {
        tx_.clear();
        rx_.clear();
        rx_pos_ = 0;
    }

private:
    std::vector<uint8_t> tx_;
    std::vector<uint8_t> rx_;
    std::size_t rx_pos_ = 0;
};

}  // namespace mock
}  // namespace edtf

#endif  // EDTF_MOCK_HARDWARE_HPP

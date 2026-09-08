#include "forensic/audit_logger.h"
#include <iomanip>
#include <sstream>
#include <chrono>
#include <cstring>
#include <iostream>
#include <fstream>

#ifdef _MSC_VER
#pragma warning(disable: 4996) // disable deprecation warnings for gmtime
#endif

namespace {
const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTRIGHT(word, bits) (((word) >> (bits)) | ((word) << (32 - (bits))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x, 2) ^ ROTRIGHT(x, 13) ^ ROTRIGHT(x, 22))
#define EP1(x) (ROTRIGHT(x, 6) ^ ROTRIGHT(x, 11) ^ ROTRIGHT(x, 25))
#define SIG0(x) (ROTRIGHT(x, 7) ^ ROTRIGHT(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x, 17) ^ ROTRIGHT(x, 19) ^ ((x) >> 10))

struct SHA256_CTX {
    uint8_t data[64];
    uint32_t datalen;
    unsigned long long bitlen;
    uint32_t state[8];
};

void sha256_transform(SHA256_CTX *ctx, const uint8_t data[]) {
    uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];

    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
    for ( ; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + sha256_k[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void sha256_init(SHA256_CTX *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

void sha256_update(SHA256_CTX *ctx, const uint8_t data[], size_t len) {
    uint32_t i;
    for (i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

void sha256_final(SHA256_CTX *ctx, uint8_t hash[]) {
    uint32_t i;
    i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56)
            ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64)
            ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += ctx->datalen * 8;
    ctx->data[63] = ctx->bitlen;
    ctx->data[62] = ctx->bitlen >> 8;
    ctx->data[61] = ctx->bitlen >> 16;
    ctx->data[60] = ctx->bitlen >> 24;
    ctx->data[59] = ctx->bitlen >> 32;
    ctx->data[58] = ctx->bitlen >> 40;
    ctx->data[57] = ctx->bitlen >> 48;
    ctx->data[56] = ctx->bitlen >> 56;
    sha256_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0x000000ff;
    }
}

std::string bytes_to_hex(const uint8_t* hash) {
    std::stringstream ss;
    for(int i = 0; i < 32; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

std::string current_time_iso() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

} // namespace

namespace forensic {

namespace {

// CA-028 crash-evidence categories: a process killed mid-operation must not
// take its own evidence (recovery/wipe/BitLocker state changes) down with it,
// so these event tokens bypass the async queue and are written synchronously.
bool IsCriticalEventToken(const std::string& token) {
    return token.rfind("RECOVER", 0) == 0 || token.rfind("WIPE", 0) == 0 ||
           token.rfind("BITLOCKER", 0) == 0 || token.rfind("FILE_RECOVERED", 0) == 0;
}

// The event token is the first segment of the message, terminated by a space
// or the first pipe ("SCAN_END | scanId=3" -> "SCAN_END").
std::string EventTokenOf(const std::string& message) {
    const size_t stop = message.find_first_of(" |");
    return stop == std::string::npos ? message : message.substr(0, stop);
}

// CA-028 bridge-origin events: first token must be UPPERCASE/underscore/digit
// (machine marker, e.g. "UI_EXPORT_START"); the payload must be printable
// ASCII; overall length is capped. Returns false on any violation.
bool IsValidBridgeEvent(const std::string& event) {
    if (event.empty() || event.size() > 512) return false;
    size_t i = 0;
    for (; i < event.size() && event[i] != ' ' && event[i] != '|'; ++i) {
        const char c = event[i];
        const bool okToken = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!okToken) return false;
    }
    if (i == 0) return false; // empty token ("| spoof", " payload") — never enters the chain
    for (; i < event.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(event[i]);
        if (c < 0x20 || c > 0x7E) return false; // control / non-ASCII rejected
    }
    return true;
}

} // namespace

AuditLogger::AuditLogger() {
    previousHash_ = std::string(64, '0');
    workerThread_ = std::thread(&AuditLogger::ProcessQueue, this);
}

AuditLogger::~AuditLogger() {
    Shutdown();
}

void AuditLogger::Initialize(const std::string& logFilePath) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (logFile_.is_open()) {
        logFile_.close();
    }
    logFile_.open(logFilePath, std::ios::app);
    if (!logFile_) {
        std::cerr << "Failed to open audit log file: " << logFilePath << std::endl;
    }
}

void AuditLogger::Shutdown() {
    // CA-028: drain whatever is queued before tearing the worker down — the
    // explicit teardown-side flush of the bounded queue.
    FlushPending();
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (stopThread_) return; // Already shut down
        stopThread_ = true;
    }
    cv_.notify_one();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    if (logFile_.is_open()) {
        logFile_.close();
    }
}

std::string AuditLogger::CalculateSHA256(const uint8_t* data, size_t size) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, size);
    uint8_t hash[32];
    sha256_final(&ctx, hash);
    return bytes_to_hex(hash);
}

void AuditLogger::EnqueueLog(const std::string& message, bool critical) {
    // CA-028: critical (crash-evidence) entries never touch the queue — they
    // are folded into the hash chain and flushed to disk on this thread, so a
    // hard crash cannot drop them. Queue ordering yields to durability here.
    if (critical) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        WriteEntryLocked(message);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        logQueue_.push({message});
    }
    cv_.notify_one();
}

void AuditLogger::FlushPending() {
    std::lock_guard<std::mutex> lock(queueMutex_);
    while (!logQueue_.empty()) {
        const LogEntry entry = std::move(logQueue_.front());
        logQueue_.pop();
        WriteEntryLocked(entry.message);
    }
}

bool AuditLogger::ConsumeBridgeRateTokenLocked() {
    const auto now = std::chrono::steady_clock::now();
    if (bridgeLastRefill_.time_since_epoch().count() == 0) {
        bridgeLastRefill_ = now;
    }
    const double elapsedSec = std::chrono::duration<double>(now - bridgeLastRefill_).count();
    if (elapsedSec > 0) {
        bridgeTokens_ = std::min(kBridgeEventBurst,
                                 bridgeTokens_ + elapsedSec * kBridgeEventsPerSecond);
        bridgeLastRefill_ = now;
    }
    if (bridgeTokens_ < 1.0) return false;
    bridgeTokens_ -= 1.0;
    return true;
}

void AuditLogger::WriteEntryLocked(const std::string& message) {
    // queueMutex_ held: serializes the hash chain between the worker thread
    // and synchronous (critical / FlushPending) writers.
    const std::string to_hash = previousHash_ + message;
    const std::string new_hash = CalculateSHA256(
        reinterpret_cast<const uint8_t*>(to_hash.c_str()), to_hash.size());

    if (logFile_.is_open()) {
        logFile_ << message << " | ChainHash: " << new_hash << "\n";
        logFile_.flush();
    }

    previousHash_ = new_hash;
}

void AuditLogger::LogDiskRead(const std::string& devicePath, uint64_t offset, uint64_t size) {
    std::stringstream ss;
    ss << "[" << current_time_iso() << "] DISK_READ | Device: " << devicePath
       << " | Offset: " << offset << " | Size: " << size;
    EnqueueLog(ss.str());
}

void AuditLogger::LogEvent(const std::string& eventMessage) {
    std::string sanitized = eventMessage;
    for (char& c : sanitized) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    std::stringstream ss;
    ss << "[" << current_time_iso() << "] EVENT | " << sanitized;
    EnqueueLog(ss.str(), IsCriticalEventToken(EventTokenOf(sanitized)));
}

bool AuditLogger::LogEventFromBridge(const std::string& event) {
    if (!IsValidBridgeEvent(event)) return false;
    // CA-028 hardening: flood cap. The renderer is an untrusted audit source
    // and critical categories (WIPE_*/RECOVER_* tokens) write + flush
    // synchronously per call — an uncapped loop would grow the audit file
    // without bound and thrash the disk from the main process. Excess events
    // are REJECTED (false); the UI layer surfaces the rejection, session.log
    // remains the unlimited side channel.
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!ConsumeBridgeRateTokenLocked()) return false;
    }
    // Existing line convention is "[ts] CATEGORY | payload..."; the JS origin
    // rides as the first payload segment so the chain distinguishes
    // renderer/main-process events from native ones. Spoof-proof by
    // construction: the bridge ALWAYS writes "EVENT | JS | " ahead of the
    // renderer-supplied text, so a bridge event can never mimic a native
    // "[ts] EVENT | TOKEN" line.
    std::stringstream ss;
    ss << "[" << current_time_iso() << "] EVENT | JS | " << event;
    EnqueueLog(ss.str(), IsCriticalEventToken(EventTokenOf(event)));
    return true;
}

void AuditLogger::LogFileRecovered(const std::string& filePath, const uint8_t* data, size_t size) {
    std::string hash = CalculateSHA256(data, size);
    std::stringstream ss;
    ss << "[" << current_time_iso() << "] FILE_RECOVERED | Path: " << filePath
       << " | Size: " << size << " | SHA256: " << hash;
    EnqueueLog(ss.str(), true); // recovery evidence — crash must not drop it
}

void AuditLogger::ProcessQueue() {
    while (true) {
        LogEntry event;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            cv_.wait(lock, [this]() { return stopThread_ || !logQueue_.empty(); });

            if (stopThread_ && logQueue_.empty()) {
                break;
            }

            event = std::move(logQueue_.front());
            logQueue_.pop();
            // CA-028: hash + write under the queue mutex so concurrent
            // synchronous writers cannot fork the chain.
            WriteEntryLocked(event.message);
        }
    }
}

AuditChainVerifyResult VerifyAuditChainFile(const std::string& path) {
    AuditChainVerifyResult res;
    std::ifstream in(path);
    if (!in.is_open()) {
        res.detail = "open failed";
        return res;
    }
    std::string prev(64, '0');
    std::string line;
    int lineNo = 0;
    const std::string marker = " | ChainHash: ";
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const size_t pos = line.rfind(marker);
        bool lineOk = false;
        std::string stored;
        if (pos != std::string::npos) {
            const std::string message = line.substr(0, pos);
            stored = line.substr(pos + marker.size());
            const std::string toHash = prev + message;
            const std::string expected = AuditLogger::CalculateSHA256(
                reinterpret_cast<const uint8_t*>(toHash.c_str()), toHash.size());
            lineOk = (expected == stored);
        }
        if (lineOk) {
            prev = stored;
            res.entries = lineNo;
            continue;
        }
        // Torn write vs tampering on the FINAL line: an append+flush writer
        // can leave half a message or a truncated hash, but it can NEVER
        // fabricate a complete 64-hex digest that fails verification. A
        // well-formed hash that does not verify is therefore tampering of the
        // last entry and must be reported broken — not excused as a torn tail.
        const bool malformedHash =
            pos == std::string::npos || stored.size() != 64 ||
            stored.find_first_not_of("0123456789abcdef") != std::string::npos;
        std::string next;
        if (!std::getline(in, next)) {
            if (!malformedHash) {
                res.entries = lineNo;
                res.brokenAt = lineNo;
                res.detail = "hash mismatch";
                return res;
            }
            res.ok = res.entries > 0;
            res.detail = res.ok ? "incomplete tail (writer active?)" : "incomplete tail";
            return res;
        }
        res.entries = lineNo;
        res.brokenAt = lineNo;
        res.detail = (pos == std::string::npos) ? "missing ChainHash" : "hash mismatch";
        return res;
    }
    res.ok = res.entries > 0;
    res.detail = res.ok ? "ok" : "empty";
    return res;
}

} // namespace forensic

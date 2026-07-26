#include "disk-cache.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "common.h"
#include "server-common.h"

namespace {

constexpr char INDEX_FILE[] = "index.json";
constexpr char KVCACHE_MAGIC[8] = {'K', 'V', 'C', 'A', 'C', 'H', 'E', '\0'};
constexpr char CKPT_MAGIC[8] = {'C', 'K', 'P', 'T', '\0', '\0', '\0', '\0'};
constexpr uint32_t KVCACHE_VERSION = 1;
constexpr uint32_t CKPT_VERSION = 1;
constexpr size_t PLAINTEXT_MAX_LEN = 500;
constexpr size_t PREVIEW_MAX_LEN = 50;

static inline uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

#define SHA256_S0(x) (rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22))
#define SHA256_S1(x) (rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25))
#define SHA256_s0(x) (rotr32(x, 7) ^ rotr32(x, 18) ^ ((x) >> 3))
#define SHA256_s1(x) (rotr32(x, 17) ^ rotr32(x, 19) ^ ((x) >> 10))
#define SHA256_CH(x, y, z) (z ^ (x & (y ^ z)))
#define SHA256_MAJ(x, y, z) ((x & y) | (z & (x | y)))

struct sha256_ctx {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
};

static const uint32_t sha256_k[64] = {
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
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

void sha256_init(sha256_ctx * ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

void sha256_process_block(sha256_ctx * ctx) {
    uint32_t w[64];

    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t) ctx->buffer[i * 4 + 0] << 24)
             | ((uint32_t) ctx->buffer[i * 4 + 1] << 16)
             | ((uint32_t) ctx->buffer[i * 4 + 2] <<  8)
             | ((uint32_t) ctx->buffer[i * 4 + 3] <<  0);
    }

    for (int i = 16; i < 64; ++i) {
        w[i] = SHA256_s1(w[i - 2]) + w[i - 7] + SHA256_s0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + SHA256_S1(e) + SHA256_CH(e, f, g) + sha256_k[i] + w[i];
        const uint32_t t2 = SHA256_S0(a) + SHA256_MAJ(a, b, c);
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

void sha256_update(sha256_ctx * ctx, const uint8_t * data, size_t len) {
    uint32_t pos = (uint32_t) ctx->count & 0x3F;

    while (len > 0) {
        ctx->buffer[pos++] = *data++;
        ctx->count++;
        len--;

        if (pos == 64) {
            pos = 0;
            sha256_process_block(ctx);
        }
    }
}

void sha256_final(sha256_ctx * ctx, uint8_t * digest) {
    uint64_t len_bits = ctx->count << 3;
    uint32_t pos = (uint32_t) ctx->count & 0x3F;

    ctx->buffer[pos++] = 0x80;

    while (pos != 56) {
        pos &= 0x3F;
        if (pos == 0) {
            sha256_process_block(ctx);
        }
        ctx->buffer[pos++] = 0;
    }

    for (int i = 0; i < 8; ++i) {
        ctx->buffer[pos++] = (uint8_t) (len_bits >> 56);
        len_bits <<= 8;
    }

    sha256_process_block(ctx);

    for (int i = 0; i < 8; ++i) {
        digest[i * 4 + 0] = (uint8_t) (ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t) (ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t) (ctx->state[i] >>  8);
        digest[i * 4 + 3] = (uint8_t) (ctx->state[i] >>  0);
    }
}

#undef SHA256_S0
#undef SHA256_S1
#undef SHA256_s0
#undef SHA256_s1
#undef SHA256_CH
#undef SHA256_MAJ

template <typename T>
bool read_value(std::istream & in, T & value) {
    in.read(reinterpret_cast<char *>(&value), sizeof(value));
    return in.good();
}

bool read_buffer(std::istream & in, void * data, size_t size) {
    if (size == 0) {
        return true;
    }

    in.read(reinterpret_cast<char *>(data), (std::streamsize) size);
    return in.good();
}

template <typename T>
bool write_value(std::ostream & out, const T & value) {
    out.write(reinterpret_cast<const char *>(&value), sizeof(value));
    return out.good();
}

bool write_buffer(std::ostream & out, const void * data, size_t size) {
    if (size == 0) {
        return true;
    }

    out.write(reinterpret_cast<const char *>(data), (std::streamsize) size);
    return out.good();
}

std::filesystem::path index_path(const std::string & slot_save_path) {
    return std::filesystem::path(slot_save_path) / INDEX_FILE;
}

std::string hash_text(const std::string & text) {
    uint8_t hash[32];
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, reinterpret_cast<const uint8_t *>(text.data()), text.size());
    sha256_final(&ctx, hash);

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < 32; ++i) {
        out << std::setw(2) << (int) hash[i];
    }

    return out.str();
}

std::string make_preview(const std::string & text, size_t max_len) {
    if (text.size() <= max_len) {
        return text;
    }

    return text.substr(0, max_len);
}

void log_user_messages(const char * prefix, const std::vector<std::string> & user_msg_texts) {
    SRV_INF("[auto-cache] %s: %zu user messages\n", prefix, user_msg_texts.size());

    for (size_t i = 0; i < user_msg_texts.size(); ++i) {
        SRV_INF("[auto-cache] %s msg[%zu] len=%zu text=%s\n",
                prefix, i, user_msg_texts[i].size(), user_msg_texts[i].c_str());
    }
}

std::string describe_index_entry(const json & entry) {
    if (entry.contains("text")) {
        return string_format("text=%s", entry["text"].get<std::string>().c_str());
    }

    if (entry.contains("hash")) {
        const std::string preview = entry.value("preview", "");
        return string_format("hash=%s preview=%s",
                entry["hash"].get<std::string>().c_str(), preview.c_str());
    }

    return "(unknown)";
}

json load_index(const std::string & slot_save_path) {
    const std::filesystem::path path = index_path(slot_save_path);
    std::ifstream in(path);

    if (!in.is_open()) {
        return json::object();
    }

    try {
        json idx = json::parse(in);
        if (idx.is_object()) {
            return idx;
        }
    } catch (...) {
        SRV_WRN("[auto-cache] failed to parse %s\n", path.string().c_str());
    }

    return json::object();
}

void save_index(const std::string & slot_save_path, const json & idx) {
    const std::filesystem::path path = index_path(slot_save_path);
    std::ofstream out(path, std::ios::trunc);

    if (!out.is_open()) {
        SRV_WRN("[auto-cache] failed to write %s\n", path.string().c_str());
        return;
    }

    out << idx.dump(2);
}

json make_index_entry(const std::string & text) {
    if (text.size() <= PLAINTEXT_MAX_LEN) {
        return json{
            {"text", text},
        };
    }

    return json{
        {"hash",    hash_text(text)},
        {"preview", make_preview(text, PREVIEW_MAX_LEN)},
    };
}

bool match_index_entry(const json & entry, const std::string & query_text) {
    if (entry.contains("text")) {
        return query_text == entry["text"].get<std::string>();
    }

    if (entry.contains("hash")) {
        return hash_text(query_text) == entry["hash"].get<std::string>();
    }

    return false;
}

void remove_file_pair(const std::filesystem::path & filepath) {
    std::error_code ec;
    std::filesystem::remove(filepath, ec);
    std::filesystem::remove(filepath.string() + ".ckpt", ec);
}

bool load_checkpoints(const std::filesystem::path & filepath, server_prompt & prompt) {
    prompt.checkpoints.clear();

    const std::filesystem::path ckpt_path = filepath.string() + ".ckpt";
    std::ifstream in(ckpt_path, std::ios::binary);

    if (!in.is_open()) {
        return true;
    }

    char magic[8];
    uint32_t version = 0;
    uint32_t count = 0;

    if (!read_buffer(in, magic, sizeof(magic)) ||
        !read_value(in, version) ||
        std::memcmp(magic, CKPT_MAGIC, sizeof(magic)) != 0 ||
        version != CKPT_VERSION ||
        !read_value(in, count)) {
        SRV_WRN("[auto-cache] failed to read checkpoints from %s\n", ckpt_path.string().c_str());
        prompt.checkpoints.clear();
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        common_prompt_checkpoint checkpoint;
        uint64_t size_tgt = 0;
        uint64_t size_dft = 0;

        if (!read_value(in, checkpoint.n_tokens) ||
            !read_value(in, checkpoint.pos_min) ||
            !read_value(in, checkpoint.pos_max) ||
            !read_value(in, size_tgt)) {
            SRV_WRN("[auto-cache] failed to read checkpoints from %s\n", ckpt_path.string().c_str());
            prompt.checkpoints.clear();
            return false;
        }

        checkpoint.data_tgt.resize(size_tgt);
        if (!read_buffer(in, checkpoint.data_tgt.data(), checkpoint.data_tgt.size()) ||
            !read_value(in, size_dft)) {
            SRV_WRN("[auto-cache] failed to read checkpoints from %s\n", ckpt_path.string().c_str());
            prompt.checkpoints.clear();
            return false;
        }

        checkpoint.data_dft.resize(size_dft);
        if (!read_buffer(in, checkpoint.data_dft.data(), checkpoint.data_dft.size())) {
            SRV_WRN("[auto-cache] failed to read checkpoints from %s\n", ckpt_path.string().c_str());
            prompt.checkpoints.clear();
            return false;
        }

        prompt.checkpoints.push_back(std::move(checkpoint));
    }

    return true;
}

bool save_checkpoints(const std::filesystem::path & filepath, const server_prompt & prompt) {
    if (prompt.checkpoints.empty()) {
        return true;
    }

    const std::filesystem::path ckpt_path = filepath.string() + ".ckpt";
    std::ofstream out(ckpt_path, std::ios::binary);

    if (!out.is_open()) {
        SRV_WRN("[auto-cache] failed to write %s\n", ckpt_path.string().c_str());
        return false;
    }

    const uint32_t count = (uint32_t) prompt.checkpoints.size();

    if (!write_buffer(out, CKPT_MAGIC, sizeof(CKPT_MAGIC)) ||
        !write_value(out, CKPT_VERSION) ||
        !write_value(out, count)) {
        SRV_WRN("[auto-cache] failed to write %s\n", ckpt_path.string().c_str());
        return false;
    }

    for (const auto & checkpoint : prompt.checkpoints) {
        const uint64_t size_tgt = checkpoint.data_tgt.size();
        const uint64_t size_dft = checkpoint.data_dft.size();

        if (!write_value(out, checkpoint.n_tokens) ||
            !write_value(out, checkpoint.pos_min) ||
            !write_value(out, checkpoint.pos_max) ||
            !write_value(out, size_tgt) ||
            !write_buffer(out, checkpoint.data_tgt.data(), checkpoint.data_tgt.size()) ||
            !write_value(out, size_dft) ||
            !write_buffer(out, checkpoint.data_dft.data(), checkpoint.data_dft.size())) {
            SRV_WRN("[auto-cache] failed to write %s\n", ckpt_path.string().c_str());
            return false;
        }
    }

    return true;
}

void evict_old_entries(const std::string & slot_save_path, int max_pairs) {
    if (max_pairs <= 0) {
        return;
    }

    std::vector<std::filesystem::path> files;
    for (const auto & entry : std::filesystem::directory_iterator(slot_save_path)) {
        if (entry.is_regular_file() && entry.path().extension() == ".kvcache") {
            files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end());

    if ((int) files.size() <= max_pairs) {
        return;
    }

    json idx = load_index(slot_save_path);

    while ((int) files.size() > max_pairs) {
        const std::filesystem::path old_path = files.front();
        const std::string old_name = old_path.filename().string();

        remove_file_pair(old_path);
        idx.erase(old_name);
        files.erase(files.begin());

        SRV_INF("[auto-cache] evicted %s (max %d pairs)\n", old_name.c_str(), max_pairs);
    }

    save_index(slot_save_path, idx);
}

} // namespace

std::string disk_cache_find(
    const std::string & slot_save_path,
    const std::vector<std::string> & user_msg_texts) {

    if (user_msg_texts.empty()) {
        SRV_INF("%s", "[auto-cache] skip lookup: no user messages\n");
        return "";
    }

    if (!std::filesystem::is_directory(slot_save_path)) {
        SRV_INF("[auto-cache] skip lookup: slot save path is not a directory: %s\n", slot_save_path.c_str());
        return "";
    }

    SRV_INF("[auto-cache] lookup start: path=%s\n", slot_save_path.c_str());
    log_user_messages("lookup", user_msg_texts);

    const json idx = load_index(slot_save_path);
    std::string best_filename;

    for (const auto & [filename, msg_list] : idx.items()) {
        if (!msg_list.is_array() || msg_list.size() != user_msg_texts.size()) {
            SRV_INF("[auto-cache] skip candidate %s: message count mismatch (%zu != %zu)\n",
                    filename.c_str(), msg_list.size(), user_msg_texts.size());
            continue;
        }

        const std::filesystem::path filepath = std::filesystem::path(slot_save_path) / filename;
        if (!std::filesystem::is_regular_file(filepath)) {
            SRV_INF("[auto-cache] skip candidate %s: file not found on disk\n", filename.c_str());
            continue;
        }

        SRV_INF("[auto-cache] checking candidate %s\n", filename.c_str());

        bool matched = true;
        for (size_t i = 0; i < user_msg_texts.size(); ++i) {
            if (!match_index_entry(msg_list[i], user_msg_texts[i])) {
                SRV_INF("[auto-cache] candidate %s mismatch at msg[%zu]: query=%s stored=%s\n",
                        filename.c_str(), i, user_msg_texts[i].c_str(), describe_index_entry(msg_list[i]).c_str());
                matched = false;
                break;
            }

            SRV_INF("[auto-cache] candidate %s matched msg[%zu]: %s\n",
                    filename.c_str(), i, user_msg_texts[i].c_str());
        }

        if (matched) {
            best_filename = filename;
        }
    }

    if (best_filename.empty()) {
        SRV_INF("[auto-cache] no exact match found under %s\n", slot_save_path.c_str());
        return "";
    }

    SRV_INF("[auto-cache] found exact match (%zu user messages) in %s\n",
            user_msg_texts.size(), best_filename.c_str());

    return (std::filesystem::path(slot_save_path) / best_filename).string();
}

bool disk_cache_load(
    const std::string & filepath,
    server_prompt & prompt,
    int slot_id,
    llama_context * ctx_tgt,
    llama_context * ctx_dft) {

    SRV_INF("[auto-cache] load start: file=%s slot=%d\n", filepath.c_str(), slot_id);

    std::ifstream in(filepath, std::ios::binary);
    if (!in.is_open()) {
        SRV_WRN("[auto-cache] failed to open %s for reading\n", filepath.c_str());
        return false;
    }

    char magic[8];
    uint32_t version = 0;
    uint32_t n_tokens = 0;

    if (!read_buffer(in, magic, sizeof(magic)) ||
        !read_value(in, version) ||
        !read_value(in, n_tokens) ||
        std::memcmp(magic, KVCACHE_MAGIC, sizeof(magic)) != 0 ||
        version != KVCACHE_VERSION) {
        SRV_WRN("[auto-cache] failed to read %s\n", filepath.c_str());
        return false;
    }

    llama_tokens tokens(n_tokens);
    if (!read_buffer(in, tokens.data(), tokens.size() * sizeof(llama_token))) {
        SRV_WRN("[auto-cache] failed to read %s\n", filepath.c_str());
        return false;
    }

    uint64_t size_tgt = 0;
    uint64_t size_dft = 0;

    if (!read_value(in, size_tgt)) {
        SRV_WRN("[auto-cache] failed to read %s\n", filepath.c_str());
        return false;
    }

    std::vector<uint8_t> data_tgt(size_tgt);
    if (!read_buffer(in, data_tgt.data(), data_tgt.size()) ||
        !read_value(in, size_dft)) {
        SRV_WRN("[auto-cache] failed to read %s\n", filepath.c_str());
        return false;
    }

    std::vector<uint8_t> data_dft(size_dft);
    if (!read_buffer(in, data_dft.data(), data_dft.size())) {
        SRV_WRN("[auto-cache] failed to read %s\n", filepath.c_str());
        return false;
    }

    if (llama_state_seq_set_data_ext(ctx_tgt, data_tgt.data(), data_tgt.size(), slot_id, LLAMA_STATE_SEQ_FLAGS_NONE) != data_tgt.size()) {
        SRV_ERR("[auto-cache] failed to restore state from %s\n", filepath.c_str());
        return false;
    }

    if (ctx_dft != nullptr && !data_dft.empty()) {
        if (llama_state_seq_set_data_ext(ctx_dft, data_dft.data(), data_dft.size(), slot_id, LLAMA_STATE_SEQ_FLAGS_NONE) != data_dft.size()) {
            SRV_WRN("[auto-cache] failed to restore draft state from %s\n", filepath.c_str());
        }
    }

    prompt.tokens.clear();
    prompt.data.main.clear();
    prompt.data.drft.clear();
    prompt.checkpoints.clear();

    for (const auto token : tokens) {
        prompt.tokens.push_back(token);
    }

    load_checkpoints(filepath, prompt);

    SRV_INF("[auto-cache] loaded file=%s slot=%d tokens=%u%s\n",
            filepath.c_str(), slot_id, n_tokens,
            prompt.checkpoints.empty() ? "" : string_format(" with %zu checkpoints", prompt.checkpoints.size()).c_str());

    return true;
}

std::string disk_cache_save(
    const server_prompt & prompt,
    int slot_id,
    const std::string & slot_save_path,
    llama_context * ctx_tgt,
    llama_context * ctx_dft,
    const std::vector<std::string> & user_msg_texts,
    int max_pairs) {

    if (prompt.tokens.has_mtmd) {
        SRV_INF("%s", "[auto-cache] skip save: multimodal prompt\n");
        return "";
    }

    if (user_msg_texts.empty()) {
        SRV_INF("%s", "[auto-cache] skip save: no user messages\n");
        return "";
    }

    if (!std::filesystem::is_directory(slot_save_path)) {
        SRV_INF("[auto-cache] skip save: slot save path is not a directory: %s\n", slot_save_path.c_str());
        return "";
    }

    const std::string filename = std::to_string(ggml_time_us()) + ".kvcache";
    const std::filesystem::path filepath = std::filesystem::path(slot_save_path) / filename;

    const size_t size_tgt = llama_state_seq_get_size_ext(ctx_tgt, slot_id, LLAMA_STATE_SEQ_FLAGS_NONE);
    const size_t size_dft = ctx_dft ? llama_state_seq_get_size_ext(ctx_dft, slot_id, LLAMA_STATE_SEQ_FLAGS_NONE) : 0;
    const uint32_t n_tokens = (uint32_t) prompt.n_tokens();

    SRV_INF("[auto-cache] save start: file=%s slot=%d tokens=%u main=%zu drft=%zu\n",
            filepath.string().c_str(), slot_id, n_tokens, size_tgt, size_dft);
    log_user_messages("save", user_msg_texts);

    std::ofstream out(filepath, std::ios::binary);
    if (!out.is_open()) {
        SRV_WRN("[auto-cache] failed to open %s for writing\n", filepath.string().c_str());
        return "";
    }

    if (!write_buffer(out, KVCACHE_MAGIC, sizeof(KVCACHE_MAGIC)) ||
        !write_value(out, KVCACHE_VERSION) ||
        !write_value(out, n_tokens)) {
        remove_file_pair(filepath);
        return "";
    }

    const llama_tokens & tokens = prompt.tokens.get_tokens();
    if (!write_buffer(out, tokens.data(), tokens.size() * sizeof(llama_token))) {
        remove_file_pair(filepath);
        return "";
    }

    std::vector<uint8_t> data_tgt(size_tgt);
    std::vector<uint8_t> data_dft(size_dft);

    if (llama_state_seq_get_data_ext(ctx_tgt, data_tgt.data(), data_tgt.size(), slot_id, LLAMA_STATE_SEQ_FLAGS_NONE) != data_tgt.size()) {
        SRV_WRN("[auto-cache] failed to get KV state for %s\n", filepath.string().c_str());
        remove_file_pair(filepath);
        return "";
    }

    if (ctx_dft != nullptr && !data_dft.empty()) {
        if (llama_state_seq_get_data_ext(ctx_dft, data_dft.data(), data_dft.size(), slot_id, LLAMA_STATE_SEQ_FLAGS_NONE) != data_dft.size()) {
            SRV_WRN("[auto-cache] failed to get draft KV state for %s\n", filepath.string().c_str());
            remove_file_pair(filepath);
            return "";
        }
    }

    const uint64_t size_tgt_u64 = size_tgt;
    const uint64_t size_dft_u64 = size_dft;

    if (!write_value(out, size_tgt_u64) ||
        !write_buffer(out, data_tgt.data(), data_tgt.size()) ||
        !write_value(out, size_dft_u64) ||
        !write_buffer(out, data_dft.data(), data_dft.size())) {
        SRV_WRN("[auto-cache] failed to write %s\n", filepath.string().c_str());
        remove_file_pair(filepath);
        return "";
    }

    out.close();

    save_checkpoints(filepath, prompt);

    json idx = load_index(slot_save_path);
    json entries = json::array();

    for (const auto & text : user_msg_texts) {
        entries.push_back(make_index_entry(text));
    }

    idx[filename] = std::move(entries);
    save_index(slot_save_path, idx);
    evict_old_entries(slot_save_path, max_pairs);

    SRV_INF("[auto-cache] saved file=%s slot=%d tokens=%u main=%zu drft=%zu\n",
            filepath.string().c_str(), slot_id, n_tokens, size_tgt, size_dft);

    return filepath.string();
}

void disk_cache_remove(
    const std::string & slot_save_path,
    const std::string & filepath) {

    if (filepath.empty()) {
        return;
    }

    const std::filesystem::path path(filepath);
    SRV_INF("[auto-cache] remove cache file=%s\n", path.string().c_str());
    remove_file_pair(path);

    if (!std::filesystem::is_directory(slot_save_path)) {
        return;
    }

    json idx = load_index(slot_save_path);
    idx.erase(path.filename().string());
    save_index(slot_save_path, idx);
}

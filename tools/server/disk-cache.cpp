#include "disk-cache.h"

#include <fstream>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdint>

#include "common.h"           // common_prompt_checkpoint
#include "server-common.h"    // json, SRV_INF

// SHA-256 implementation provided by server-common.h

namespace {

constexpr char INDEX_FILE[] = "index.json";
constexpr size_t PLAINTEXT_MAX_LEN = 500;

// Load index.json from slot_save_path.
// Format: { "filename.kvcache": [ { "text": "..." } | { "hash": "...", "preview": "..." } ], ... }
json load_index(const std::string & slot_save_path) {
    const std::string index_path = slot_save_path + INDEX_FILE;
    std::ifstream fin(index_path);
    if (!fin.is_open()) return json::object();
    try {
        json idx = json::parse(fin);
        if (idx.is_object()) return idx;
    } catch (...) {
        SRV_WRN("[auto-cache] failed to parse %s\n", index_path.c_str());
    }
    return json::object();
}

// Save index.json to slot_save_path.
void save_index(const std::string & slot_save_path, const json & idx) {
    const std::string index_path = slot_save_path + INDEX_FILE;
    std::ofstream fout(index_path, std::ios::trunc);
    if (fout.is_open()) fout << idx.dump(2);
}

// Get display string for a message entry from index.
static std::string msg_preview(const json & entry) {
    if (entry.contains("text")) return entry["text"].get<std::string>();
    if (entry.contains("preview")) return entry["preview"].get<std::string>() + "...";
    return "(unknown)";
}

} // namespace

std::string disk_cache_find(
    const std::string & slot_save_path,
    const std::vector<uint8_t> & user_msg_hashes,
    const std::vector<std::string> & user_msg_texts) {

    if (user_msg_hashes.empty()) return "";
    if (!std::filesystem::is_directory(slot_save_path)) return "";

    const json idx = load_index(slot_save_path);

    std::string best_filename;
    int best_match_len = 0;

    for (const auto & [filename, msg_list] : idx.items()) {
        if (!msg_list.is_array()) continue;
        if (msg_list.size() != user_msg_texts.size()) continue; // must match exactly

        int match_len = 0;
        for (size_t i = 0; i < user_msg_texts.size(); i++) {
            const json & entry = msg_list[i];
            const std::string & query_text = user_msg_texts[i];

            bool matched = false;
            if (entry.contains("text")) {
                // Plaintext comparison
                matched = (query_text == entry["text"].get<std::string>());
            } else if (entry.contains("hash")) {
                // Hash comparison
                uint8_t q_hash[32];
                sha256_ctx ctx;
                sha256_init(&ctx);
                sha256_update(&ctx, (const uint8_t*)query_text.data(), query_text.size());
                sha256_final(&ctx, q_hash);

                std::ostringstream oss;
                for (int j = 0; j < 32; j++)
                    oss << std::hex << std::setfill('0') << std::setw(2) << (int)q_hash[j];
                matched = (oss.str() == entry["hash"].get<std::string>());
            }

            if (!matched) break;
            match_len++;
        }

        if (match_len > best_match_len && match_len == (int)user_msg_texts.size()) {
            best_match_len = match_len;
            best_filename = filename;
        }
    }

    if (!best_filename.empty()) {
        SRV_INF("[auto-cache] found exact match (%d user messages) in %s\n",
                best_match_len, best_filename.c_str());
        for (size_t i = 0; i < user_msg_texts.size(); i++) {
            std::string preview = user_msg_texts[i];
            if ((int)preview.size() > 80) preview = preview.substr(0, 80) + "...";
            SRV_INF("  msg[%zu]: %s\n", i, preview.c_str());
        }
        return slot_save_path + best_filename;
    }

    return "";
}

void disk_cache_load(
    const std::string & filepath,
    server_prompt & prompt,
    int slot_id,
    llama_context * ctx_tgt,
    llama_context * ctx_dft) {

    std::ifstream in(filepath, std::ios::binary);
    if (!in.is_open()) return;

    // header
    char magic[8] = {};
    uint32_t ver = 0, n_tok = 0;
    in.read(magic, 8);
    in.read(reinterpret_cast<char*>(&ver), sizeof(ver));
    in.read(reinterpret_cast<char*>(&n_tok), sizeof(n_tok));

    // read tokens
    llama_tokens token_vec(n_tok);
    in.read(reinterpret_cast<char*>(token_vec.data()), n_tok * sizeof(llama_token));

    // read KV state
    uint64_t main_size = 0;
    in.read(reinterpret_cast<char*>(&main_size), sizeof(main_size));
    std::vector<uint8_t> main_data(main_size);
    in.read(reinterpret_cast<char*>(main_data.data()), (std::streamsize)main_size);

    uint64_t drft_size = 0;
    in.read(reinterpret_cast<char*>(&drft_size), sizeof(drft_size));
    std::vector<uint8_t> drft_data(drft_size);
    if (drft_size > 0) {
        in.read(reinterpret_cast<char*>(drft_data.data()), (std::streamsize)drft_size);
    }

    if (!in.good()) {
        SRV_WRN("[auto-cache] failed to read %s\n", filepath.c_str());
        return;
    }

    // restore KV state into context
    {
        const size_t n = llama_state_seq_set_data_ext(ctx_tgt, main_data.data(), main_size, slot_id, 0);
        if (n != main_size) {
            SRV_ERR("[auto-cache] failed to restore state from %s (%zu != %zu)\n", filepath.c_str(), n, main_size);
            return;
        }
    }
    if (drft_size > 0 && ctx_dft) {
        const size_t n = llama_state_seq_set_data_ext(ctx_dft, drft_data.data(), drft_size, slot_id, 0);
        if (n != drft_size) {
            SRV_WRN("[auto-cache] failed to restore draft state from %s\n", filepath.c_str());
        }
    }

    // set prompt tokens
    prompt.tokens.clear();
    for (auto tok : token_vec) {
        prompt.tokens.push_back(tok);
    }

    // load checkpoints companion file
    prompt.checkpoints.clear();
    const std::string ckpt_path = filepath + ".ckpt";
    std::ifstream ckpt_in(ckpt_path, std::ios::binary);
    if (ckpt_in.is_open()) {
        char ckpt_magic[8] = {};
        uint32_t ckpt_ver = 0, ckpt_count = 0;
        ckpt_in.read(ckpt_magic, 8);
        ckpt_in.read(reinterpret_cast<char*>(&ckpt_ver), sizeof(ckpt_ver));
        if (std::memcmp(ckpt_magic, "CKPT\x00\x00\x00\x00", 8) == 0 && ckpt_ver == 1) {
            ckpt_in.read(reinterpret_cast<char*>(&ckpt_count), sizeof(ckpt_count));
            for (uint32_t ci = 0; ci < ckpt_count; ci++) {
                common_prompt_checkpoint cp;
                uint64_t tgt_size = 0, dft_size = 0;
                ckpt_in.read(reinterpret_cast<char*>(&cp.n_tokens), sizeof(cp.n_tokens));
                ckpt_in.read(reinterpret_cast<char*>(&cp.pos_min),  sizeof(cp.pos_min));
                ckpt_in.read(reinterpret_cast<char*>(&cp.pos_max),  sizeof(cp.pos_max));
                ckpt_in.read(reinterpret_cast<char*>(&tgt_size), sizeof(tgt_size));
                cp.data_tgt.resize(tgt_size);
                ckpt_in.read(reinterpret_cast<char*>(cp.data_tgt.data()), (std::streamsize)tgt_size);
                ckpt_in.read(reinterpret_cast<char*>(&dft_size), sizeof(dft_size));
                if (dft_size > 0) {
                    cp.data_dft.resize(dft_size);
                    ckpt_in.read(reinterpret_cast<char*>(cp.data_dft.data()), (std::streamsize)dft_size);
                }
                prompt.checkpoints.push_back(std::move(cp));
            }
        }
    }

    SRV_INF("[auto-cache] loaded %u tokens%s\n", n_tok,
        prompt.checkpoints.empty() ? "" : (" with " + std::to_string(prompt.checkpoints.size()) + " checkpoints").c_str());
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
        SRV_INF("[auto-cache] skipping multimodal prompt%s", "");
        return "";
    }

    if (!std::filesystem::is_directory(slot_save_path)) {
        return "";
    }

    const std::string filepath = slot_save_path + std::to_string(ggml_time_us()) + ".kvcache";
    const std::string filename = std::to_string(ggml_time_us()) + ".kvcache";

    const size_t main_size = llama_state_seq_get_size_ext(ctx_tgt, slot_id, LLAMA_STATE_SEQ_FLAGS_NONE);
    const size_t drft_size = ctx_dft ? llama_state_seq_get_size_ext(ctx_dft, slot_id, LLAMA_STATE_SEQ_FLAGS_NONE) : 0;

    std::ofstream out(filepath, std::ios::binary);
    if (!out.is_open()) return "";

    const uint32_t n_tok = (uint32_t)prompt.n_tokens();

    // header
    out.write("KVCACHE\x00", 8);
    const uint32_t ver = 1;
    out.write(reinterpret_cast<const char*>(&ver), sizeof(ver));
    out.write(reinterpret_cast<const char*>(&n_tok), sizeof(n_tok));

    // tokens
    const llama_tokens & token_data = prompt.tokens.get_tokens();
    out.write(reinterpret_cast<const char*>(token_data.data()), n_tok * sizeof(llama_token));

    // KV state
    const uint64_t main_size_u64 = main_size;
    const uint64_t drft_size_u64 = drft_size;
    out.write(reinterpret_cast<const char*>(&main_size_u64), sizeof(main_size_u64));
    if (main_size > 0) {
        std::vector<uint8_t> state_data(main_size);
        if (llama_state_seq_get_data_ext(ctx_tgt, state_data.data(), main_size, slot_id, LLAMA_STATE_SEQ_FLAGS_NONE) != main_size) {
            SRV_WRN("[auto-cache] failed to get KV state for %s\n", filepath.c_str());
            out.close();
            std::filesystem::remove(filepath);
            return "";
        }
        out.write(reinterpret_cast<const char*>(state_data.data()), (std::streamsize)main_size);
    }
    out.write(reinterpret_cast<const char*>(&drft_size_u64), sizeof(drft_size_u64));
    if (drft_size > 0 && ctx_dft) {
        std::vector<uint8_t> state_data(drft_size);
        if (llama_state_seq_get_data_ext(ctx_dft, state_data.data(), drft_size, slot_id, LLAMA_STATE_SEQ_FLAGS_NONE) != drft_size) {
            SRV_WRN("[auto-cache] failed to get draft KV state for %s\n", filepath.c_str());
            out.close();
            std::filesystem::remove(filepath);
            return "";
        }
        out.write(reinterpret_cast<const char*>(state_data.data()), (std::streamsize)drft_size);
    }

    if (!out.good()) {
        SRV_WRN("[auto-cache] failed to write %s\n", filepath.c_str());
        std::filesystem::remove(filepath);
        return "";
    }
    out.close();

    // save checkpoints companion file
    if (!prompt.checkpoints.empty()) {
        const std::string ckpt_path = filepath + ".ckpt";
        std::ofstream ckpt_out(ckpt_path, std::ios::binary);
        if (ckpt_out.is_open()) {
            ckpt_out.write("CKPT\x00\x00\x00\x00", 8);
            const uint32_t ckpt_ver = 1;
            const uint32_t ckpt_count = (uint32_t)prompt.checkpoints.size();
            ckpt_out.write(reinterpret_cast<const char*>(&ckpt_ver), sizeof(ckpt_ver));
            ckpt_out.write(reinterpret_cast<const char*>(&ckpt_count), sizeof(ckpt_count));
            for (const auto & cp : prompt.checkpoints) {
                const uint64_t tgt_size = cp.data_tgt.size();
                const uint64_t dft_size = cp.data_dft.size();
                ckpt_out.write(reinterpret_cast<const char*>(&cp.n_tokens), sizeof(cp.n_tokens));
                ckpt_out.write(reinterpret_cast<const char*>(&cp.pos_min),  sizeof(cp.pos_min));
                ckpt_out.write(reinterpret_cast<const char*>(&cp.pos_max),  sizeof(cp.pos_max));
                ckpt_out.write(reinterpret_cast<const char*>(&tgt_size), sizeof(tgt_size));
                ckpt_out.write(reinterpret_cast<const char*>(cp.data_tgt.data()), (std::streamsize)tgt_size);
                ckpt_out.write(reinterpret_cast<const char*>(&dft_size), sizeof(dft_size));
                if (dft_size > 0) {
                    ckpt_out.write(reinterpret_cast<const char*>(cp.data_dft.data()), (std::streamsize)dft_size);
                }
            }
            ckpt_out.close();
        }
    }

    // update index.json with user message texts/hashes
    if (!user_msg_texts.empty()) {
        json idx = load_index(slot_save_path);
        json msg_array = json::array();
        for (const auto & text : user_msg_texts) {
            if ((int)text.size() <= (int)PLAINTEXT_MAX_LEN) {
                msg_array.push_back(json{{"text", text}});
            } else {
                uint8_t hash[32];
                sha256_ctx ctx;
                sha256_init(&ctx);
                sha256_update(&ctx, (const uint8_t*)text.data(), text.size());
                sha256_final(&ctx, hash);
                std::ostringstream oss;
                for (int j = 0; j < 32; j++)
                    oss << std::hex << std::setfill('0') << std::setw(2) << (int)hash[j];
                std::string preview = text.substr(0, 50);
                msg_array.push_back(json{{"hash", oss.str()}, {"preview", preview}});
            }
        }
        idx[filename] = msg_array;
        save_index(slot_save_path, idx);
    }

    // eviction: keep at most max_pairs .kvcache files
    if (max_pairs > 0) {
        std::vector<std::filesystem::path> candidates;
        for (const auto & entry : std::filesystem::directory_iterator(slot_save_path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".kvcache") {
                candidates.push_back(entry.path());
            }
        }
        // sort by name (=timestamp), oldest first
        std::sort(candidates.begin(), candidates.end());
        json idx = load_index(slot_save_path);
        while ((int)candidates.size() > max_pairs) {
            const auto & old = candidates.front();
            const std::string old_name = old.filename().string();
            std::filesystem::remove(old);
            std::filesystem::remove(std::filesystem::path(old.string() + ".ckpt"));
            idx.erase(old_name);
            SRV_INF("[auto-cache] evicted %s (max %d pairs)\n", old_name.c_str(), max_pairs);
            candidates.erase(candidates.begin());
        }
        save_index(slot_save_path, idx);
    }

    SRV_INF("[auto-cache] saved %u tokens, main=%zu, drft=%zu to %s\n", n_tok, main_size, drft_size, filepath.c_str());
    return filepath;
}

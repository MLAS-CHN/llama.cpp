#pragma once

#include <string>
#include <vector>
#include "server-common.h"  // server_tokens, SRV_INF
#include "server-task.h"    // server_prompt
#include "llama.h"          // llama_context, llama_state_seq_*

// Find the best .kvcache file by user message text/hash exact match.
// user_msg_texts: list of user message strings (pure text, no tool results).
// Returns filepath, or empty if no exact match found.
std::string disk_cache_find(
    const std::string & slot_save_path,
    const std::vector<uint8_t> & /*user_msg_hashes*/,
    const std::vector<std::string> & user_msg_texts);

// Load KV state + checkpoints from file into ctx/ctx_dft and prompt.
// slot_id is the seq_id for KV state operations.
void disk_cache_load(
    const std::string & filepath,
    server_prompt & prompt,
    int slot_id,
    llama_context * ctx_tgt,
    llama_context * ctx_dft);

// Save KV state + checkpoints from ctx/ctx_dft to disk.
// user_msg_texts: list of user message strings (pure text, no tool results).
// max_pairs: max .kvcache+.ckpt file pairs to keep (0 = unlimited).
// Returns saved filepath, or empty on failure.
std::string disk_cache_save(
    const server_prompt & prompt,
    int slot_id,
    const std::string & slot_save_path,
    llama_context * ctx_tgt,
    llama_context * ctx_dft,
    const std::vector<std::string> & user_msg_texts,
    int max_pairs);
#pragma once

#include <string>
#include <vector>
#include "server-task.h"
#include "llama.h"

// Find the newest .kvcache file whose required fields and user messages exactly match.
// Returns filepath, or empty if no exact match found.
std::string disk_cache_find(
    const std::string & slot_save_path,
    const std::vector<std::string> & required_match_texts,
    const std::vector<std::string> & user_msg_texts);

// Load KV state + checkpoints from file into ctx_tgt/ctx_dft and prompt.
// Returns true on success.
bool disk_cache_load(
    const std::string & filepath,
    server_prompt & prompt,
    int slot_id,
    llama_context * ctx_tgt,
    llama_context * ctx_dft);

// Save KV state + checkpoints from ctx/ctx_dft to disk.
// required_match_texts: fields that must match exactly before a cache entry is considered usable.
// user_msg_texts: list of user message strings (pure text, no tool results).
// max_pairs: max .kvcache+.ckpt file pairs to keep (0 = unlimited).
// Returns saved filepath, or empty on failure.
std::string disk_cache_save(
    const server_prompt & prompt,
    int slot_id,
    const std::string & slot_save_path,
    llama_context * ctx_tgt,
    llama_context * ctx_dft,
    const std::vector<std::string> & required_match_texts,
    const std::vector<std::string> & user_msg_texts,
    int max_pairs);

// Remove a .kvcache file pair and its index entry.
void disk_cache_remove(
    const std::string & slot_save_path,
    const std::string & filepath);

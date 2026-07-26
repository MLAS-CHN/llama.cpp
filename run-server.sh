#!/bin/bash
build/bin/llama-server \
    -m ~/models/Qwen3.5-9B-Q4_K_M.gguf \
    -c 131072 \
    -ctk q4_0 -ctv q4_0 \
    --slot-save-path ~/llama.cpp/kvcaches \
    --auto-disk-cache \
    --host 0.0.0.0 --port 8080 \
    -t 32 -ngl 99 --no-mmap --perf --reasoning on --parallel 1

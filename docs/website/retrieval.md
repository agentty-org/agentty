---
title: Retrieval (RAG)
description: How agentty's search_docs and search_code tools find the right docs and code, how to turn on semantic search in two commands, and how to check it's working.
nav_section: Tools
nav_order: 40
slug: retrieval
---

agentty gives the model two local search tools. **`search_code`** finds code by what it does ("where are retries handled") when you don't know the name. **`search_docs`** searches your docs folder, your installed skills, and facts you asked it to `remember`. Both work with no setup using keyword search, and everything runs on your machine.

For exact names and strings, the model uses `grep` instead.

## Turn on search by meaning

Keyword search needs no setup. To also search by meaning, run [Ollama](https://ollama.com) and pull an embedding model:

```bash
ollama pull qwen3-embedding:0.6b    # best for code, ~640 MB
# or: ollama pull nomic-embed-text  # smaller, ~270 MB
```

That's all. agentty finds Ollama on `127.0.0.1:11434` by itself. It prefers `qwen3-embedding:0.6b` and falls back to `nomic-embed-text` if that's the one you pulled.

The first search in a repo builds the index. This takes from about 30 seconds to a few minutes on a large repo. After that, the index loads in about a second and only changed files are updated.

Check that it's on:

```bash
agentty diagnostics
```

The `retrieval (embeddings)` section should say `probe: ok`. Each search result header also says `hybrid` (keyword + meaning) or `bm25` (keyword only).

To use a different model or a remote server, type `/settings` and open **Proactive retrieval (RAG)** (the Retrieval pane), or set:

```bash
export AGENTTY_EMBED_MODEL=nomic-embed-text
export AGENTTY_OLLAMA_HOST=gpu-box.lan:11434
```

## If results look wrong

| Symptom | Cause | Fix |
|---|---|---|
| Results say `bm25`, never `hybrid` | No embedding server, or the model isn't pulled | `ollama pull qwen3-embedding:0.6b`, then `agentty diagnostics` |
| `diagnostics` says `invalid: host ...` | A typo in the Retrieval pane (e.g. `127.0.0`) | Fix the host there, or delete the `rag` keys in `~/.agentty/settings.json` |
| `search_docs` finds nothing | No docs folder | Put docs in `./docs` or `./.agentty/knowledge`, or set `AGENTTY_DOCS_DIR` |
| First search is slow | The index is being built | Only the first time per repo and model |
| Wrong file comes first | The query matches a sibling file better | Ask with words from the file's purpose; `grep` for exact names |

When a search returns no hits, the result says why. `no matches (bm25)` means it ran, nothing was relevant, and meaning search was off. `no matches (no docs indexed — set AGENTTY_DOCS_DIR …)` means there was nothing to search.

## How well it works

agentty measures its own search on a judged set of real questions about its own code and docs (`tests/rag_eval`, run with `agentty rag-eval <file>`). "Right file first" means the file that answers the question is the top result:

| Search | Setup | Right file first | Right file in top 8 |
|---|---|---:|---:|
| `search_code` | keyword only | 57-60% | 92-95% |
| `search_code` | + `nomic-embed-text` | 60-63% | 88-90% |
| `search_code` | + `qwen3-embedding:0.6b` | 63-78% | 93-97% |
| `search_docs` | + embeddings | 70-85% | 93-97% |

Each range covers the main and held-out halves of the set (60 + 41 code questions, 30 + 20 docs questions).

What helps code search:

- Identifiers are split into words, so `onClipboardRead` matches "clipboard read" (`AGENTTY_RAG_SPLIT_IDENT=0` turns this off).
- Source files rank above tests and docs unless you ask about tests or docs.
- A header pulls its `.cpp` up with it.
- Results spread across files instead of returning one file eight times.

`AGENTTY_RAG_CODE_RERANK=0` turns off these code-specific ranking rules.

## How a search runs

Each search passes through these stages in order. Cheap stages are on. Slow or corpus-sensitive stages are off until you turn them on. Defaults are set in `src/rag/adapter.cpp:715-761`.

| Stage | What it does | Default | Env var |
|---|---|---|---|
| Chunking | Splits code at functions and classes, docs at headings | On | - |
| Contextual chunks | Adds context about where a chunk sits to the chunk | On | `AGENTTY_RAG_CONTEXTUAL=0` |
| Keyword search (BM25) | Matches words | On | - |
| Meaning search | Matches embeddings, if the startup check found a model | On when a model answers | `AGENTTY_EMBED_MODEL` |
| Fusion | Merges the two lists. Convex by default; `rrf` is weighted reciprocal-rank fusion | `convex` | `AGENTTY_RAG_FUSION=rrf` |
| Adaptive fusion | Picks the keyword/meaning balance per query | On | `AGENTTY_RAG_ADAPTIVE=0` |
| Reranker | Deterministic rerank, plus code rules for `search_code` | On | `AGENTTY_RAG_CODE_RERANK=0` |
| Diversity (MMR) | Spreads results across files | On, lambda 0.65 | `AGENTTY_RAG_MMR=0`, `AGENTTY_RAG_MMR_LAMBDA` |
| Dedup | Drops near-duplicate passages | On, threshold 0.92 | `AGENTTY_RAG_DEDUP=0`, `AGENTTY_RAG_DEDUP_THRESHOLD` |
| Autocut | Cuts the list where scores drop sharply | On, sensitivity 2.0 | `AGENTTY_RAG_AUTOCUT=0`, `AGENTTY_RAG_AUTOCUT_SENSITIVITY` |
| Stitching | Joins adjacent hits from the same file | On | `AGENTTY_RAG_STITCH=0` |
| Trimming | Fits passages into the size budget (see [Token budget](#token-economy)) | On | `AGENTTY_RAG_OUTPUT_BYTES` |

Every result starts with a short **funnel** line. It shows how many candidates each stage kept and whether the run was `bm25` or `hybrid`. You can see when a query found little, or when the relevance floor cut a weak tail.

The off-by-default stages are listed in [Optional stages](#optional-advanced-stages). Turn one on only after measuring it on your own files with `agentty rag-bench` and real questions.

## What gets indexed

| Source | Default | Setting |
|---|---:|---|
| Docs folder | Found automatically | `AGENTTY_DOCS_DIR`, then `./docs`, then `./.agentty/knowledge` |
| Installed skills | On | `AGENTTY_RAG_SKILLS=0` turns off |
| Learned memory | On | `AGENTTY_RAG_MEMORY=0` turns off |
| MCP resources | Off | `AGENTTY_RAG_MCP=1` turns on |
| Workspace source code | On demand | Indexed separately by `search_code` |

MCP indexing is off by default because reading a resource can mean server I/O. MCP indexes are not saved between sessions, so stale resources can't leak into a later run.

## Meaning search and fallback

At startup agentty runs one quick embedding check. If the server or model is missing, the session stays keyword-only. This avoids a failed network timeout on every search. The check waits up to 30 seconds for Ollama to load a model. A server that isn't running fails at once.

With the backend on **Auto** (the default) and the model left at its default, agentty tries `qwen3-embedding:0.6b` first (`src/rag/embed_backend.cpp:321`) and uses `nomic-embed-text` if that isn't pulled. A model or backend you set yourself is used as is.

Models trained with query/document prefixes (nomic, e5, mxbai, arctic) get them automatically. `AGENTTY_EMBED_PREFIX=0` turns that off. Other backends (an OpenAI-compatible endpoint, llama.cpp, or an in-process GGUF/ONNX model) are picked in the Retrieval pane.

`AGENTTY_RAG_BM25_WEIGHT` and `AGENTTY_RAG_DENSE_WEIGHT` (both default 1.0) set the keyword/meaning balance in `rrf` mode. The default convex mode adapts the balance per query instead.

## Proactive retrieval

By default agentty does not search before each turn. The model calls `search_docs` or `search_code` only when it needs to, which costs fewer tokens.

To search automatically, type `/settings` and open **Proactive retrieval (RAG)**. Pick one mode:

| Mode | What it does |
|------|-----------|
| **On** | Retrieve and inject context before **every** turn |
| **First turn only** | Ground the **first** turn of a thread, then stay quiet |
| **Off** | No injection before turns. The search tools still work |

The choice is saved in `settings.json`. A [forked thread](/docs/fork) can have its own mode, chosen in the fork picker. `AGENTTY_RAG_PROACTIVE=1` is the env-var equivalent of **On**:

```bash
export AGENTTY_RAG_PROACTIVE=1
```

When it's on, agentty runs one retrieval in the background, shows `retrieving context…`, and starts the model when retrieval is done. It injects only when retrieval confidence is at least `AGENTTY_RAG_PROACTIVE_MIN` (default 0.35, `adapter.cpp:760`). The block is capped at about 6 KiB (~1,500 tokens), tighter than the tool budget, because you didn't ask for it:

```bash
export AGENTTY_RAG_PROACTIVE_BYTES=6144   # clamped 1 KiB - 32 KiB
```

Injected context shows as its own **retrieved-context** card, not as your message. The card lists the sources and shows a confidence bar. These blocks never appear in [[↑]]/[[↓]] history recall. They ground only that turn, and older ones are dropped from later requests so you aren't charged for them again.

## Optional stages

These are off by default. Each adds latency or can hurt results on some corpora.

| Stage | Turn on with | Default | Code |
|---|---|---|---|
| Pseudo-relevance feedback | `AGENTTY_RAG_PRF=1` | Off | `adapter.cpp:735` |
| Corrective grading | `AGENTTY_RAG_CORRECT=1` | Off | `adapter.cpp:736` |
| GraphRAG | `AGENTTY_RAG_GRAPH=1` | Off | `adapter.cpp:737` |
| Multi-query expansion | `AGENTTY_RAG_EXPAND=1` | Off | `adapter.cpp:740` |
| HyDE | `AGENTTY_RAG_HYDE=1` | Off | `adapter.cpp:741` |
| Learning from file opens | `AGENTTY_RAG_LEARN=1` | Off | `adapter.cpp:2832` |

### PRF

`AGENTTY_RAG_PRF=1` expands the query with words from the top keyword hits. It can help when your words differ from the file's words. It can also drift toward a wrong first result.

### GraphRAG (off by default)

`AGENTTY_RAG_GRAPH=1` builds links between documents, similarity edges, and communities, then expands results along the graph. Building the graph can take time quadratic in the number of documents, so it is not a default. Base and graph results are merged by rank because their scores use different scales.

### Corrective grading

`AGENTTY_RAG_CORRECT=1` grades each retrieval using its scores plus keyword support. A meaning match is not rejected just because it paraphrases the query. Grading keeps the number of results you asked for, so `k=10` stays 10.

### Multi-query and HyDE

`AGENTTY_RAG_EXPAND=1` and `AGENTTY_RAG_HYDE=1` use a small local model (`AGENTTY_RAG_GEN_MODEL`, default `qwen2.5:0.5b`) to rewrite the query. They help with vague or research-style questions but add model latency.

### Learning

`AGENTTY_RAG_LEARN=1` turns on an experimental loop that learns from which files you open next. It is stored in `.agentty/rag_feedback.tsv`. It's off because opening a file afterwards doesn't prove a skill, memory, or passage was relevant.

## Saved indexes and freshness

Indexes are saved and reopened, not rebuilt on every start. They live with the project, so each checkout has its own:

```text
<project>/.agentty/cache/rag_docs.<id>.ragdb   # docs, skills, memory
<project>/.agentty/cache/rag_code.<id>.ragdb   # search_code
```

`AGENTTY_RAG_DIR` moves them (see [Environment](/docs/environment)).

`<id>` names the embedding model, so switching models keeps the old index for when you switch back. Each index stores the size and mtime of every file. When files change, only those files are reindexed, so one edited file costs seconds. Changing the chunking, the docs folder, or the model starts a fresh index. A large change set also triggers a clean rebuild.

The freshness scan:

- skips `.git`, build outputs, dependency trees, virtual environments, and caches;
- looks only at file types and sizes the loader can index;
- includes relative paths, so a rename updates the index;
- fingerprints skill and memory content, not just their count.

`search_code` keeps its own index, separate from docs. It splits source at definitions, so a result is usually a whole function, class, or declaration. It indexes at most 4,000 source files of up to 256 KiB each.

## Token budget

Retrieval is the one place agentty spends model tokens for you, so its output is tightly budgeted. These settings shape how many tokens the ranked passages use. None of them change the ranking. All are on by default.

| Setting | What it does | Default |
|---|---|---|
| `AGENTTY_RAG_OUTPUT_BYTES` | Total size of all returned passages, clamped 2 KiB - 64 KiB | 12288 (~3,000 tokens) |
| `AGENTTY_RAG_RELEVANCE_FLOOR` | Keeps a passage only if its score is within this fraction of the top hit. The top hit is always kept. `0` keeps all | 0.30 |
| `AGENTTY_RAG_BUDGET_GAMMA` | Splits bytes in proportion to score^gamma, with a per-passage floor. Higher gives more to the top hits | 1.5 |
| `AGENTTY_RAG_CONF_BUDGET_FLOOR` | A low-confidence retrieval gets only this fraction of the total budget, rising to 100% at full confidence | 0.45 |
| `AGENTTY_RAG_EXTRACTIVE` | Compresses long prose passages. `0` forces a plain line window | On |
| `AGENTTY_RAG_PROACTIVE_BYTES` | Separate cap for proactive injection | 6144 |

Defaults are in `src/rag/adapter.cpp:216-290`.

When a prose passage is too long, agentty keeps the sentences that best match the query (rare query words count more). It prints them in their original order with `…` marking gaps. No model is involved. Code and config keep a single block of lines with the most query words, because code is hard to read with gaps.

The requested `k` is still honored when enough distinct passages exist.

Measured effect: on a 161-file corpus, these settings together cut retrieval output from ~1,845 to ~1,605 estimated tokens per query (−13%), with identical ranking (recall@10 1.000, MRR 0.968, nDCG@10 0.976). Small chunks that already fit see little change. Retrieval is a small part of a turn's tokens, since the system prompt, tools, and history dominate and are mostly cached. Measure on your own corpus instead of assuming.

## Measuring retrieval

Two built-in tools, both offline.

`agentty rag-eval <cases.json>` scores search against questions with known answers, using the real tools. agentty's own sets are in `tests/rag_eval/` (code and docs, each with a held-out half kept out of tuning). A cases file is a list of `{"q": "...", "gold": ["path", ...]}`. Wrap it as `{"tool": "search_docs", "cases": [...]}` to test docs search. It prints hit@1 (right file first), hit@8, MRR, and every miss:

```bash
agentty rag-eval tests/rag_eval/agentty_code.json
AGENTTY_RAG_EVAL_ALL=1 agentty rag-eval my_cases.json   # every case's rank
```

Write twenty questions about your repo, each with the file that answers it. Then you can check whether a model or setting change really helped. The `rag_eval_guard` test runs agentty's sets keyword-only on every build and fails if search gets worse.

`agentty rag-bench` benchmarks `search_docs` on your docs folder. It reports indexing time, recall@k, MRR, nDCG@10, and milliseconds per query:

```bash
agentty rag-bench docs
```

To measure **token cost**, add `AGENTTY_RAG_MEASURE=1`. The queries then go through the real output path (relevance floor, budget split, confidence budget, compression). It reports bytes and estimated tokens per query, plus a machine-readable `MEASURE` line:

```bash
AGENTTY_RAG_MEASURE=1 agentty rag-bench docs
```

To measure one setting, run twice with it toggled and compare the `tokens=` field:

```bash
AGENTTY_RAG_MEASURE=1 AGENTTY_RAG_EXTRACTIVE=0 agentty rag-bench docs   # off
AGENTTY_RAG_MEASURE=1                          agentty rag-bench docs   # on
```

No rebuild is needed. `rag-bench` builds its queries from the documents, so it tests the mechanics. `rag-eval` with real questions tests what you actually care about. If a metric drops after turning on a stage, leave it off or tune it.

## Provenance

Every passage keeps its source and path:

```text
docs:guide/auth.md:20-44
skill:release-checklist
memory:fact-id
mcp:file://resource-uri
code:src/auth/token.cpp:80-126
```

Use it to cite, open, or check what was retrieved.

## Why it works this way

- **Cheap by default.** Slow or risky stages are off, so every search has predictable latency and token cost.
- **Fallback, not failure.** One startup check decides between hybrid and keyword-only, so a missing model never slows each search.
- **Local.** Indexes live in your project. Embeddings run on your machine unless you point `AGENTTY_OLLAMA_HOST` elsewhere.
- **Measured.** Optional stages stay off until you measure them on your own corpus. `rag_eval_guard` catches regressions.

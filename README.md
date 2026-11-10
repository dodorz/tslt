# tslt

`tslt` is a minimal native Windows text translation tool built with Win32 API, CMake, and WinHTTP. It reads LLM provider settings from `tslt.ini`, keeps window state in `%LOCALAPPDATA%`, and runs translation requests on a worker thread so the UI stays responsive.

## Current scope

- Native Win32 desktop UI
- OpenAI-compatible chat-completions backends (default, `protocol = openai`)
- OpenAI Responses backends via `protocol = responses`
- Anthropic Messages backends via `protocol = anthropic`
- Configurable provider selection from `tslt.ini`
- Target language selection, automatically set to English for predominantly Chinese input and Simplified Chinese for all other predominantly textual input
- Copy translated output to clipboard
- Separate runtime state file at `%LOCALAPPDATA%/tslt/state.dat`
- Prompt history is available from the Prompt dialog and stored in `state.dat`
- Optional piped stdin input preloaded into the input box

## Build

This repository now defaults to Microsoft toolchains through CMake presets.

### Local build

```bash
cmake --preset default
cmake --build --preset default
```

Output:

- `build/vs2026-x64/Release/tslt.exe`

### Tests

`src/Json.h`, `src/IniFile.h`, `src/LlmProtocol.h`, `src/ConfigKeys.h`, `src/ApiErrorHints.h`, `src/WinHttpErrors.h`, `src/SseStream.h` and `src/DictionaryHeuristic.h` are platform independent, so they have portable unit tests:

```bash
cmake -S . -B build/tests -DTSLT_BUILD_TESTS=ON
cmake --build build/tests --target tslt_json_protocol_tests tslt_ini_tests tslt_config_keys_tests \
    tslt_api_hints_tests tslt_winhttp_errors_tests tslt_sse_tests tslt_dictionary_heuristic_tests
ctest --test-dir build/tests --output-on-failure
```

## Configuration

Copy `tslt.ini.example` to `tslt.ini` next to the executable, or place it under `%AppData%/tslt/tslt.ini`.

Example:

```ini
[LLM]
provider = Qwen

[LLM.Qwen]
name = SiliconFlow Qwen
protocol = openai
base_url = https://api.siliconflow.cn/v1
api_key = sk-XXXXX
model = Qwen/Qwen3.5-4B

[LLM.DSK]
base_url = https://api.deepseek.com
api_key = sk-xxxxxx
model = deepseek-v4-flash

[LLM.GPT]
name = OpenAI GPT (Responses API)
protocol = responses
base_url = https://api.openai.com/v1
api_key = sk-xxxxxx
model = gpt-5-mini
max_tokens = 8192

[LLM.Claude]
name = Anthropic Claude
protocol = anthropic
base_url = https://api.anthropic.com
api_key = sk-ant-xxxxxx
model = claude-sonnet-4-5
max_tokens = 8192

[Translate]
target_language = zh-CN
source_language = auto
temperature = 0.2
```

Rules:

- The active provider is selected by `[LLM].provider`.
- Every provider is loaded from an `LLM.<name>` section.
- `name` is optional and only affects the UI label.
- `protocol` selects the request format:
  - `openai` (default when omitted) - `POST <base_url>/chat/completions` with `Authorization: Bearer`, body has `messages` (plus `temperature` unless disabled).
  - `responses` - `POST <base_url>/responses` with `Authorization: Bearer`, body has `instructions` + `input` + `max_output_tokens`.
  - `anthropic` - `POST <base_url>/v1/messages` with `x-api-key` and `anthropic-version: 2023-06-01`, body has `system` + `messages` + `max_tokens`.
  - Unknown values fall back to `openai`; `openai-chat-completions`, `openai-responses`, `anthropic-messages`, `claude` are accepted aliases.
- `stream` defaults to `1`. Responses are requested with `stream: true` and the SSE deltas are assembled locally; set `stream = 0` for gateways that reject or do not support streaming. Streaming matters because a non-streamed reply only starts sending bytes after the whole answer is generated, so long inputs hit reverse-proxy timeouts (Cloudflare answers HTTP 524 after ~100s) even when the model would eventually finish. If a gateway rejects `stream`, tslt retries without it and remembers that for the remaining chunks.
- `timeout` is the send/receive timeout in seconds (default `300`, clamped to `10`-`3600`). WinHTTP's own default is 30 seconds, which is too short for reasoning models that emit reasoning tokens before the first output byte; raise it for slow models and lower it for fast local servers.
- `max_tokens` defaults to `8192` and is sent as `max_tokens` (Anthropic) or `max_output_tokens` (Responses API); the OpenAI chat-completions body leaves the limit to the server default.
- `base_url` normally already contains the version prefix (`https://api.openai.com/v1`, `https://api.siliconflow.cn/v1`). A `base_url` that already points at the final endpoint (for example `https://api.openai.com/v1/responses`) is used as-is. For `protocol = anthropic`, both `https://api.anthropic.com` and `https://api.anthropic.com/v1` work.
- `auth` is optional and controls the credential header: `auto` (default, `Authorization: Bearer` except for Anthropic which uses `x-api-key`), `bearer`, `x-api-key`, `api-key` (Azure-style), or `none` for unauthenticated local servers such as Ollama.
- `temperature` is inherited from `[Translate]`, and a provider section may override it. Set a negative value (for example `temperature = -1`) to omit `temperature` from the request body; reasoning-only models such as `gpt-5` reject it. If a model still rejects the configured temperature (for example `claude-haiku-5-5`), tslt retries the request once without `temperature` and keeps omitting it for the remaining chunks instead of failing; the status bar mentions the omission.
- A base URL query string is preserved, so Azure endpoints such as `https://<resource>.openai.azure.com/openai/v1?api-version=...` work.
- Network failures are reported with the operation, the reason and the WinHTTP code, for example `WinHttpReceiveResponse failed: timed out (12002)` or `WinHttpConnect failed: host name could not be resolved (12007)`.
- Responses are parsed structurally (`choices[0].message.content`, `output[].content[].text`, or Anthropic `content[].text`), and API errors are reported from the `error.message` field.
- `#` starts a comment anywhere on a line, so `model = Qwen/Qwen3-8B   # swap to Qwen3-30B` reads as `Qwen/Qwen3-8B`. A quoted value keeps the `#`, e.g. `name = "Qwen #1"`. `;` still comments out a whole line but is kept inside values, so API keys and URLs containing `;` survive. A duplicate key keeps its first definition.
- `tslt.ini` is validated while loading. Keys the reader never consumes are reported instead of being silently ignored: the status bar shows the first problem and the output pane lists all of them. Keys are matched case-insensitively with `-`, `_` and spaces ignored, so `api_style`, `apikey` or `max-output-tokens` are flagged with a `did you mean ...` hint (for example `api_style` -> `protocol`). Values that cannot be parsed for `protocol` or `auth` are reported the same way, as are provider sections not spelled exactly `[LLM.<name>]`.
- Dictionary selection uses language-aware thresholds (`src/DictionaryHeuristic.h`), probed against dict.cn:
  - single English word: up to `max_english_word_chars` (default `32`);
  - English phrase/idiom: up to `max_phrase_words` (default `6`) and `max_phrase_chars` (default `35`);
  - unspaced CJK run: up to `max_cjk_chars` (default `8`), since Chinese has no word separators;
  - anything longer, spaced or sentence-like goes straight to the LLM.
  Trailing and wrapping punctuation is stripped before the lookup (`apple.`, `苹果。` and `"apple"` all miss otherwise). If the dictionary returns no definition or the request fails, the input is translated by the configured LLM instead, so a miss never leaves an empty result. The legacy `auto_select_max_characters` key still works and keeps the old "single word, that many characters" rule.
- Runtime window state is never written into `tslt.ini`.

## Release

A GitHub Actions workflow at `.github/workflows/release.yml` creates Windows release zips when you push a version tag. Both `0.0.2` and `v0.0.2` will trigger the workflow.

Examples:

```bash
# Annotated tags are required for `git push --follow-tags`.
git tag -a 0.0.11 -m "tslt 0.0.11"
git push origin 0.0.11
```

```bash
git tag -a v0.0.11 -m "tslt 0.0.11"
git push --follow-tags
```

The tag is the single source of truth for the version: `.github/scripts/derive-version.ps1` strips an optional leading `v` and passes the result to CMake as `-DTSLT_VERSION=...`, so the `FileVersion`/`ProductVersion` embedded in `tslt.exe` always matches the tag. The build then fails if the executable reports a different version. Tags must be numeric (`X.Y.Z` or `X.Y.Z.W`); prerelease suffixes such as `0.0.11-rc1` are rejected rather than producing a mislabelled binary.

Local builds use `TSLT_VERSION_DEFAULT` from `CMakeLists.txt`; bump it after tagging if you want the default to track the latest release. Manual `workflow_dispatch` runs also fall back to that value.

The uploaded asset is always named without the `v` prefix:

- `tslt-0.0.11-x86_64.zip`
- `tslt-0.0.11-x86.zip`
- `tslt-0.0.11-arm64.zip`

## License

MIT. See `LICENSE`.

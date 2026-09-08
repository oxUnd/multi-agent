# morph

![demo](misc/demo.png)

A terminal-native multimodal AI agent written in pure C. Orchestrates text, image, and video generation and understanding through a ReAct loop.

中文系统介绍: [docs/introduction.zh-CN.md](docs/introduction.zh-CN.md)

## Features

- **Multimodal in one place**: text chat, image generation/editing, and video generation under a single entry point
- **ReAct engine**: automatic Thought → Action → Observation orchestration
- **Skills**: hot-loadable instruction packs (SKILL.md) that inject specialized behavior into the agent
- **Extensions**: hot-pluggable extensions running in a sandbox, written in any language
- **Local-first**: sessions and artifacts persisted to SQLite, replayable offline
- **Lightweight**: minimal static dependencies, fast startup

Sandbox capabilities, platform policy, extension manifests, and nested-macOS
testing are documented in [docs/sandbox.md](docs/sandbox.md).

## Build

Requirements: CMake ≥ 3.20, SQLite3, libcurl, and
[mathjax-c](https://github.com/oxUnd/mathjax-c). Optional: readline.

```bash
git clone https://github.com/oxUnd/mathjax-c vendor/mathjax-c
cmake -S . -B build
cmake --build build
```

Install the CLI, JavaScript runner, and runtime data files:

```bash
cmake --install build --prefix /usr/local
```

Runtime data is placed under `share/morph`, next to the `bin` directory, in
both the build and install trees. Morph resolves this fixed layout relative to
its executable. The installed user manual is available at
`share/morph/morph.txt`.

Run tests:

```bash
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure
```

With readline, the interactive prompt remains editable while the agent runs.
Press Enter to submit a requirement adjustment: the active model request yields
and ReAct continues with the new message. Running tools finish before applying
adjustments. Esc or Ctrl+C cancels; Ctrl+J or Alt+Enter inserts a newline.

Pasted images appear as blue `[IMAGE#1]`, `[IMAGE#2]` chips in the composer,
without opening a preview. Pasted image paths (including quoted or escaped
spaces) use the same chips; typed paths and `/image <path>` convert on Enter.
Backspace or Delete removes an entire chip, and only chips still present are
attached when you submit. Multiple images and images added during a turn are
supported. Use `/render <path>` when you explicitly want a preview.

The PTY regression uses the production CLI, a disposable database, and a local
streaming model server. It checks both rendered terminal screens and model
requests (including editing, steering, resizing, pasting, and cancellation):

```sh
python3 -m venv /tmp/morph-pty-venv
/tmp/morph-pty-venv/bin/pip install pexpect pyte
cmake -S . -B build -DPython3_EXECUTABLE=/tmp/morph-pty-venv/bin/python
cmake --build build
ctest --test-dir build -R cli_pty_integration --output-on-failure
```

CTest skips this regression when the Python dependencies are unavailable.


## Configuration

Copy the example config and set your API key:

```bash
mkdir -p ~/.morph
cp config.toml.example ~/.morph/config.toml
export OPENAI_API_KEY=sk-...
```

Supported providers: `openai`, `volcengine`, `deepseek`.

## Usage

```bash
./build/bin/morph
```

Optional flags:

- `-c <path>`: specify a config file
- `-w <path>`: specify the working directory
- `-p <prompt>`: run one prompt and exit
- `-s <name>`: select or create a named session; combine with `-p` to reuse
  the same conversation across invocations

## Extensions

Extensions are installed under `[ext].dir` from `config.toml`, which defaults to
`~/.morph/exts`.

Install from GitHub:

```bash
/ext install github:owner/repo
/ext install github:owner/repo@v1.2.0
/ext install github:owner/repo//exts/foo
/ext install github:owner/repo@v1.2.0//exts/foo
/ext install https://github.com/owner/repo/tree/main/exts/foo
```

The source format is `github:<owner>/<repo>[@ref][//subdir]`. `ref` may be a
tag, branch, or commit. Monorepo installs use `subdir` as the extension package
root. GitHub tree URLs are also accepted for the common
`https://github.com/<owner>/<repo>/tree/<ref>/<subdir>` form.

An extension package contains `manifest.toml` or `morph-ext.toml`:

```toml
name = "demo-native"
version = "0.1.0"
description = "Native demo extension"
type = "exec"
entry = "bin/demo-native"
fronts = ["cli"]
categories = ["dev"]

[build]
command = "make build"
```

`[build]` is optional. If present, morph asks before running the command unless
`--yes` is passed. After download or build, `entry` must exist inside the
package directory; no separate output list is configured.

## Layout

```
src/
  agent/    ReAct loop, context compression, tool dispatch
  agent/tools/
            Built-in tools (credits, memory, img_gen, vid_gen, ...)
  persistence/
            Persistent stores for memory and credit queries
  models/   LLM / image / video backends
  skill/    Skill discovery, parsing, and activation
  ext/      Ext loading and management
  sandbox/  Sandboxed ext execution
  ipc/      JSON-RPC
  render/   Markdown / image / video terminal rendering
exts/       Example exts (manifest.toml + entry script)
vendor/     Third-party libraries (cJSON, stb_image, toml)
```

See [AGENTS.md](AGENTS.md) for conventions and [REQUIREMENTS.md](docs/REQUIREMENTS.md) for the full spec.

# Package Management Prototype

This document proposes a package-management design for Nari that avoids two common failure modes:

1. **Node-style duplication** where the same dependency is copied into many projects.
2. **Python-style environment sprawl** where users accumulate many isolated interpreter/package environments.

The design favors a **global immutable package store** plus **per-project lockfiles**, with no per-project package copies.

## Goals

- One shared package store for all projects.
- Reproducible builds via a lockfile.
- Minimal parser changes.
- Good support for pure `.nari` source packages.
- Future support for precompiled `.naric` packages.
- No requirement to create a full per-project environment.

## Non-Goals for the First Prototype

- Solving multi-version package isolation perfectly.
- Supporting arbitrary install scripts.
- Building a full central registry protocol.
- Replacing the existing file-based `import` flow for local project code.

## Key Idea

Use three layers:

1. **Manifest**: human-edited project metadata and dependency ranges.
2. **Lockfile**: exact resolved versions and integrity hashes.
3. **Global store**: immutable unpacked packages shared across all projects.

That gives the reproducibility of Python lock-based workflows without the "one full environment per project" cost, and the deduplication Node generally lacks.

## Store Layout

A layout close to this is recommended:

```text
~/.nari/
  store/
    pkg/
      <package-id>/
        nari.toml
        src/
        naric/
    src/
      <archive-hash>/
  cache/
    registry/
    downloads/
  abi/
    <abi-version>/
      build/
        <package-id>/
```

### Important Change from `~/.nari/<VERSION>/packages/...`

Using the full interpreter version as the top-level directory will cause avoidable duplication.

Instead, separate:

- **Package store identity**: based on package contents.
- **Language/package ABI**: used only for compiled artifacts.
- **Interpreter release version**: mostly irrelevant for pure source packages.

A better package path is:

```text
~/.nari/store/pkg/<name>@<version>-<content_hash>/
```

Examples:

```text
~/.nari/store/pkg/std-json@1.4.0-a1b2c3d4/
~/.nari/store/pkg/acme-http@0.2.1-9f8e7d6c/
```

Then compiled caches can live under an ABI-scoped path:

```text
~/.nari/abi/1/build/std-json@1.4.0-a1b2c3d4/
```

This avoids reinstalling source packages every time Nari itself changes.

## Package Identity

Each installed package should be identified by:

- package name
- exact version
- normalized source hash

Recommended ID format:

```text
<name>@<version>-<short_hash>
```

Where `<short_hash>` is derived from the package archive or normalized file tree, not just the name.

## Manifest Format

TOML is a good choice for the prototype.

Reasons:

- easier to read and edit by hand than JSON
- still straightforward to parse in C++
- works well for manifests and lockfiles
- avoids inventing a custom format too early

Suggested filename:

```text
nari.toml
```

Example:

```toml
packageFormat = 1
name = "acme/my-app"
version = "0.1.0"
entry = "src/main.nari"
registries = ["https://packages.nari-lang.org/index.toml"]

[dependencies]
"std/json" = { version = ^1.4.0 }
"acme/http" = { version = ~0.2.0 }
"local/util" = { path = "../util" }
```

## Formal Schema: `nari.toml`

The root project manifest and the published package manifest use the same file name, but they are validated in slightly different modes.

### Common Top-Level Keys

| Key | Type | Required | Notes |
| --- | --- | --- | --- |
| `packageFormat` | integer | yes | Schema version. Start with `1`. |
| `name` | string | yes | Canonical package or project name. |
| `version` | string | yes | Semantic version string like `0.1.0`. |
| `dependencies` | table | no | Dependency declarations keyed by package name. |
| `registries` | array of strings | no | Registry index URLs. Usually only needed in the root manifest. |

### Root Project Keys

| Key | Type | Required | Notes |
| --- | --- | --- | --- |
| `entry` | string | yes | Entry file for the application or package executable. |

### Published Package Keys

| Key | Type | Required | Notes |
| --- | --- | --- | --- |
| `exports` | table | yes | Export map from import subpath to source or compiled file. |

### Name Rules

For the MVP, package names should follow this shape:

```text
<namespace>/<name>
```

Examples:

```text
std/json
acme/http
wearr/termui
```

Recommended restrictions:

- lowercase ASCII only
- allowed characters: `a-z`, `0-9`, `-`, `_`, `/`
- exactly one `/` separator for the MVP
- no leading or trailing `/`
- no `.` or `..` path segments

### Version Rules

For the MVP, versions should use semantic versioning:

```text
MAJOR.MINOR.PATCH
```

Examples:

```text
1.0.0
0.2.4
12.1.3
```

Version requirement strings in dependencies may support a limited range syntax initially:

- exact: `1.2.3`
- compatible: `^1.2.3`
- patch/minor constrained: `~1.2.3`

### Dependency Entry Schema

Each key in `[dependencies]` is a package name. Each value is an inline table.

Allowed shapes:

```toml
[dependencies]
"std/json" = { version = ^1.4.0 }
"acme/http" = { version = ~0.2.0 }
"local/util" = { path = "../util" }
```

Formal rules:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `version` | string | conditional | Required for registry dependencies. |
| `path` | string | conditional | Required for local path dependencies. |

Validation rules:

- exactly one of `version` or `path` must be present
- the prototype parser accepts bare semver tokens like `0.1.0`, `^1.2.3`, and `~1.2.3` inside dependency inline tables
- `path` must be relative in the MVP
- dependency keys must be unique
- a package cannot depend on itself

### Root Manifest Example

```toml
packageFormat = 1
name = "acme/my-app"
version = "0.1.0"
entry = "src/main.nari"
registries = ["https://packages.nari-lang.org/index.toml"]

[dependencies]
"std/json" = { version = ^1.4.0 }
"acme/http" = { version = ~0.2.0 }
"local/util" = { path = "../util" }
```

## Package Manifest

Each published package should also contain a manifest.

Example:

```toml
packageFormat = 1
name = "std/json"
version = "1.4.0"

[exports]
"." = "src/json.nari"
encode = "src/encode.nari"
decode = "src/decode.nari"

[dependencies]
"std/strings" = { version = "^1.0.0" }
```

### Package Manifest Schema

Published packages use `nari.toml` too.

Required fields:

| Key | Type | Required | Notes |
| --- | --- | --- | --- |
| `packageFormat` | integer | yes | Must currently be `1`. |
| `name` | string | yes | Canonical package name. |
| `version` | string | yes | Exact package version. |
| `exports` | table | yes | Export map used by import resolution. |

Optional fields:

| Key | Type | Required | Notes |
| --- | --- | --- | --- |
| `dependencies` | table | no | Same schema as root dependencies. |
| `registries` | array of strings | no | Usually omitted for published packages. |

### `exports` Schema

The `exports` table maps import subpaths to files inside the package.

Example:

```toml
[exports]
"." = "src/json.nari"
encode = "src/encode.nari"
decode = "src/decode.nari"
```

Resolution rules:

- `import "std/json";` resolves through `exports["."]`
- `import "std/json/encode";` resolves through `exports["encode"]`
- `import "std/json/decode";` resolves through `exports["decode"]`

Validation rules:

- every export target must be a relative path inside the package
- export targets must not escape the package root
- export targets should point to either `.nari` or `.naric`
- `"."` must exist for any package intended to be imported by its bare name

## Lockfile Format

Suggested filename:

```text
nari.lock.toml
```

This file should be generated, not hand-edited.

Example:

```toml
lockfileVersion = 1
root = "acme/my-app@0.1.0"

[packages."std/json"]
version = "1.4.0"
integrity = "sha256-..."
storePath = "~/.nari/store/pkg/std-json@1.4.0-a1b2c3d4"

[packages."std/json".dependencies]
"std/strings" = "1.0.2"

[packages."std/strings"]
version = "1.0.2"
integrity = "sha256-..."
storePath = "~/.nari/store/pkg/std-strings@1.0.2-f0e1d2c3"
```

## Formal Schema: `nari.lock.toml`

The lockfile is generated by tooling and should preserve exact resolution state.

### Top-Level Keys

| Key | Type | Required | Notes |
| --- | --- | --- | --- |
| `lockfileVersion` | integer | yes | Start with `1`. |
| `root` | string | yes | Root project identity, usually `<name>@<version>`. |
| `packages` | table | yes | Exact resolved package set keyed by package name. |

### Package Entry Schema

Each package entry is stored under:

```toml
[packages."<package-name>"]
```

Required fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `version` | string | yes | Exact resolved version. |
| `integrity` | string | yes | Content hash, for example `sha256-...`. |
| `storePath` | string | yes | Absolute or store-relative installed package path. |

Optional fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `source` | string | no | Example values: `registry`, `path`. |

Dependency pins are stored under a nested table:

```toml
[packages."std/json".dependencies]
"std/strings" = "1.0.2"
```

Validation rules:

- every package listed in a dependency table must also exist under `[packages]`
- dependency values in the lockfile are exact versions, not ranges
- the flat-graph MVP means there is only one `[packages."name"]` entry per package name
- `storePath` must point under `~/.nari/store/pkg/`

### Lockfile Example

```toml
lockfileVersion = 1
root = "acme/my-app@0.1.0"

[packages."std/json"]
version = "1.4.0"
integrity = "sha256-abc123..."
storePath = "~/.nari/store/pkg/std-json@1.4.0-a1b2c3d4"
source = "registry"

[packages."std/json".dependencies]
"std/strings" = "1.0.2"

[packages."std/strings"]
version = "1.0.2"
integrity = "sha256-def456..."
storePath = "~/.nari/store/pkg/std-strings@1.0.2-f0e1d2c3"
source = "registry"
```

## Import Design

Import statements should not need a `pkg:` prefix.

Nari already accepts string-based imports, so the resolver can distinguish local paths from package names without changing the surface syntax much.

However, the long-term direction should be a **proper language-level module syntax** rather than relying only on side-effect imports and globals.

## Proposed Language-Level Import Syntax

The package system should pair with a real import/export model.

### Namespace Import

This should import the module namespace as a single value:

```nari
import json from "std/json";

let value = json.decode("{\"ok\":true}");
```

Semantics:

- `json` is a module namespace object
- the object exposes all exported bindings from `std/json`
- the namespace object should be read-only from user code

This fits well with the existing FFI-style `import name from "..."` syntax.

### Named Imports

This should import selected symbols directly:

```nari
import { json_decode } from "std/json";

let value = json_decode("{\"ok\":true}");
```

Aliasing should also be supported:

```nari
import { json_decode as decode, json_encode as encode } from "std/json";
```

### Side-Effect Imports

The current form should remain valid for modules that only need top-level execution:

```nari
import "./polyfills.nari";
import "./config/bootstrap.nari";
```

### Suggested Import Forms

For the MVP, these forms give a complete and coherent model:

```nari
import "./side_effect_only.nari";
import json from "std/json";
import { json_decode } from "std/json";
import { json_decode as decode } from "std/json";
```

## Proposed Export Syntax

To support named imports cleanly, Nari should gain explicit export syntax.

### Exported Declarations

Recommended forms:

```nari
export func json_decode(text) {
  // ...
}

export func json_encode(value) {
  // ...
}

export let JSON_VERSION = "1.0";
```

### Export Lists

It should also be possible to export existing bindings explicitly:

```nari
func json_decode(text) {
  // ...
}

func json_encode(value) {
  // ...
}

export { json_decode, json_encode };
```

Aliased exports may be useful later:

```nari
export { json_decode as decode };
```

### Module Namespace Semantics

When a module is imported with:

```nari
import json from "std/json";
```

the runtime should produce a namespace object roughly equivalent to:

```nari
{
  json_decode: <function>,
  json_encode: <function>,
  JSON_VERSION: "1.0"
}
```

except treated as an immutable module namespace rather than a normal mutable object.

### Example Module

```nari
// std/json/src/json.nari
export func json_decode(text) {
  // ...
}

export func json_encode(value) {
  // ...
}

export let JSON_VERSION = "1.0";
```

Consumers could then write either:

```nari
import json from "std/json";
print(json.JSON_VERSION);
```

or:

```nari
import { json_decode } from "std/json";
let value = json_decode("{}");
```

## Package Specifiers

Suggested package specifiers:

```nari
import "std/json";
import "std/json/encode";
```

Local imports stay as they are:

```nari
import "./helpers.nari";
import "../shared/config.nari";
import "/usr/local/share/nari/site/init.nari";
import "std/json";
```

That means the parser can continue treating imports as strings while the resolver adds package-aware behavior.

In practice, once real module syntax exists, the preferred forms should become:

```nari
import json from "std/json";
import { json_decode } from "std/json";
```

## Resolution Rules

For a first prototype:

1. If the import starts with `./`, `../`, or `/`, treat it as a file path.
2. Otherwise, treat it as a package import.
3. Package exports are resolved through the package manifest's `exports` table.
4. The resolved export points to a `.nari` or `.naric` file in the global store.

This gives a clean rule:

- path-like string => file import
- bare specifier => package import

For the language-level syntax, resolution should work like this:

1. Resolve the string specifier to either a file module or package module.
2. Load and execute the target module once.
3. Construct the module namespace from explicit exports.
4. Bind either:
  - the whole namespace for `import name from "specifier";`
  - selected bindings for `import { a, b as c } from "specifier";`

This is a better long-term fit than the current global-export behavior.

## Dependency Strategy

### MVP Recommendation: Flat Graph

If package management lands before the full import/export overhaul, the current module system still has these limitations:

- no selective imports
- no aliases
- global export leakage

So the first package manager should intentionally enforce:

- **at most one resolved version per package name in an application graph**

This is a major simplification, but it matches the current language better.

If two packages require incompatible versions of the same dependency, installation should fail with a clear resolution error.

That is much better than pretending side-by-side versions work before module isolation exists.

### Later Upgrade Path

After Nari gains namespaced exports or proper module objects, the resolver can support multiple versions in one graph by resolving dependencies relative to the importing package.

In other words:

- **phase 1**: flat graph, shared store, lockfile, basic package resolution
- **phase 2**: explicit exports, namespace objects, named imports, eventually more flexible multi-version resolution

## Registry Format

A simple TOML index is enough for the prototype, but each package version now points
to a signed `.tar.gz` instead of a loose file tree.

Example:

```toml
registryVersion = 1

[packages."std/json"."1.4.0"]
url = "https://packages.nari-lang.org/archives/std-json-1.4.0.tar.gz"
integrity = "sha256-a1b2c3d4"
signatureUrl = "https://packages.nari-lang.org/archives/std-json-1.4.0.tar.gz.sig"
publicKeyUrl = "https://packages.nari-lang.org/keys/std-json-signing.pem"
```

## Formal Schema: Registry Index

Registry indexes can stay intentionally small in the MVP.

### Top-Level Keys

| Key | Type | Required | Notes |
| --- | --- | --- | --- |
| `registryVersion` | integer | yes | Start with `1`. |
| `packages` | table | yes | Package/version metadata. |

### Version Entry Schema

Each version entry lives at:

```toml
[packages."<package-name>"."<version>"]
```

Required fields:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `url` | string | yes | Archive URL for the hosted `.tar.gz` package. |
| `integrity` | string | yes | Archive integrity hash. The prototype uses `sha256-<hex>`. |
| `signatureUrl` | string | yes | Detached signature for the archive. |
| `publicKeyUrl` | string | yes | PEM-encoded public key used to verify the detached signature. |

Optional future fields:

- `publishedAt`
- `yanked`
- `size`
- `signingKeyId`
- mirrors

Archive layout rules for the current prototype:

- the extracted archive must contain `nari.toml` at its root, or inside a single top-level directory
- installers verify integrity before extraction
- installers verify the detached signature before copying files into the store
- current prototype implementation shells out to `sha256sum`, `openssl`, and `tar`

### Registry Example

```toml
registryVersion = 1

[packages."std/json"."1.4.0"]
url = "https://packages.nari-lang.org/archives/std-json-1.4.0.tar.gz"
integrity = "sha256-abc12345"
signatureUrl = "https://packages.nari-lang.org/archives/std-json-1.4.0.tar.gz.sig"
publicKeyUrl = "https://packages.nari-lang.org/keys/std-json-signing.pem"

[packages."std/json"."1.4.1"]
url = "https://packages.nari-lang.org/archives/std-json-1.4.1.tar.gz"
integrity = "sha256-def45678"
signatureUrl = "https://packages.nari-lang.org/archives/std-json-1.4.1.tar.gz.sig"
publicKeyUrl = "https://packages.nari-lang.org/keys/std-json-signing.pem"
```

## Validation Summary

For the MVP, tooling should reject manifests or lockfiles when any of the following is true:

- required keys are missing
- unknown top-level schema versions are used
- a dependency uses both `version` and `path`
- a package import cannot be matched through `exports`
- an export path escapes the package root
- two dependencies resolve to incompatible versions of the same package name
- the lockfile disagrees with the root manifest after resolution

This can evolve later into key rotation, signed registry metadata, mirrors, and incremental indexes.

## Project Workflow

Suggested commands:

```text
nari pkg init
nari pkg add std/json
nari pkg install
nari pkg update
nari pkg clean
```

Behavior:

- `init`: create `nari.toml`
- `add`: update manifest dependency ranges
- `install`: resolve graph, fetch packages, write lockfile
- `update`: refresh selected dependency versions
- `clean`: remove unused cached artifacts

## Compiler / Runtime Integration

The compiler should:

1. load `nari.lock.toml` from the current project root
2. resolve every bare package import to a store path
3. parse `.nari` packages normally
4. load `.naric` packages directly when available
5. optionally populate an ABI-scoped compiled cache

This fits the current source-import model reasonably well.

## Why This Avoids Node and Python Problems

### Avoids Node-style duplication

- packages live once in a global content-addressed store
- projects only reference packages through the lockfile
- no giant per-project dependency trees

### Avoids Python-style environment sprawl

- no full interpreter copy per project
- no mandatory virtual environment directory per app
- project isolation comes from the lockfile, not from copying everything

## Recommended MVP Decisions

If Nari implements package management soon, the simplest good prototype is:

- `nari.toml` for the manifest
- `nari.lock.toml` for exact resolution
- bare package imports like `import "std/json";`
- one global immutable package store
- one ABI-scoped compiled cache
- flat dependency graph with one version per package name

## Concrete Recommendation

A slightly revised form of the original idea is likely the best starting point:

```text
~/.nari/store/pkg/<name>@<version>-<short_content_hash>/
~/.nari/abi/<abi-version>/build/<name>@<version>-<short_content_hash>/
```

Not:

```text
~/.nari/<interpreter-version>/packages/name-<small_hash>/
```

The first form deduplicates better and keeps package storage stable across interpreter updates.

## Suggested Next Step

Before implementing networking or a registry, add local-manifest support only:

1. support `nari.toml`
2. support `nari.lock.toml`
3. support bare package imports resolved from a local cache/store
4. support path dependencies
5. enforce flat dependency resolution

That would be enough to validate the package model before committing to a full ecosystem design.

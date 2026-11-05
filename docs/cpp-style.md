# C++ style and encapsulation

Selected **2026-11-03**. Follow the
[ESPHome C++ conventions](https://developers.esphome.io/contributing/code/#c-code-style),
derived from the Google guide, with the project-specific visibility rule below.
Formatting conventions do not replace the ownership and composition contracts
in [the design](design.md).

## Required conventions

- Use `lower_snake_case` for functions, methods and variables;
  `UpperCamelCase` for types; `UPPER_SNAKE_CASE` for constants and enumerators;
  and trailing underscores for class fields.
- Write `this->field_` and `this->method()` for non-static members of the current
  instance, including inherited methods. Access another instance through its
  own reference/pointer. Static members are not instance accesses. Constructor
  initializer targets keep their normal syntax: `field_(value)`.
- Keep class state **private**. **No protected data members**, including static
  fields and fields injected by macros. Prefer composition and narrow behavior
  methods over exposing state to subclasses. Do not add getters returning
  mutable references solely to evade this rule. Protected behavior methods may
  be appropriate for an actual framework extension point; they do not grant
  permission for protected state.
- Plain value records may use public struct fields. Test doubles may expose
  explicit observation/control data to their tests, but may not use protected
  fields. This does not relax encapsulation in production services.
- Use two-space indentation, a 120-column limit and the checked-in
  `.clang-format`. Retain `.cpp`, `#pragma once` and existing framework override
  names. The portable host baseline remains C++17; firmware follows the pinned
  ESPHome toolchain. This is not a language-version migration.

`SendChannelMessageAction` uses the same `TemplatableStorage` type and setters as
ESPHome's `TEMPLATABLE_VALUE`, but declares its storage privately: that upstream
macro injects protected fields. No new schema or action API is introduced.

These rules cover project-owned C++ in `components/` and `tests/`. Keep pinned
vendored Ed25519 and other third-party/generated code unchanged. Formatting YAML
lambda bodies and checking subjective naming/design quality remain review work.

A shortened declaration from the scheduler illustrates private fields, explicit
instance access, and the constructor initializer syntax:

```cpp
class RadioScheduler {
 public:
  explicit RadioScheduler(AsyncPacketRadio &radio) : radio_(radio) {}
  size_t outstanding() const { return this->count_; }
  // Other scheduler operations are omitted here.

 private:
  AsyncPacketRadio &radio_;
  size_t count_{};
};
```

Calls to the current instance's methods also need qualification, for example
`this->finish_(now_ms, result, event)`. A constructor initializer such as
`radio_(radio)` initializes a member and does not take `this->`.

## What is enforced automatically

| Convention | Enforcement | Automatic fix |
| --- | --- | --- |
| Whitespace, indentation and wrapping | Pinned `clang-format` and `.clang-format`, for every owned C++ source/header | Yes |
| Explicit `this->` for non-static fields and methods | Clang semantic rule `explicit-this`, in the reported analysis profile | Yes, where a safe source location is available |
| No protected fields, including static and macro-generated fields | Clang semantic rule `protected-field` | Manual: choose private storage and an appropriate behavior interface |
| Private class state in production components | Clang semantic rule `private-class-state`; plain structs and test observation/control fields follow the exceptions above | Manual |
| Naming, meaningful interfaces, composition and access-policy design | Code review; these are not checked by the current script | Manual |

The checked-in policy applies even when a rule needs review. A passing formatter
alone does not establish compliance with the visibility or instance-access rules.
Review also determines whether a struct is a plain value record: the checker
does not infer service responsibilities from a type's name or public fields.

## Local checks

Use LLVM **21.1.8**, including its Python bindings and libclang, plus the existing
host-test dependencies (Clang and OpenSSL). On the checked macOS environment,
LLVM is the Homebrew `llvm@21` package and OpenSSL is `openssl@3`. The script
finds those prefixes through Homebrew; `LLVM_PREFIX` and `OPENSSL_PREFIX` can
select explicit installations. It rejects a different formatter version.

Run commands from the standalone checkout root, the directory containing this
repository's `README.md`, `components/` and `scripts/`.

Check without editing C++ sources:

```sh
python3 scripts/cpp_style.py
```

Apply supported fixes, then inspect the diff:

```sh
python3 scripts/cpp_style.py --fix
git diff
```

Run the checker regressions and existing host behavior tests:

```sh
python3 tests/test_cpp_style.py
sh tests/run_host_tests.sh
```

The first command checks formatting for **every owned source/header** and
reports the semantic coverage of the host profile. `--fix` inserts `this->` at
resolved member-reference locations and formats owned files; it does not change
visibility or rename APIs. It rechecks its result. Parse errors fail the command
before fixes; macro-generated expressions without a safe source location require
a manual edit. To check formatting alone or fix formatting alone:

```sh
python3 scripts/cpp_style.py --format-only
python3 scripts/cpp_style.py --format-only --fix
```

The command exits with **0** when the selected checks pass and a **nonzero**
status for violations or tool/parse failures. Semantic diagnostics include a
file, line, column, rule name and member name. Fix `protected-field` and
`private-class-state` findings by reviewing ownership and access; do not change
them to public fields or add mutable-reference getters simply to silence a
diagnostic. Rerun the check after manual edits. Formatting and semantic checks
can create ignored helper files under `build/cpp-style/`.

The semantic checker uses Clang's syntax tree, not identifier-name heuristics.
It detects implicit non-static field/method access, protected fields (including
macro expansions), and public class state in production components. It ignores
third-party declarations. The host profile currently covers the portable code,
host-testable adapters and test doubles; it does **not** stand in for the actual
ESPHome/ESP-IDF headers or cover every platform-only adapter.

To add platform-adapter analysis, pass an existing ESPHome build directory with
`build/compile_commands.json`, its generated `src/` headers and installed target
toolchain. For example, the existing private V3 build was inspected with:

```sh
python3 scripts/cpp_style.py --firmware-build local/.esphome/build/meshcore-heltec-v3
```

Replace the example path with the build directory for the configuration being
changed. Combine `--fix` and `--firmware-build` to apply supported fixes in that
profile. Inspect the printed `Semantic coverage` and `Not analyzed` lines;
source files outside the selected profile need their own analysis or review.
After the 2026-11-04 application extraction, the combined profile covers 54/54
library files. Consumer adapters are checked in their own project. Coverage is
reported per run, not assumed from a historical result.

The checker uses owned sources through an include overlay, not stale generated
copies. Its ESP32 analysis uses a 32-bit Clang surrogate and real target headers,
following [ESPHome's clang-tidy setup](https://github.com/fortuna/esphome/blob/9b0ea38449bd188d4e2a30e4b4100b43a3048b9d/script/clang-tidy).
A consumer that owns radio-probe tooling can use the same checker/profile.
It is not a target build or proof of RF behavior. The selected firmware only
exposes the configured preprocessor branches; uninstantiated dependent templates
and other build variants still need review or their own build checks. A stale
generated SDK/driver tree must be regenerated.

## Enforcing changes before merge

Run `python3 scripts/cpp_style.py` before submitting C++ changes. When platform
adapters or their headers change, also run the appropriate firmware-build
profile and actual firmware build. Review the conventions marked as manual in
the table above.

The checked-in [GitHub workflow](../.github/workflows/cpp-style.yml),
**C++ style and host tests**, runs on pushes and pull requests. Its `cpp-style`
job runs the default style check, checker regressions and sanitizer host suites.
It uses check mode, so violations fail the job rather than editing submitted
sources. Full firmware adapter analysis is currently a local check.

To make that CI result a merge requirement when the repository is published:

1. Include the workflow and style configuration in the repository and let the
   workflow run once so its check is available for selection.
2. Configure the target branch's protection or ruleset to require a pull request
   and the successful `cpp-style` job from this workflow before merging.
3. Keep platform-profile evidence and manual style review in the pull request
   when the default host profile does not cover the changed code.

No hosted run or required branch-protection rule has been established here.
This tooling does not install a local Git hook; contributors run the local
commands explicitly. Keep LLVM upgrades deliberate: update the expected version in
`scripts/cpp_style.py`, validate formatting/fixes and the checker regressions,
and update the setup instructions and CI together.

## Troubleshooting

- **Formatter version mismatch:** use LLVM 21.1.8, or select that installation
  with `LLVM_PREFIX`. Avoid changing the expected version only to bypass the
  check; another formatter version can produce a different diff.
- **Missing bindings or libclang:** install the matching LLVM Python bindings
  and shared library. A standalone `clang-format` executable is sufficient only
  for `--format-only`.
- **Missing headers or semantic parse failure:** check the Clang/OpenSSL host
  setup, or regenerate the selected ESPHome build and its compilation database.
  The checker stops before applying semantic fixes when parsing fails.
- **An unfixed `explicit-this` diagnostic:** inspect the reported expression and
  macro expansion. The fixer leaves expressions alone when it cannot identify a
  safe insertion point; resolve those accesses manually and rerun the check.

## Provenance and evidence

The formatting configuration was copied from `fortuna/esphome` commit
`9b0ea38449bd188d4e2a30e4b4100b43a3048b9d`, inspected **2026-11-03**. This records
the formatter source only; it does not change the project's ESPHome dependency
pin. The selected policy explicitly overrides upstream's preference for
protected fields. See [the project log](../LOG.md) for measured check results
and any unrelated working-tree blockers to firmware validation.

#!/usr/bin/env bash
# Lint script para o projeto DrawpileITI
# Executa clippy (Rust) e clang-tidy (C++/C) com as configurações do projeto.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

RUST_DIR="src/drawdance/rust"
BUILD_DIR="build"

echo "=== Clippy (Rust) ==="
if command -v cargo >/dev/null 2>&1; then
    (cd "$RUST_DIR" && cargo clippy --workspace --all-targets -- \
        -A clippy::not_unsafe_ptr_arg_deref \
        -A clippy::missing_safety_doc \
        -A clippy::too_many_arguments \
        -W clippy::empty_structs_with_brackets \
        -W clippy::shadow_reuse \
        -W clippy::shadow_same \
        -W clippy::string_to_string \
        -W clippy::try_err \
        -W clippy::unseparated_literal_suffix \
        -W clippy::verbose_file_reads \
        -W clippy::pedantic \
        -A clippy::case_sensitive_file_extension_comparisons \
        -A clippy::cast_possible_truncation \
        -A clippy::cast_possible_wrap \
        -A clippy::cast_precision_loss \
        -A clippy::cast_sign_loss \
        -A clippy::missing_panics_doc \
        -A clippy::module_name_repetitions \
        -A clippy::needless_pass_by_value \
        -A clippy::many_single_char_names \
        -A clippy::missing_errors_doc \
        -A clippy::must_use_candidate \
        -A clippy::similar_names \
        -A clippy::too_many_lines \
        -A clippy::uninlined_format_args \
        -A clippy::unreadable_literal)
else
    echo "AVISO: cargo não encontrado, pulando clippy."
fi

echo ""
echo "=== Clang-Tidy (C/C++) ==="
if command -v clang-tidy >/dev/null 2>&1 && [ -d "$BUILD_DIR" ]; then
    compile_commands="$BUILD_DIR/compile_commands.json"
    if [ -f "$compile_commands" ]; then
        find src -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' | \
            while read -r file; do
                clang-tidy -p "$BUILD_DIR" "$file" || true
            done
    else
        echo "AVISO: compile_commands.json não encontrado em $BUILD_DIR. Rode o CMake com -DCMAKE_EXPORT_COMPILE_COMMANDS=ON primeiro."
    fi
else
    echo "AVISO: clang-tidy ou build dir não encontrados, pulando clang-tidy."
fi

echo ""
echo "=== Concluído ==="

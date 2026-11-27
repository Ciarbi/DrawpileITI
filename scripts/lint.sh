#!/usr/bin/env bash
# Lint script para o projeto DrawpileITI
# Executa clippy (Rust) e clang-tidy (C++/C) com as configurações do projeto.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

RUST_DIR="src/drawdance/rust"
if [ -n "${1:-}" ]; then
    BUILD_DIR="$1"
elif [ -f build-check/compile_commands.json ]; then
    BUILD_DIR="build-check"
else
    BUILD_DIR="build"
fi

echo "=== Clippy (Rust) ==="
if command -v cargo >/dev/null 2>&1; then
    (
        cd "$RUST_DIR"
        cargo clippy --workspace --all-targets -- \
            -A clippy::not_unsafe_ptr_arg_deref \
            -A clippy::missing_safety_doc \
            -A clippy::too_many_arguments \
            -W clippy::empty_structs_with_brackets \
            -W clippy::shadow_reuse \
            -W clippy::shadow_same \
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
            -A clippy::unreadable_literal
    )
else
    echo "AVISO: cargo não encontrado, pulando clippy."
fi

echo ""
echo "=== Clang-Tidy (C/C++) ==="
if command -v clang-tidy >/dev/null 2>&1 && [ -d "$BUILD_DIR" ]; then
    compile_commands="$BUILD_DIR/compile_commands.json"
    if [ -f "$compile_commands" ]; then
        # clang-tidy usa uma versão de clang mais antiga que o GCC que gerou
        # o build; remove a flag incompatível sem alterar o build original.
        tidy_dir="$BUILD_DIR/.clang-tidy"
        mkdir -p "$tidy_dir"
        sed 's/ -mno-direct-extern-access//g' \
            "$compile_commands" > "$tidy_dir/compile_commands.json"
        find src -type f \( -name '*.cpp' -o -name '*.c' \) -print0 |
            while IFS= read -r -d '' file; do
                clang-tidy -p "$tidy_dir" "$file" || true
            done
    else
        echo "AVISO: compile_commands.json não encontrado em $BUILD_DIR. Rode o CMake com -DCMAKE_EXPORT_COMPILE_COMMANDS=ON primeiro."
    fi
else
    echo "AVISO: clang-tidy ou build dir não encontrados, pulando clang-tidy."
fi

echo ""
echo "=== Concluído ==="

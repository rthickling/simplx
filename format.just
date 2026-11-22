# Format C/C++ sources with clang-format (repo .clang-format).
format:
    #!/usr/bin/env bash
    set -euo pipefail
    mapfile -d '' files < <(
        git ls-files -z -- \
            '*.h' '*.hpp' '*.hh' '*.hxx' \
            '*.c' '*.cc' '*.cpp' '*.cxx' \
            ':!:thirdparty/**' ':!:**/build/**'
    )
    if ((${#files[@]} == 0)); then
        echo "No C/C++ sources to format." >&2
        exit 0
    fi
    clang-format -i -- "${files[@]}"
    echo "Formatted ${#files[@]} file(s)."

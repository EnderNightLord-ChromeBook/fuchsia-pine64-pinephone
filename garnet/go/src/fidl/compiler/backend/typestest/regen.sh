#!/usr/bin/env bash
set -e

if [ ! -x "${FUCHSIA_BUILD_DIR}" ]; then
    echo "error: did you fx exec? missing \$FUCHSIA_BUILD_DIR" 1>&2
    exit 1
fi

FIDLC="${FUCHSIA_BUILD_DIR}/host_x64/fidlc"
if [ ! -x "${FIDLC}" ]; then
    echo "error: fidlc missing; did you fx clean-build?" 1>&2
    exit 1
fi

FIDLGEN="${FUCHSIA_BUILD_DIR}/host_x64/fidlgen"
if [ ! -x "${FIDLGEN}" ]; then
    echo "error: fidlgen missing; maybe fx clean-build?" 1>&2
    exit 1
fi
FIDLGEN_LLCPP="${FUCHSIA_BUILD_DIR}/host_x64/fidlgen_llcpp"
if [ ! -x "${FIDLGEN_LLCPP}" ]; then
    echo "error: fidlgen_llcpp missing; maybe fx clean-build?" 1>&2
    exit 1
fi

EXAMPLE_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
EXAMPLE_DIR="$( echo $EXAMPLE_DIR | sed -e "s+${FUCHSIA_DIR}/++" )"
GOLDENS_DIR="${EXAMPLE_DIR}/../goldens"
GOLDENS=()
cd "${FUCHSIA_DIR}"
for src_path in `find "${EXAMPLE_DIR}" -name '*.fidl'`; do
    src_name="$( basename "${src_path}" )"
    json_name=${src_name}.json
    cpp_header_name=${json_name}.h
    cpp_test_header_name=${json_name}_test_base.h
    cpp_source_name=${json_name}.cc
    llcpp_header_name=${json_name}.llcpp.h
    llcpp_source_name=${json_name}.llcpp.cc
    overnet_internal_header_name=${json_name}.overnet_internal.h
    overnet_internal_source_name=${json_name}.overnet_internal.cc
    overnet_embedded_header_name=${json_name}.overnet_embedded.h
    overnet_embedded_source_name=${json_name}.overnet_embedded.cc
    go_impl_name=${json_name}.go
    rust_name=${json_name}.rs
    syzkaller_name=${json_name}.syz.txt

    GOLDENS+=(
      $json_name,
      "${cpp_header_name}.golden",
      "${cpp_test_header_name}.golden",
      "${cpp_source_name}.golden",
      "${llcpp_header_name}.golden",
      "${llcpp_source_name}.golden",
      "${overnet_internal_header_name}.golden",
      "${overnet_internal_source_name}.golden",
      "${go_impl_name}.golden",
      "${rust_name}.golden",
      "${syzkaller_name}.golden",
    )

    echo -e "\033[1mexample: ${src_name}\033[0m"

    echo "  json ir: ${src_name} > ${json_name}"
    ${FIDLC} \
        --json "${GOLDENS_DIR}/${json_name}" \
        --files "${EXAMPLE_DIR}/${src_name}"

    echo "  cpp: ${json_name} > ${cpp_header_name}, ${cpp_source_name}, and ${cpp_test_header_name}"
    ${FIDLGEN} \
        -generators cpp \
        -json "${GOLDENS_DIR}/${json_name}" \
        -output-base "${GOLDENS_DIR}/${json_name}" \
        -include-base "${GOLDENS_DIR}"
    mv "${GOLDENS_DIR}/${cpp_header_name}" "${GOLDENS_DIR}/${cpp_header_name}.golden"
    mv "${GOLDENS_DIR}/${cpp_source_name}" "${GOLDENS_DIR}/${cpp_source_name}.golden"
    mv "${GOLDENS_DIR}/${cpp_test_header_name}" "${GOLDENS_DIR}/${cpp_test_header_name}.golden"

    echo "  llcpp: ${json_name} > ${llcpp_header_name} and ${llcpp_source_name}"
    ${FIDLGEN_LLCPP} \
        -json "${GOLDENS_DIR}/${json_name}" \
        -header "${GOLDENS_DIR}/${llcpp_header_name}" \
        -source "${GOLDENS_DIR}/${llcpp_source_name}" \
        -include-base "${GOLDENS_DIR}"
    mv "${GOLDENS_DIR}/${llcpp_header_name}" "${GOLDENS_DIR}/${llcpp_header_name}.golden"
    mv "${GOLDENS_DIR}/${llcpp_source_name}" "${GOLDENS_DIR}/${llcpp_source_name}.golden"

    echo "  overnet_internal: ${json_name} > ${overnet_internal_header_name} and ${overnet_internal_source_name}"
    ${FIDLGEN} \
        -generators overnet_internal \
        -json "${GOLDENS_DIR}/${json_name}" \
        -output-base "${GOLDENS_DIR}/${json_name}.overnet_internal" \
        -include-base "${GOLDENS_DIR}"
    mv "${GOLDENS_DIR}/${overnet_internal_header_name}" "${GOLDENS_DIR}/${overnet_internal_header_name}.golden"
    mv "${GOLDENS_DIR}/${overnet_internal_source_name}" "${GOLDENS_DIR}/${overnet_internal_source_name}.golden"

    echo "  overnet_embedded: ${json_name} > ${overnet_embedded_header_name} and ${overnet_embedded_source_name}"
    ${FIDLGEN} \
        -generators overnet_embedded \
        -json "${GOLDENS_DIR}/${json_name}" \
        -output-base "${GOLDENS_DIR}/${json_name}.overnet_embedded" \
        -include-base "${GOLDENS_DIR}"
    mv "${GOLDENS_DIR}/${overnet_embedded_header_name}" "${GOLDENS_DIR}/${overnet_embedded_header_name}.golden"
    mv "${GOLDENS_DIR}/${overnet_embedded_source_name}" "${GOLDENS_DIR}/${overnet_embedded_source_name}.golden"

    echo "  go: ${json_name} > ${go_impl_name}"
    ${FIDLGEN} \
        -generators go \
        -json "${GOLDENS_DIR}/${json_name}" \
        -output-base "${GOLDENS_DIR}" \
        -include-base "${GOLDENS_DIR}"
    rm "${GOLDENS_DIR}/pkg_name"
    mv "${GOLDENS_DIR}/impl.go" "${GOLDENS_DIR}/${go_impl_name}.golden"

    echo "  rust: ${json_name} > ${rust_name}"
    ${FIDLGEN} \
        -generators rust \
        -json "${GOLDENS_DIR}/${json_name}" \
        -output-base "${GOLDENS_DIR}/${json_name}" \
        -include-base "${GOLDENS_DIR}"
    mv "${GOLDENS_DIR}/${rust_name}" "${GOLDENS_DIR}/${rust_name}.golden"

    echo "  syzkaller: ${json_name} > ${syzkaller_name}"
    ${FIDLGEN} \
        -generators syzkaller \
        -json "${GOLDENS_DIR}/${json_name}" \
        -output-base "${GOLDENS_DIR}/${json_name}" \
        -include-base "${GOLDENS_DIR}"
    mv "${GOLDENS_DIR}/${syzkaller_name}" "${GOLDENS_DIR}/${syzkaller_name}.golden"
done

> "${GOLDENS_DIR}/goldens.txt"
printf "%s\n" "${GOLDENS[@]//,}" | sort >> "${GOLDENS_DIR}/goldens.txt"

> "${GOLDENS_DIR}/OWNERS"
find "${EXAMPLE_DIR}/../.."  -name 'OWNERS' -exec cat {} \; | sort -u > "${GOLDENS_DIR}/OWNERS"

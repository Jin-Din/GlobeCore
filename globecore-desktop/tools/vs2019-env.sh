# shellcheck shell=bash
# Imports the VS2019 x64 (MSVC) environment into the current bash session.
# INCLUDE/LIB stay native Windows form (for cl/link); PATH is unix-ified.
# Self-contained: no project path dependencies, safe to source from anywhere.
vcvars_win2unix() {
  local out="" part drive rest
  local IFS=';'
  for part in $1; do
    [ -z "$part" ] && continue
    part=${part//\//}
    case "$part" in
      [A-Za-z]:*) drive=${part:0:1}; drive=${drive,,}; part="/$drive${part:2}" ;;
    esac
    out+="$part:"
  done
  printf '%s' "${out%:}"
}
vcvars_import() {
  local vars winpath
  vars=$(cmd <<< '"C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" && set' 2>/dev/null | tr -d '\r' | grep -E '^(PATH|INCLUDE|LIB|LIBPATH)=')
  if [ -z "$vars" ]; then echo "vcvars_import: failed" >&2; return 1; fi
  while IFS='=' read -r k v; do
    case "$k" in
      PATH) winpath="$v" ;;
      *)    export "$k=$v" ;;
    esac
  done <<< "$vars"
  PATH="$(vcvars_win2unix "$winpath"):$PATH"; export PATH
}

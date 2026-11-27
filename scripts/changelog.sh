#!/usr/bin/env bash
# Changelog section from the commit messages since the last release tag.
#   scripts/changelog.sh <previous tag or ""> <new version>
# Commits are grouped by their prefix (Conventional Commits): feat: -> Added,
# fix: -> Fixed, everything else -> Changed. Merges and release commits are left out.
set -euo pipefail
prev="${1:-}"
version="${2:?version}"
range="${prev:+$prev..}HEAD"

added="" fixed="" changed=""
while IFS=$'\t' read -r hash subject; do
  [ -n "$subject" ] || continue
  case "$subject" in
    "Release v"*|Merge*) continue ;;
  esac
  # strip a "type(scope)!:" prefix for the text
  text=$(sed -E 's/^[a-zA-Z]+(\([^)]*\))?!?:[[:space:]]*//' <<< "$subject")
  text="$(tr '[:lower:]' '[:upper:]' <<< "${text:0:1}")${text:1}"
  line="- $text ($hash)"
  case "$subject" in
    feat*|add*|Add*)        added+="$line"$'\n' ;;
    fix*|Fix*)              fixed+="$line"$'\n' ;;
    *)                      changed+="$line"$'\n' ;;
  esac
done < <(git log --no-merges --reverse --format='%h%x09%s' "$range")

echo "## $version ($(date -u +%Y-%m-%d))"
echo
[ -n "$added" ]   && printf '### Added\n%s\n' "$added"
[ -n "$fixed" ]   && printf '### Fixed\n%s\n' "$fixed"
[ -n "$changed" ] && printf '### Changed\n%s\n' "$changed"
if [ -z "$added$fixed$changed" ]; then echo "- No changes listed."; echo; fi

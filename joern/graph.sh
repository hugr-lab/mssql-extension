#!/usr/bin/env bash
#
# Render one method's graph as an SVG -- the closest thing to a UI that the open
# source distribution has (see "Is there a UI?" in JOERN_README.md).
#
#   joern/graph.sh ParseError            # control flow (default)
#   joern/graph.sh ParseError ast        # abstract syntax tree
#   joern/graph.sh ParseError pdg        # program dependence (data + control)
#   joern/graph.sh ReadPLPType cfg all   # from the `all`-scope CPG
#
# Needs graphviz (`brew install graphviz`). Writes joern/out/<method>-<kind>.svg
# and opens it on macOS.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
METHOD="${1:?usage: joern/graph.sh <MethodName> [cfg|ast|pdg|cpg14] [tds|all]}"
KIND="${2:-cfg}"
SCOPE="${3:-tds}"
OUT_DIR="${JOERN_OUT:-$ROOT/joern/out}"
CPG="$OUT_DIR/$SCOPE.cpg.bin"

command -v dot >/dev/null 2>&1 || { echo "graphviz not found: brew install graphviz" >&2; exit 127; }
[ -f "$CPG" ] || { echo "no CPG at $CPG — run: joern/build.sh $SCOPE" >&2; exit 1; }

case "$KIND" in
cfg) STEP="dotCfg" ;;
ast) STEP="dotAst" ;;
pdg) STEP="dotPdg" ;;
cpg14) STEP="dotCpg14" ;;
*) echo "unknown kind '$KIND' (cfg|ast|pdg|cpg14)" >&2; exit 2 ;;
esac

DOT="$OUT_DIR/$METHOD-$KIND.dot"
SVG="$OUT_DIR/$METHOD-$KIND.svg"
SCRIPT="$OUT_DIR/.graph-$KIND.sc"

export JAVA_OPTS="${JAVA_OPTS:--Xmx${JOERN_HEAP:-8g}}"

# The DOT text comes from the REPL step of the same name; a method that the
# frontend saw more than once (a header and its definition) yields several
# graphs, so only the first is rendered.
cat > "$SCRIPT" <<EOF
@main def main(cpgFile: String, method: String, out: String) = {
	importCpg(cpgFile)
	val graphs = cpg.method.nameExact(method).$STEP.l
	if (graphs.isEmpty) println(s"no method named '\$method' in this CPG")
	else {
		java.nio.file.Files.write(java.nio.file.Paths.get(out), graphs.head.getBytes)
		println(s"\${graphs.size} graph(s) for '\$method'; wrote the first to \$out")
	}
}
EOF

cd "$OUT_DIR"
joern --script "$SCRIPT" --param "cpgFile=$CPG" --param "method=$METHOD" --param "out=$DOT"

[ -s "$DOT" ] || { echo "no graph produced for '$METHOD'" >&2; exit 1; }
dot -Tsvg "$DOT" -o "$SVG"
echo "svg: $SVG"
[ "$(uname)" = "Darwin" ] && open "$SVG" || true

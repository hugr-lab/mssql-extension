//
// Interactive helpers, loaded by `joern/shell.sh [tds|all]`, which starts the
// Joern REPL with the CPG already open and these definitions in scope.
//
// Top-level definitions on purpose: this file is passed to `joern --import`,
// which compiles it and puts it on the REPL's classpath. It must therefore NOT
// contain an @main method (that is what joern/scan.sc is, for `--script`).
//
// The saved query suite lives in joern/scan.sc; this file is for the questions
// that are worth asking once.
//
import io.shiftleft.codepropertygraph.generated.Operators
import io.shiftleft.codepropertygraph.generated.nodes.AstNode
import io.shiftleft.semanticcpg.language._
import io.joern.joerncli.console.Joern.cpg

def where(n: AstNode): String =
	s"${n.location.filename.replaceFirst(".*?(src/)", "$1")}:${n.lineNumber.map(_.toString).getOrElse("?")}"

/** The fuzz harness entry points (fuzz/README.md). */
def wireEntry = cpg.method.nameExact(
	"Feed", "TryParseNext", "ParseLoginResponse", "ParseBrowserResponse",
	"Utf16LEDecode", "LegacyUtf16LEDecode", "FindBeginTxnDescriptor", "StageRow", "StageNBCRow"
)

/** Methods that take a raw wire buffer. */
def parsers = cpg.method.internal.filter(_.parameter.exists(_.typeFullName.matches(".*(uint8_t|unsigned char)\\s*\\*.*")))

def copies = cpg.call.name("memcpy|memmove|memset|strncpy|snprintf")
def allocs = cpg.call.name("malloc|calloc|realloc|resize|reserve|assign")
def indexes = cpg.call.nameExact(Operators.indirectIndexAccess, Operators.indexAccess)
def casts = cpg.call.nameExact(Operators.cast)

def cheatsheet(): Unit = println("""
  wireEntry   the fuzz harness entry points
  parsers     methods that take a uint8_t* buffer
  copies      memcpy / memmove / memset calls
  allocs      malloc / resize / reserve / assign calls
  indexes     pointer index accesses
  casts       every cast
  where(n)    "src/file.cpp:123" for any node
  cheatsheet() this text again

  parsers.name.l
  copies.map(c => (where(c), c.code)).l
  copies.argument.reachableByFlows(wireEntry.parameter).p
  cpg.method.name("ParseRow").ast.isControlStructure.code.l
""")

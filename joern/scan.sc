//
// Joern query suite for the mssql-extension C++ sources.
//
// Run through joern/scan.sh, which loads the CPG built by joern/build.sh:
//   joern/scan.sh tds              # every query
//   joern/scan.sh tds q3,q5        # only those
//
// Findings are HINTS. Every one has to be read against the source: the wire
// buffer here is guarded by hand-written `Available() < n` checks whose
// correctness is arithmetic, not structural, and no query can model that. The
// suite's job is to keep an inventory of the places where such a guard has to
// exist, so that a new one that lacks it stands out when a report is diffed
// against the previous tree's.
//
// Two facts about c2cpg (the CDT frontend) shape every query below, both
// verified on this codebase's CPG:
//   * an expression's `typeFullName` is almost always `ANY` -- `3 + token_length`
//     carries no type -- but an IDENTIFIER's is accurate (`token_length:uint16_t`,
//     `data:uint8_t*`, `offset:size_t`). Every type test therefore keys on the
//     operands, never on the expression.
//   * a cast's `typeFullName` never carries a `*`, so a pointer cast is found by
//     its source text, not its type.
//
import io.shiftleft.codepropertygraph.generated.Operators
import io.shiftleft.codepropertygraph.generated.nodes.{AstNode, Expression, Method}

@main def main(cpgFile: String, only: String = "") = {
	importCpg(cpgFile)

	val selected: Set[String] =
		if (only.trim.isEmpty) Set.empty else only.split("[,; ]+").map(_.trim.toLowerCase).filter(_.nonEmpty).toSet
	def wanted(id: String) = selected.isEmpty || selected.contains(id)

	// ---------------------------------------------------------------- helpers

	def header(id: String, title: String, why: String): Unit = {
		println("")
		println(s"## $id — $title")
		println("")
		why.linesIterator.foreach(l => println(s"  $l"))
		println("")
	}

	// The `tds` scope's filenames carry the staging path, the `all` scope's are
	// relative to src/ itself, so both are normalised to "src/...".
	def loc(n: AstNode): String = {
		val raw = n.location.filename.replaceFirst(".*?(src/)", "$1")
		val f = if (raw.startsWith("src/") || raw.startsWith("/")) raw else s"src/$raw"
		s"$f:${n.lineNumber.map(_.toString).getOrElse("?")}"
	}

	def oneLine(s: String, max: Int = 96): String = {
		val t = s.replaceAll("\\s+", " ").trim
		if (t.length > max) t.take(max - 3) + "..." else t
	}

	// Method name as a human reads it: Class.Method(short param types).
	def sig(m: Method): String = {
		val cls = m.fullName.split(":").head.split("\\.").dropRight(1).lastOption.filter(_.head.isUpper)
		val params = m.parameter.filterNot(_.index == 0).typeFullName.l
			.map(_.split("\\.").last.replace("std.", ""))
			.mkString(",")
		s"${cls.map(_ + ".").getOrElse("")}${m.name}($params)"
	}

	def report(rows: List[(String, String)], cap: Int = 60): Unit = {
		if (rows.isEmpty) {
			println("    (none)")
		} else {
			rows.take(cap).foreach { case (where, what) => println(f"    $where%-50s $what") }
			if (rows.size > cap) println(s"    ... ${rows.size - cap} more (raise the cap in scan.sc to see them)")
			println("")
			println(s"    total: ${rows.size}")
		}
	}

	def byLocation(ns: List[AstNode]): List[AstNode] =
		ns.sortBy(n => (n.location.filename, n.lineNumber.map(_.toInt).getOrElse(0)))

	// A raw wire buffer: uint8_t* / unsigned char* / byte*. Plain `char*` is
	// excluded deliberately -- in this codebase that is a C string (identifier
	// names, SQL text), and including it drowns the parsing surface in catalog
	// and pushdown helpers.
	val bytePtr = ".*(uint8_t|unsigned char|std.byte|uint8_t const)\\s*\\*.*"
	val lengthName = "(?i).*(length|len|size|count|offset|total|remaining|avail|bytes|nbytes|width).*"
	val narrowInt = ".*(uint8_t|uint16_t|uint32_t|int8_t|int16_t|int32_t|unsigned short|unsigned int|short|^int)$"
	val memSizeFns = "memcpy|memmove|memset|strncpy|strncat|snprintf"

	// An operand whose declared type is known: an identifier, or a field access
	// whose type the frontend resolved.
	def typedOperands(c: Expression): List[(String, String)] =
		c.ast.isIdentifier.l.map(i => (i.name, i.typeFullName)) ++
			c.ast.isCall.nameExact(Operators.fieldAccess, Operators.indirectFieldAccess).l
				.filter(_.typeFullName != "ANY")
				.map(f => (f.code.split("[.>]").last, f.typeFullName))

	def touchesPointer(c: Expression): Boolean =
		c.ast.isIdentifier.exists(_.typeFullName.contains("*")) ||
			c.ast.isCall.nameExact(Operators.fieldAccess, Operators.indirectFieldAccess).exists(_.typeFullName.contains("*")) ||
			c.ast.isCall.name("data|c_str|begin|end").nonEmpty

	val parseMethods: List[Method] = cpg.method.internal
		.nameNot("<global>")
		.filter(m => m.parameter.exists(_.typeFullName.matches(bytePtr)) || m.ast.isCall.name("Current|Available|Peek").nonEmpty)
		.l

	val dataflowOk: Boolean =
		try {
			cpg.call.name(memSizeFns).argument.reachableByFlows(cpg.method.nameExact("Feed").parameter).size
			true
		} catch { case _: Throwable => false }

	println("# Joern scan — mssql-extension")
	println("")
	println(s"CPG: $cpgFile")
	println(s"files: ${cpg.file.size}   methods: ${cpg.method.internal.size}   calls: ${cpg.call.size}")
	println(s"dataflow engine: ${if (dataflowOk) "available" else "UNAVAILABLE — dataflow queries skipped"}")

	// ------------------------------------------------------------------- q1

	if (wanted("q1")) {
		header(
			"q1",
			"Wire-parsing surface inventory",
			"""Every method that takes a raw byte buffer (uint8_t*) or reads the TokenParser
			  |receive buffer. This is the set that must never trust a declared length.
			  |Diff it against the harness table in fuzz/README.md: a method here that no
			  |harness can reach is unfuzzed parsing code.""".stripMargin
		)
		report(byLocation(parseMethods).map(m => (loc(m), sig(m.asInstanceOf[Method]))), 100)
	}

	// ------------------------------------------------------------------- q2

	if (wanted("q2")) {
		header(
			"q2",
			"Byte-buffer method that indexes without comparing anything",
			"""A method that takes a byte buffer and indexes it (or memcpy's out of it) while
			  |containing no comparison operator at all. The guard may legitimately live in
			  |the caller, but the convention here is that the parsing method checks
			  |`Available()` / its own length parameter itself (TokenParser::ParseError is
			  |the model), so every hit is an exception to that convention.""".stripMargin
		)

		val rows = parseMethods
			.filter(m => m.parameter.exists(_.typeFullName.matches(bytePtr)))
			.filter { m =>
				m.ast.isCall.exists(c =>
					c.name == Operators.indirectIndexAccess || c.name == Operators.indexAccess || c.name.matches(memSizeFns)
				)
			}
			.filterNot { m =>
				m.ast.isCall.exists(c =>
					Set(
						Operators.lessThan, Operators.greaterThan, Operators.lessEqualsThan,
						Operators.greaterEqualsThan, Operators.equals, Operators.notEquals
					).contains(c.name) || c.name.matches("(?i)(Available|Remaining|Capacity)")
				)
			}
			.l
		report(byLocation(rows).map(m => (loc(m), sig(m.asInstanceOf[Method]))))
	}

	// ------------------------------------------------------------------- q3

	if (wanted("q3")) {
		header(
			"q3",
			"Length arithmetic that can lose information (the issue #185 class)",
			"""[wrap]  addition or multiplication of a length/size/offset declared as a
			  |        32-bit integer (uint32_t / unsigned int / int). `5 + token_length`
			  |        in uint32 wraps, and the wrapped sum then passes a bounds check it
			  |        should have failed -- the fuzz_tds_tokens zero-progress hang (issue
			  |        #185, PoC `EE FB FF FF FF`). Each hit should either be widened to
			  |        size_t or have its operands bounded before the arithmetic.
			  |[trunc] a length narrowed by a cast to uint8_t/uint16_t/uint32_t. The wire
			  |        field is 1, 2 or 4 bytes wide and a value that does not fit is
			  |        silently truncated on the way out (the issue #181 class, where a
			  |        varchar length was declared at 1x its byte count).
			  |
			  |Two C++ rules keep [wrap] honest, and both silence shapes an earlier cut of
			  |this query reported: integer PROMOTION widens a uint8_t/uint16_t operand to
			  |int before the arithmetic (so `1 + data_length` on a uint8_t cannot wrap --
			  |only its narrowing, under [trunc], can lose it), and the usual arithmetic
			  |CONVERSIONS make any expression with a size_t operand 64-bit (so `header +
			  |data_length` with header:size_t cannot wrap either). An operand already
			  |wrapped in a cast to size_t -- which is how #185 was FIXED -- is skipped for
			  |the same reason.
			  |
			  |The operand types that earned each hit are printed after the expression.""".stripMargin
		)

		val wrapWidth = ".*(uint32_t|unsigned int|int32_t|^int)$"
		val wide = ".*(size_t|uint64_t|idx_t|unsigned long|int64_t)$"

		// A string concatenation is also <operator>.addition in this frontend; an
		// error message built with to_string() is not length arithmetic.
		def isConcat(c: Expression): Boolean =
			c.ast.isLiteral.exists(_.code.startsWith("\"")) || c.ast.isCall.name("to_string|c_str|str|append").nonEmpty

		// An operand already widened by an enclosing cast cannot wrap: the fixed
		// form of the #185 bug reads `static_cast<size_t>(token_length) + 5` and
		// must not be reported as the bug it replaced.
		def alreadyWidened(id: io.shiftleft.codepropertygraph.generated.nodes.Identifier): Boolean =
			id.inAst.isCall.nameExact(Operators.cast).exists(_.code.matches(".*(size_t|uint64_t|idx_t|unsigned long|int64_t).*"))

		val wrapRows = cpg.call
			.nameExact(Operators.addition, Operators.multiplication)
			.filterNot(touchesPointer)
			.filterNot(isConcat)
			.l
			.flatMap { c =>
				// Usual arithmetic conversions: one 64-bit operand makes the whole
				// expression 64-bit, so `header + data_length` with header:size_t
				// cannot wrap however narrow data_length is.
				val has64 = typedOperands(c).exists { case (_, t) => t.matches(wide) }
				val culprits = c.ast.isIdentifier.l
					.filterNot(alreadyWidened)
					.filter(i => i.name.matches(lengthName) && i.typeFullName.matches(wrapWidth))
					.map(i => (i.name, i.typeFullName))
					.distinct
				if (has64 || culprits.isEmpty) None
				else
					Some(
						(loc(c), s"[wrap]  ${oneLine(c.code, 42)}   [${culprits.map { case (n, t) => s"$n:$t" }.mkString(", ")}]")
					)
			}

		// A cast's arguments are (1) the TYPE_REF, (2) the value. Only the value
		// matters: `static_cast<uint32_t>(data[offset])` narrows nothing -- it
		// widens one byte -- and reporting the size_t index inside it was noise.
		val truncRows = cpg.call
			.nameExact(Operators.cast)
			.code(".*(uint8_t|uint16_t|uint32_t|int8_t|int16_t|unsigned char|unsigned short)\\s*>.*")
			.l
			.flatMap { c =>
				val value = c.argument.argumentIndex(2).l
				val readsAByte = value.exists(v =>
					v.ast.isCall.nameExact(Operators.indirectIndexAccess, Operators.indexAccess).nonEmpty
				)
				val widened = value
					.flatMap(v => typedOperands(v))
					.filter { case (n, t) => n.matches(lengthName) && t.matches(wide) }
					.distinct
				if (readsAByte || widened.isEmpty) None
				else
					Some(
						(loc(c), s"[trunc] ${oneLine(c.code, 42)}   [${widened.map { case (n, t) => s"$n:$t" }.mkString(", ")}]")
					)
			}

		report((wrapRows ++ truncRows).distinct.sortBy(_._1), 80)
	}

	// ------------------------------------------------------------------- q4

	if (wanted("q4")) {
		header(
			"q4",
			"Allocation or append sized by a declared length",
			"""resize/reserve/assign/insert/malloc whose size argument carries a length
			  |variable. A server-declared 4 GB length reaching an allocation is a remote
			  |OOM even when nothing is written out of bounds. Constant-sized allocations
			  |(STANDARD_VECTOR_SIZE and friends) are filtered out.""".stripMargin
		)

		val sizedByLength = cpg.call
			.name("malloc|calloc|realloc|resize|reserve|assign|insert|append")
			.l
			.filter { c =>
				c.argument.l.exists { a =>
					typedOperands(a).exists { case (n, t) => n.matches(lengthName) && !t.contains("*") } &&
						!a.code.matches("^[A-Z_0-9]+$")
				}
			}
		report(byLocation(sizedByLength).map(c => (loc(c), oneLine(c.code))), 80)
	}

	// ------------------------------------------------------------------- q5

	if (wanted("q5")) {
		header(
			"q5",
			"[dataflow] Fuzz entry-point parameter reaching a copy size or an index",
			"""Taint from the fuzz harness entry points (Feed, TryParseNext,
			  |ParseLoginResponse, ParseBrowserResponse, Utf16LEDecode,
			  |FindBeginTxnDescriptor) to a memcpy size or an array index. Joern's OSS
			  |engine is conservative -- an argument taints the result of a call it cannot
			  |see into -- so a flow here is a candidate fuzz seed, never a proven bug.""".stripMargin
		)

		if (!dataflowOk) println("    (skipped: dataflow engine unavailable)")
		else {
			val entry = cpg.method
				.nameExact("Feed", "TryParseNext", "ParseLoginResponse", "ParseBrowserResponse", "Utf16LEDecode", "LegacyUtf16LEDecode", "FindBeginTxnDescriptor")
				.parameter
				.l

			val sinks =
				cpg.call.name(memSizeFns).argument.argumentIndex(3).l ++
					cpg.call.nameExact(Operators.indirectIndexAccess).argument.argumentIndex(2).l

			val rows = sinks
				.reachableByFlows(entry)
				.l
				.map { f =>
					val src = f.elements.head
					val snk = f.elements.last
					(loc(snk), s"${oneLine(snk.code, 46)}   <- ${oneLine(src.code, 24)} @ ${loc(src)}")
				}
				.distinct
			report(rows, 50)
		}
	}

	// ------------------------------------------------------------------- q6

	if (wanted("q6")) {
		header(
			"q6",
			"Credential inside a log, exception or format expression",
			"""Spec 047 T028a (the ATTACH error path must never echo a password) and the
			  |mssql_pool_stats redaction gate, as a query: a password/secret/token/keytab
			  |identifier appearing anywhere inside a message expression. Structural on
			  |purpose -- the dataflow form was tried and is unusable here: Joern taints a
			  |call's result with its arguments, so every debug fprintf downstream of
			  |DoLogin7 reports as reached by `password` (28 flows, all false). Run this one
			  |in the `all` scope; credentials live outside src/tds/.""".stripMargin
		)

		val secretish = "(?i).*(password|passwd|pwd|secret|client_secret|access_token|refresh_token|keytab|credential|connection_string|dsn).*"
		// `secret_name`, `secret_type`, `token_id`: an identifier OF a credential,
		// not the credential. Logging those is what the extension's error messages
		// are supposed to do ("Secret 'x' not found"), and leaving them in buries
		// the one finding that would matter.
		val identifierOfSecret = "(?i).*(_name|_type|_id|_key_name|name|type)$"
		val messageSinks = cpg.call
			.name("printf|fprintf|sprintf|snprintf|puts|fputs|Log|Logger|.*Exception|.*Error|Throw.*|StringUtil.*|Format|format|to_string|str")
			.l
			.filter { c =>
				c.argument.l.exists { a =>
					a.ast.isIdentifier.name(secretish).nameNot(identifierOfSecret).nonEmpty ||
						a.ast.isCall
							.nameExact(Operators.fieldAccess, Operators.indirectFieldAccess)
							.code(secretish)
							.codeNot(identifierOfSecret)
							.nonEmpty
				}
			}
		report(byLocation(messageSinks).map(c => (loc(c), oneLine(c.code))), 60)
	}

	// ------------------------------------------------------------------- q7

	if (wanted("q7")) {
		header(
			"q7",
			"Socket / TLS call whose return value is discarded",
			"""recv/send/poll/SSL_* return a count or an error that has to be read. A call in
			  |statement position is either a bug or a deliberate best-effort call on a
			  |teardown path -- the teardown chain is noexcept by spec 047, so close() and
			  |SSL_shutdown() hits are expected; a discarded poll() or recv() is not.""".stripMargin
		)

		val sockFns = "recv|recvfrom|send|sendto|read|write|poll|select|SSL_read|SSL_write|SSL_shutdown|SSL_do_handshake|close|closesocket|setsockopt|getsockopt|shutdown|fcntl|ioctlsocket"
		val discarded = cpg.call.name(sockFns).filter(_.astParent.label == "BLOCK").l
		report(byLocation(discarded).map(c => (loc(c), oneLine(c.code))), 60)
	}

	// ------------------------------------------------------------------- q8

	if (wanted("q8")) {
		header(
			"q8",
			"Type-punning cast to a multi-byte pointer",
			"""A cast producing uint16_t*/uint32_t*/char16_t*/double*/... over a byte
			  |buffer: reading through it is unaligned-access UB that UBSan only reports if
			  |an input reaches the site. The read path does not do this -- it assembles
			  |values byte-wise with shifts (TokenParser::ParseError) -- so the expected set
			  |is small and known: utf16.cpp proves 2-byte alignment at the site before
			  |casting (`AlignedUtf16View`, which copies into scratch when the test fails)
			  |and bcp_row_encoder reads DuckDB vector data, which the allocator aligns.
			  |What matters is a NEW entry, especially one in a parser. Matched on the
			  |cast's source text: a cast's typeFullName in this frontend never carries the
			  |`*`.""".stripMargin
		)

		val punned = ".*(reinterpret_cast|static_cast|const_cast)\\s*<\\s*(const\\s+)?(uint16_t|uint32_t|uint64_t|int16_t|int32_t|int64_t|unsigned short|unsigned int|short|long|float|double|wchar_t|char16_t)\\s*\\*.*"
		val punnedCasts = cpg.call.nameExact(Operators.cast).code(punned).l
		report(byLocation(punnedCasts).map(c => (loc(c), oneLine(c.code))))
	}

	// ------------------------------------------------------------------- q9

	if (wanted("q9")) {
		header(
			"q9",
			"Parsing method not statically reachable from any fuzz entry point",
			"""The call graph answer to "what does fuzz/ not cover". Starts at the harness
			  |entry points and walks callees; a method from q1 that the walk never visits
			  |is parsing code no harness can reach -- either because it needs a new
			  |harness, or because it is write-path code that never sees wire bytes (the
			  |BCP encoders and the row stager's append side are the honest exceptions).
			  |CDT leaves some callees unresolved, so this over-reports rather than under.""".stripMargin
		)

		val entryNames = List("Feed", "TryParseNext", "ParseLoginResponse", "ParseBrowserResponse", "Utf16LEDecode", "LegacyUtf16LEDecode", "FindBeginTxnDescriptor", "StageRow", "StageNBCRow")
		val reached = cpg.method
			.nameExact(entryNames*)
			.repeat(_.callee(NoResolve))(_.emit.maxDepth(10))
			.fullName
			.toSet

		val rows = parseMethods
			.filterNot(m => reached.contains(m.fullName))
			.filterNot(m => entryNames.contains(m.name))
			.l
		report(byLocation(rows).map(m => (loc(m), sig(m.asInstanceOf[Method]))), 80)
	}

	println("")
	println("---")
	println("Every finding needs reading against the source; joern/README.md says what each")
	println("query can and cannot see.")
}

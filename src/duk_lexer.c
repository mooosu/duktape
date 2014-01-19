/*
 *  Lexer for source files, ToNumber() string conversions, RegExp expressions,
 *  and JSON.
 *
 *  Provides a stream of Ecmascript tokens from an UTF-8/CESU-8 buffer.  The
 *  caller can also rewind the token stream into a certain position which is
 *  needed by the compiler part for multi-pass scanning.  Tokens are
 *  represented as duk_token structures, and contain line number information.
 *  Token types are identified with DUK_TOK_* defines.
 *
 *  Characters are decoded into a fixed size lookup window consisting of
 *  decoded Unicode code points, with window positions past the end of the
 *  input filled with an invalid codepoint (-1).  The tokenizer can thus
 *  perform multiple character lookups efficiently and with few sanity
 *  checks (such as access outside the end of the input), which keeps the
 *  tokenization code small at the cost of performance.
 * 
 *  Character data in tokens (such as identifier names and string literals)
 *  is encoded into CESU-8 format on-the-fly while parsing the token in
 *  question.  The string data is made reachable to garbage collection by
 *  placing the token-related values in value stack entries allocated for
 *  this purpose by the caller.  The characters exist in Unicode code point
 *  form only in the fixed size lookup window, which keeps character data
 *  expansion (of especially ASCII data) low.
 *
 *  Token parsing supports the full range of Unicode characters as described
 *  in the E5 specification.  Parsing has been optimized for ASCII characters
 *  because ordinary Ecmascript code consists almost entirely of ASCII
 *  characters.  Matching of complex Unicode codepoint sets (such as in the
 *  IdentifierStart and IdentifierPart productions) is optimized for size,
 *  and is done using a linear scan of a bit-packed list of ranges.  This is
 *  very slow, but should never be entered unless the source code actually
 *  contains Unicode characters.
 *
 *  Ecmascript tokenization is partially context sensitive.  First,
 *  additional future reserved words are recognized in strict mode (see E5
 *  Section 7.6.1.2).  Second, a forward slash character ('/') can be
 *  recognized either as starting a RegExp literal or as a division operator,
 *  depending on context.  The caller must provide necessary context flags
 *  when requesting a new token.
 *
 *  Future work:
 *
 *    * Make the input window a circular array to avoid copying.  This would
 *      not necessarily complicate the tokenizer much, although it would make
 *      the window fetches more expensive (one AND).
 *
 *    * Make line number tracking optional, as it consumes space.  Also, is
 *      tracking end line really useful for tokens?
 *
 *    * Add a feature flag for disabling UTF-8 decoding of input, as most
 *      source code is ASCII.  Because of Unicode escapes written in ASCII,
 *      this does not allow Unicode support to be removed from e.g.
 *      duk_unicode_is_identifier_start() nor does it allow removal of CESU-8
 *      encoding of e.g. string literals.
 *
 *    * Add a feature flag for disabling Unicode compliance of e.g. identifier
 *      names.  This allows for a build more than a kilobyte smaller, because
 *      Unicode ranges needed by duk_unicode_is_identifier_start() and
 *      duk_unicode_is_identifier_part() can be dropped.  String literals
 *      should still be allowed to contain escaped Unicode, so this still does
 *      not allow removal of CESU-8 encoding of e.g. string literals.
 *
 *    * Character lookup tables for codepoints above BMP could be stripped.
 *
 *    * Strictly speaking, E5 specification requires that source code consists
 *      of 16-bit code units, and if not, must be conceptually converted to
 *      that format first.  The current lexer processes Unicode code points
 *      and allows characters outside the BMP.  These should be converted to
 *      surrogate pairs while reading the source characters into the window,
 *      not after tokens have been formed (as is done now).  However, the fix
 *      is not trivial because two characters are decoded from one codepoint.
 *
 *    * Optimize for speed as well as size.  Large if-else ladders are slow.
 */

#include "duk_internal.h"

/* FIXME: check defines */

/*
 *  Various defines and file specific helper macros
 */

#define MAX_REGEXP_DECIMAL_ESCAPE_DIGITS  9
#define MAX_REGEXP_QUANTIFIER_DIGITS      9   /* FIXME: does not allow e.g. 2**31-1, but one more would allow overflows of u32 */

#define LOOKUP(lex_ctx,index)    ((lex_ctx)->window[(index)])
#define ADVANCE(lex_ctx,count)   advance_chars((lex_ctx), (count))
#define INITBUFFER(lex_ctx)      initbuffer((lex_ctx))
#define APPENDBUFFER(lex_ctx,x)  appendbuffer((lex_ctx), (int) (x))

/* whether to use macros or helper function depends on call count */
#define ISDIGIT(x)          ((x) >= '0' && (x) <= '9')
#define ISHEXDIGIT(x)       is_hex_digit((x))
#define ISOCTDIGIT(x)       ((x) >= '0' && (x) <= '7')
#define ISDIGIT03(x)        ((x) >= '0' && (x) <= '3')
#define ISDIGIT47(x)        ((x) >= '4' && (x) <= '7')

/* lookup shorthands (note: assume context variable is named 'lex_ctx') */
#define L0()  LOOKUP(lex_ctx, 0)
#define L1()  LOOKUP(lex_ctx, 1)
#define L2()  LOOKUP(lex_ctx, 2)
#define L3()  LOOKUP(lex_ctx, 3)
#define L4()  LOOKUP(lex_ctx, 4)
#define L5()  LOOKUP(lex_ctx, 5)

/* packed advance/token number macro used by multiple functions */
#define ADVTOK(adv,tok)  (((adv) << 8) + (tok))

/*
 *  Read a character from the window leading edge and update the line counter.
 *
 *  Decodes UTF-8/CESU-8 leniently with support for code points from U+0000 to
 *  U+10FFFF, causing an error if the input is unparseable.  Leniency means:
 *
 *    * Unicode code point validation is intentionally not performed,
 *      except to check that the codepoint does not exceed 0x10ffff.
 *
 *    * In particular, surrogate pairs are allowed and not combined, which
 *      allows source files to represent all SourceCharacters with CESU-8.
 *      Broken surrogate pairs are allowed, as Ecmascript does not mandate
 *      their validation.
 *
 *    * Allow non-shortest UTF-8 encodings.
 *
 *  Leniency here causes few security concerns because all character data is
 *  decoded into Unicode codepoints before lexer processing, and is then
 *  re-encoded into CESU-8.  The source can be parsed as strict UTF-8 with
 *  a compiler option.  However, Ecmascript source characters include -all-
 *  16-bit unsigned integer codepoints, so leniency seems to be appropriate.
 *
 *  Note that codepoints above the BMP are not strictly SourceCharacters,
 *  but the lexer still accepts them as such.  Before ending up in a string
 *  or an identifier name, codepoints above BMP are converted into surrogate
 *  pairs and then CESU-8 encoded, resulting in 16-bit Unicode data as
 *  expected by Ecmascript.
 *
 *  An alternative approach to dealing with invalid or partial sequences
 *  would be to skip them and replace them with e.g. the Unicode replacement
 *  character U+FFFD.  This has limited utility because a replacement character
 *  will most likely cause a parse error, unless it occurs inside a string.
 *  Further, Ecmascript source is typically pure ASCII.
 *
 *  See:
 *
 *     http://en.wikipedia.org/wiki/UTF-8
 *     http://en.wikipedia.org/wiki/CESU-8
 *     http://tools.ietf.org/html/rfc3629
 *     http://en.wikipedia.org/wiki/UTF-8#Invalid_byte_sequences
 *
 *  Future work:
 *
 *    * Reject other invalid Unicode sequences (see Wikipedia entry for examples)
 *      in strict UTF-8 mode.
 * 
 *    * Size optimize.  An attempt to use a 16-byte lookup table for the first
 *      byte resulted in a code increase though.
 *
 *    * Is checking against maximum 0x10ffff really useful?  4-byte encoding
 *      imposes a certain limit anyway.
 */

static int read_char(duk_lexer_ctx *lex_ctx) {
	/* attempting to reduce size of 'len' and/or 'i' resulted in larger code */
	int x;
	int len;
	int i;
	duk_uint8_t *p;
#ifdef DUK_USE_STRICT_UTF8_SOURCE
	int mincp;
#endif

	if (lex_ctx->input_offset >= lex_ctx->input_length) {
		return -1;
	}

	p = &lex_ctx->input[lex_ctx->input_offset];
	x = (int) *p++;

	if (x < 0x80) {
		/* 0xxx xxxx -> fast path */
		len = 1;
		goto fastpath;
	} else if (x < 0xc0) {
		/* 10xx xxxx -> invalid */
		goto error;
	} else if (x < 0xe0) {
		/* 110x xxxx   10xx xxxx  */
		len = 2;
#ifdef DUK_USE_STRICT_UTF8_SOURCE
		mincp = 0x80;
#endif
		x = x & 0x1f;
	} else if (x < 0xf0) {
		/* 1110 xxxx   10xx xxxx   10xx xxxx */
		len = 3;
#ifdef DUK_USE_STRICT_UTF8_SOURCE
		mincp = 0x800;
#endif
		x = x & 0x0f;
	} else if (x < 0xf8) {
		/* 1111 0xxx   10xx xxxx   10xx xxxx   10xx xxxx */
		len = 4;
#ifdef DUK_USE_STRICT_UTF8_SOURCE
		mincp = 0x10000;
#endif
		x = x & 0x07;
	} else {
		/* no point in supporting encodings of 5 or more bytes */
		goto error;
	}

	if (len > lex_ctx->input_length - lex_ctx->input_offset) {
		goto error;
	}

	for (i = 1; i < len; i++) {
		int y = *p++;
		if ((y & 0xc0) != 0x80) {
			/* check that byte has the form 10xx xxxx */
			goto error;
		}
		x = x << 6;
		x += y & 0x3f;
	}

	/* check final character validity */

	if (x > 0x10ffff) {
		goto error;
	}
#ifdef DUK_USE_STRICT_UTF8_SOURCE
	if (x < mincp || (x >= 0xd800 && x <= 0xdfff) || x == 0xfffe) {
		goto error;
	}
#endif

	/* fall through */

 fastpath:
	/* input offset tracking */
	lex_ctx->input_offset += len;

	/* line tracking */
	if ((x == 0x000a) ||
	    ((x == 0x000d) && (lex_ctx->input_offset >= lex_ctx->input_length ||
	                       lex_ctx->input[lex_ctx->input_offset] != 0x000a)) ||
	    (x == 0x2028) ||
	    (x == 0x2029)) {
		/* lookup for 0x000a above assumes shortest encoding now */

		/* E5 Section 7.3, treat the following as newlines:
		 *   LF
		 *   CR [not followed by LF]
		 *   LS
		 *   PS
		 *
		 * For CR LF, CR is ignored if it is followed by LF, and the LF will bump
		 * the line number.
		 */
		lex_ctx->input_line++;
	}

	return x;

 error:
	DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR, "invalid char encoding in source");
	return 0;
}

/*
 *  Advance lookup window by N characters.  Also used to fill the window
 *  after position is changed (call with count == DUK_LEXER_WINDOW_SIZE).
 *
 *  Future work:
 *
 *    * A lot of copying now, perhaps change to circular array or at
 *      least use memcpy().  For memcpy(), putting all elements of the
 *      window (code point, offset, line) into a struct would allow one
 *      memcpy() to slide the window, instead of three separate copys.
 */

static void advance_chars(duk_lexer_ctx *lex_ctx, int count) {
	int i;

	DUK_ASSERT(count >= 0 && count <= DUK_LEXER_WINDOW_SIZE);

	if (count == 0) {
		/* allowing zero count makes some special caller flows easier */
		return;
	}

	for (i = 0; i < DUK_LEXER_WINDOW_SIZE - count; i++) {
		lex_ctx->offsets[i] = lex_ctx->offsets[i + count];
		lex_ctx->lines[i] = lex_ctx->lines[i + count];
		lex_ctx->window[i] = lex_ctx->window[i + count];
	}

	for (; i < DUK_LEXER_WINDOW_SIZE; i++) {
		lex_ctx->offsets[i] = lex_ctx->input_offset;
		lex_ctx->lines[i] = lex_ctx->input_line;
		lex_ctx->window[i] = read_char(lex_ctx);
	}
}

/*
 *  (Re)initialize the temporary byte buffer.  May be called extra times
 *  with little impact.
 */

static void initbuffer(duk_lexer_ctx *lex_ctx) {
	if (lex_ctx->buf->usable_size < DUK_LEXER_TEMP_BUF_LIMIT) {
		/* FIXME: resize (zero) without realloc -> API change */
		lex_ctx->buf->size = 0;
	} else {
		duk_hbuffer_resize(lex_ctx->thr, lex_ctx->buf, 0, DUK_LEXER_TEMP_BUF_LIMIT);
	}
}

/*
 *  Append a Unicode codepoint to the temporary byte buffer.  Performs
 *  CESU-8 surrogate pair encoding for codepoints above the BMP.
 *  Existing surrogate pairs are allowed and also encoded into CESU-8.
 */

static void appendbuffer(duk_lexer_ctx *lex_ctx, int x) {
	/*
	 *  Since character data is only generated by decoding the source or by
	 *  the compiler itself, we rely on the input codepoints being correct
	 *  and avoid a check here.
	 *
	 *  Character data can also come here through decoding of Unicode
	 *  escapes ("\udead\ubeef") so all 16-but unsigned values can be
	 *  present, even when the source file itself is strict UTF-8.
	 */

	DUK_ASSERT(x >= 0 && x <= 0x10ffff);

	duk_hbuffer_append_cesu8(lex_ctx->thr, lex_ctx->buf, x);
}

/*
 *  Intern the temporary byte buffer into a valstack slot
 *  (in practice, slot1 or slot2).
 */

static void internbuffer(duk_lexer_ctx *lex_ctx, int valstack_idx) {
	duk_context *ctx = (duk_context *) lex_ctx->thr;

	DUK_ASSERT(valstack_idx == lex_ctx->slot1_idx || valstack_idx == lex_ctx->slot2_idx);

	duk_dup(ctx, lex_ctx->buf_idx);
	duk_to_string(ctx, -1);
	duk_replace(ctx, valstack_idx);
}

/*
 *  Init lexer context
 */

void duk_lexer_initctx(duk_lexer_ctx *lex_ctx) {
	DUK_ASSERT(lex_ctx != NULL);

	DUK_MEMSET(lex_ctx, 0, sizeof(*lex_ctx));
#ifdef DUK_USE_EXPLICIT_NULL_INIT
	lex_ctx->thr = NULL;
	lex_ctx->input = NULL;
	lex_ctx->buf = NULL;
#endif
}

/*
 *  Set lexer input position and reinitialize lookup window.
 */

/* NB: duk_lexer_getpoint() is a macro only */

void duk_lexer_setpoint(duk_lexer_ctx *lex_ctx, duk_lexer_point *pt) {
	DUK_ASSERT(pt->offset >= 0);
	DUK_ASSERT(pt->line >= 1);
	lex_ctx->input_offset = pt->offset;
	lex_ctx->input_line = pt->line;
	advance_chars(lex_ctx, DUK_LEXER_WINDOW_SIZE);  /* fill window */
}

/*
 *  Lexing helpers
 */

/* numeric value of a hex digit (also covers octal and decimal digits) */
static int hexval(duk_lexer_ctx *lex_ctx, int x) {
	if (x >= '0' && x <= '9') {
		return ((int) x) - ((int) '0');
	} else if (x >= 'a' && x <= 'f') {
		return ((int) x) - ((int) 'a') + 0x0a;
	} else if (x >= 'A' && x <= 'F') {
		return ((int) x) - ((int) 'A') + 0x0a;
	}

	/* Throwing an error this deep makes the error rather vague, but
	 * saves hundreds of bytes of code.
	 */
	DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR, "decode error");
	return 0;
}

/* having this as a separate function provided a size benefit */
static int is_hex_digit(int x) {
	return (x >= '0' && x <= '9') ||
	       (x >= 'a' && x <= 'f') ||
	       (x >= 'A' && x <= 'F');
}

static int decode_hex_escape_from_window(duk_lexer_ctx *lex_ctx, int lookup_offset) {
	/* validation performed by hexval */
	return (hexval(lex_ctx, lex_ctx->window[lookup_offset]) << 4) |
	       (hexval(lex_ctx, lex_ctx->window[lookup_offset + 1]));
}

static int decode_unicode_escape_from_window(duk_lexer_ctx *lex_ctx, int lookup_offset) {
	/* validation performed by hexval */
	return (hexval(lex_ctx, lex_ctx->window[lookup_offset]) << 12) |
	       (hexval(lex_ctx, lex_ctx->window[lookup_offset + 1]) << 8) |
	       (hexval(lex_ctx, lex_ctx->window[lookup_offset + 2]) << 4) |
	       (hexval(lex_ctx, lex_ctx->window[lookup_offset + 3]));
}

/*
 *  Eat input characters until first character of window is not
 *  a white space (may be -1 if EOF encountered).
 */
static void eat_whitespace(duk_lexer_ctx *lex_ctx) {
	/* guaranteed to finish, as EOF (-1) is not a whitespace */
	while (duk_unicode_is_whitespace(LOOKUP(lex_ctx, 0))) {
		ADVANCE(lex_ctx, 1);
	}
}

/*
 *  Parse Ecmascript source InputElementDiv or InputElementRegExp
 *  (E5 Section 7).
 *
 *  Possible results are:
 *    (1) a token
 *    (2) a line terminator
 *    (3) a comment
 *    (4) EOF
 *
 *  White space is automatically skipped from the current position (but
 *  not after the input element).  If input has already ended, returns
 *  DUK_TOK_EOF indefinitely.  If a parse error occurs, uses an DUK_ERROR()
 *  macro call (and hence a longjmp through current heap longjmp context).
 *
 *  The input element being matched is determined by regexp_mode; if set,
 *  parses a InputElementRegExp, otherwise a InputElementDiv.  The
 *  difference between these are handling of productions starting with a
 *  forward slash.
 *
 *  If strict_mode is set, recognizes additional future reserved words
 *  specific to strict mode, and refuses to parse octal literals.
 *
 *  The matching strategy below is to (currently) use a six character
 *  lookup window to quickly determine which production is the -longest-
 *  matching one, and then parse that.  The top-level if-else clauses
 *  match the first character, and the code blocks for each clause
 *  handle -all- alternatives for that first character.  Ecmascript
 *  specification uses the "longest match wins" semantics, so the order
 *  of the if-clauses matters.
 *
 *  Misc notes:
 *
 *    * Ecmascript numeric literals do not accept a sign character.
 *      Consequently e.g. "-1.0" is parsed as two tokens: a negative
 *      sign and a positive numeric literal.  The compiler performs
 *      the negation during compilation, so this has no adverse impact.
 *
 *    * There is no token for "undefined": it is just a value available
 *      from the global object (or simply established by doing a reference
 *      to an undefined value).
 *
 *    * Some contexts want Identifier tokens, which are IdentifierNames
 *      excluding reserved words, while some contexts want IdentifierNames
 *      directly.  In the latter case e.g. "while" is interpreted as an
 *      identifier name, not a DUK_TOK_WHILE token.  The solution here is
 *      to provide both token types: DUK_TOK_WHILE goes to 't' while
 *      DUK_TOK_IDENTIFIER goes to 't_nores', and 'slot1' always contains
 *      the identifier / keyword name.
 *
 *    * Directive prologue needs to identify string literals such as
 *      "use strict" and 'use strict', which are sensitive to line
 *      continuations and escape sequences.  For instance, "use\u0020strict"
 *      is a valid directive but is distinct from "use strict".  The solution
 *      here is to decode escapes while tokenizing, but to keep track of the
 *      number of escapes.  Directive detection can then check that the
 *      number of escapes is zero.
 *
 *    * Comments are expressed as DUK_TOK_COMMENT tokens, with the type
 *      (single- or multi-line) and contents of the comments lost.
 *      Furthermore, multi-line comments with one or more internal
 *      LineTerminator are treated as DUK_TOK_LINETERM to comply with
 *      automatic semicolon insertion and to avoid complicating the
 *      tokenization process.  See E5 Section 7.4.
 */

static void parse_input_element_raw(duk_lexer_ctx *lex_ctx,
                                    duk_token *out_token,
                                    int strict_mode,
                                    int regexp_mode) {
	int x, y;               /* temporaries, must be 32-bit to hold Unicode code points */
	int advtok = 0;         /* (advance << 8) + token_type, updated at function end,
	                         * init is unnecessary but suppresses "may be used uninitialized warnings
	                         */

	if (++lex_ctx->token_count >= lex_ctx->token_limit) {
		DUK_ERROR(lex_ctx->thr, DUK_ERR_RANGE_ERROR, "token limit");
		return;  /* unreachable */
	}

	eat_whitespace(lex_ctx);

	out_token->t = DUK_TOK_EOF;
	out_token->t_nores = -1;	/* marker: copy t if not changed */
	out_token->num = DUK_DOUBLE_NAN;
	out_token->str1 = NULL;
	out_token->str2 = NULL;
	out_token->num_escapes = 0;
	out_token->start_line = lex_ctx->lines[0];
	/* out_token->end_line set at exit */
	/* out_token->lineterm set by caller */

	duk_to_undefined((duk_context *) lex_ctx->thr, lex_ctx->slot1_idx);
	duk_to_undefined((duk_context *) lex_ctx->thr, lex_ctx->slot2_idx);

	/* 'advtok' indicates how much to advance and which token id to assign
	 * at the end.  This shared functionality minimizes code size.  All
	 * code paths are required to set 'advtok' to some value, so no default
	 * init value is used.  Code paths calling DUK_ERROR() never return so
	 * they don't need to set advtok.
	 */

	/*
	 *  Matching order:
	 *
	 *    Punctuator first chars, also covers comments, regexps
	 *    LineTerminator
	 *    Identifier or reserved word, also covers null/true/false literals
	 *    NumericLiteral
	 *    StringLiteral
	 *    EOF
	 *
	 *  The order does not matter as long as the longest match is
	 *  always correctly identified.  There are order dependencies
	 *  in the clauses, so it's not trivial to convert to a switch.
	 *
	 *  Maybe change this to a switch which handles all single character
	 *  cases and a follow-up if-else chain.  Switch matches need to goto
	 *  to bypass the if-else chain.
	 */

	x = L0();
	y = L1();

	if (x == '/') {
		if (y == '/') {
			/*
			 *  E5 Section 7.4, allow SourceCharacter (which is any 16-bit
			 *  code point).
			 */

			/* ADVANCE(lex_ctx, 2) would be correct here, but it unnecessary */
			for (;;) {
				x = L0();
				if (x < 0 || duk_unicode_is_line_terminator(x)) {
					break;
				}
				ADVANCE(lex_ctx, 1);
			}
			advtok = ADVTOK(0, DUK_TOK_COMMENT);
		} else if (y == '*') {
			/*
			 *  E5 Section 7.4.  If the multi-line comment contains a newline,
			 *  it is treated like a single DUK_TOK_LINETERM to facilitate
			 *  automatic semicolon insertion.
			 */

			duk_uint8_t last_asterisk = 0;
			advtok = ADVTOK(0, DUK_TOK_COMMENT);
			ADVANCE(lex_ctx, 2);
			for (;;) {
				x = L0();
				if (x < 0) {
					DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
					          "eof while parsing multiline comment");
				}
				ADVANCE(lex_ctx, 1);
				if (last_asterisk && x == '/') {
					break;
				}
				if (duk_unicode_is_line_terminator(x)) {
					advtok = ADVTOK(0, DUK_TOK_LINETERM);
				}
				last_asterisk = (x == (int) '*');
			}
		} else if (regexp_mode) {
#ifdef DUK_USE_REGEXP_SUPPORT
			/*
			 *  "/" followed by something in regexp mode.  See E5 Section 7.8.5.
			 *
			 *  RegExp parsing is a bit complex.  First, the regexp body is delimited
			 *  by forward slashes, but the body may also contain forward slashes as
			 *  part of an escape sequence or inside a character class (delimited by
			 *  square brackets).  A mini state machine is used to implement these.
			 *
			 *  Further, an early (parse time) error must be thrown if the regexp
			 *  would cause a run-time error when used in the expression new RegExp(...).
			 *  Parsing here simply extracts the (candidate) regexp, and also accepts
			 *  invalid regular expressions (which are delimited properly).  The caller
			 *  (compiler) must perform final validation and regexp compilation.
			 *
			 *  RegExp first char may not be '/' (single line comment) or '*' (multi-
			 *  line comment).  These have already been checked above, so there is no
			 *  need below for special handling of the first regexp character as in
			 *  the E5 productions.
			 *
			 *  About unicode escapes within regexp literals:
			 *
			 *      E5 Section 7.8.5 grammar does NOT accept \uHHHH escapes.
			 *      However, Section 6 states that regexps accept the escapes,
			 *      see paragraph starting with "In string literals...".
			 *      The regexp grammar, which sees the decoded regexp literal
			 *      (after lexical parsing) DOES have a \uHHHH unicode escape.
			 *      So, for instance:
			 *
			 *          /\u1234/
			 *
			 *      should first be parsed by the lexical grammar as:
			 *
			 *          '\' 'u'		RegularExpressionBackslashSequence
			 *          '1'			RegularExpressionNonTerminator
			 *          '2'			RegularExpressionNonTerminator
			 *          '3'			RegularExpressionNonTerminator
			 *          '4'			RegularExpressionNonTerminator
			 *
			 *      and the escape itself is then parsed by the regexp engine.
			 *      This is the current implementation. 
			 *
			 *  Minor spec inconsistency:
			 *
			 *      E5 Section 7.8.5 RegularExpressionBackslashSequence is:
			 *
			 *         \ RegularExpressionNonTerminator
			 *
			 *      while Section A.1 RegularExpressionBackslashSequence is:
			 *
			 *         \ NonTerminator
			 * 
			 *      The latter is not normative and a typo.
			 * 
			 */

			/* FIXME: needs to be checked carefully */

			/* FIXME: lexical parsing of regexps may be needed even without regexp
			 * support because regexp mode is the default in the compiler.
			 */

			/* first, parse regexp body roughly */

			duk_uint8_t state = 0;  /* 0=base, 1=esc, 2=class, 3=class+esc */

			INITBUFFER(lex_ctx);
			for (;;) {
				ADVANCE(lex_ctx, 1);	/* skip opening slash on first loop */
				x = L0();
				if (x < 0 || duk_unicode_is_line_terminator(x)) {
					DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
					          "eof or line terminator while parsing regexp");
				}
				x = L0();	/* re-read to avoid spill / fetch */
				if (state == 0) {
					if (x == '/') {
						ADVANCE(lex_ctx, 1);	/* eat closing slash */
						break;
					} else if (x == '\\') {
						state = 1;
					} else if (x == '[') {
						state = 2;
					}
				} else if (state == 1) {
					state = 0;
				} else if (state == 2) {
					if (x == ']') {
						state = 0;
					} else if (x == '\\') {
						state = 3;
					}
				} else { /* state == 3 */
					state = 2;
				}
				APPENDBUFFER(lex_ctx, x);
			}
			internbuffer(lex_ctx, lex_ctx->slot1_idx);
			out_token->str1 = duk_get_hstring((duk_context *) lex_ctx->thr, lex_ctx->slot1_idx);

			/* second, parse flags */

			INITBUFFER(lex_ctx);
			for (;;) {
				x = L0();
				if (!duk_unicode_is_identifier_part(x)) {
					break;
				}
				x = L0();	/* re-read to avoid spill / fetch */
				APPENDBUFFER(lex_ctx, x);
				ADVANCE(lex_ctx, 1);
			}
			internbuffer(lex_ctx, lex_ctx->slot2_idx);
			out_token->str2 = duk_get_hstring((duk_context *) lex_ctx->thr, lex_ctx->slot2_idx);

			INITBUFFER(lex_ctx);	/* free some memory */

			/* validation of the regexp is caller's responsibility */

			advtok = ADVTOK(0, DUK_TOK_REGEXP);
#else
			DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR, "regexp support disabled");
#endif
		} else if (y == '=') {
			/* "/=" and not in regexp mode */
			advtok = ADVTOK(2, DUK_TOK_DIV_EQ);
		} else {
			/* "/" and not in regexp mode */
			advtok = ADVTOK(1, DUK_TOK_DIV);
		}
	} else if (x == '{') {
		advtok = ADVTOK(1, DUK_TOK_LCURLY);
	} else if (x == '}') {
		advtok = ADVTOK(1, DUK_TOK_RCURLY);
	} else if (x == '(') {
		advtok = ADVTOK(1, DUK_TOK_LPAREN);
	} else if (x == ')') {
		advtok = ADVTOK(1, DUK_TOK_RPAREN);
	} else if (x == '[') {
		advtok = ADVTOK(1, DUK_TOK_LBRACKET);
	} else if (x == ']') {
		advtok = ADVTOK(1, DUK_TOK_RBRACKET);
	} else if (x == '.' && !ISDIGIT(y)) {
		/* Note: period followed by a digit can only start DecimalLiteral (captured below) */
		advtok = ADVTOK(1, DUK_TOK_PERIOD);
	} else if (x == ';') {
		advtok = ADVTOK(1, DUK_TOK_SEMICOLON);
	} else if (x == ',') {
		advtok = ADVTOK(1, DUK_TOK_COMMA);
	} else if (x == '<') {
		if (y == '<' && L2() == '=') {
			advtok = ADVTOK(3, DUK_TOK_ALSHIFT_EQ);
		} else if (y == '=') {
			advtok = ADVTOK(2, DUK_TOK_LE);
		} else if (y == '<') {
			advtok = ADVTOK(2, DUK_TOK_ALSHIFT);
		} else {
			advtok = ADVTOK(1, DUK_TOK_LT);
		}
	} else if (x == '>') {
		if (y == '>' && L2() == '>' && L3() == '=') {
			advtok = ADVTOK(4, DUK_TOK_RSHIFT_EQ);
		} else if (y == '>' && L2() == '>') {
			advtok = ADVTOK(3, DUK_TOK_RSHIFT);
		} else if (y == '>' && L2() == '=') {
			advtok = ADVTOK(3, DUK_TOK_ARSHIFT_EQ);
		} else if (y == '=') {
			advtok = ADVTOK(2, DUK_TOK_GE);
		} else if (y == '>') {
			advtok = ADVTOK(2, DUK_TOK_ARSHIFT);
		} else {
			advtok = ADVTOK(1, DUK_TOK_GT);
		}
	} else if (x == '=') {
		if (y == '=' && L2() == '=') {
			advtok = ADVTOK(3, DUK_TOK_SEQ);
		} else if (y == '=') {
			advtok = ADVTOK(2, DUK_TOK_EQ);
		} else {
			advtok = ADVTOK(1, DUK_TOK_EQUALSIGN);
		}
	} else if (x == '!') {
		if (y == '=' && L2() == '=') {
			advtok = ADVTOK(3, DUK_TOK_SNEQ);
		} else if (y == '=') {
			advtok = ADVTOK(2, DUK_TOK_NEQ);
		} else {
			advtok = ADVTOK(1, DUK_TOK_LNOT);
		}
	} else if (x == '+') {
		if (y == '+') {
			advtok = ADVTOK(2, DUK_TOK_INCREMENT);
		} else if (y == '=') {
			advtok = ADVTOK(2, DUK_TOK_ADD_EQ);
		} else {
			advtok = ADVTOK(1, DUK_TOK_ADD);
		}
	} else if (x == '-') {
		if (y == '-') {
			advtok = ADVTOK(2, DUK_TOK_DECREMENT);
		} else if (y == '=') {
			advtok = ADVTOK(2, DUK_TOK_SUB_EQ);
		} else {
			advtok = ADVTOK(1, DUK_TOK_SUB);
		}
	} else if (x == '*') {
		if (y == '=') {
			advtok = ADVTOK(2, DUK_TOK_MUL_EQ);
		} else {
			advtok = ADVTOK(1, DUK_TOK_MUL);
		}
	} else if (x == '%') {
		if (y == '=') {
			advtok = ADVTOK(2, DUK_TOK_MOD_EQ);
		} else {
			advtok = ADVTOK(1, DUK_TOK_MOD);
		}
	} else if (x == '&') {
		if (y == '&') {
			advtok = ADVTOK(2, DUK_TOK_LAND);
		} else if (y == '=') {
			advtok = ADVTOK(2, DUK_TOK_BAND_EQ);
		} else {
			advtok = ADVTOK(1, DUK_TOK_BAND);
		}
	} else if (x == '|') {
		if (y == '|') {
			advtok = ADVTOK(2, DUK_TOK_LOR);
		} else if (y == '=') {
			advtok = ADVTOK(2, DUK_TOK_BOR_EQ);
		} else {
			advtok = ADVTOK(1, DUK_TOK_BOR);
		}
	} else if (x == '^') {
		if (y == '=') {
			advtok = ADVTOK(2, DUK_TOK_BXOR_EQ);
		} else {
			advtok = ADVTOK(1, DUK_TOK_BXOR);
		}
	} else if (x == '~') {
		advtok = ADVTOK(1, DUK_TOK_BNOT);
	} else if (x == '?') {
		advtok = ADVTOK(1, DUK_TOK_QUESTION);
	} else if (x == ':') {
		advtok = ADVTOK(1, DUK_TOK_COLON);
	} else if (duk_unicode_is_line_terminator(x)) {
		if (x == 0x000d && y == 0x000a) {
			/*
			 *  E5 Section 7.3: CR LF is detected as a single line terminator for
			 *  line numbers.  Here we also detect it as a single line terminator
			 *  token.
			 */
			advtok = ADVTOK(2, DUK_TOK_LINETERM);
		} else {
			advtok = ADVTOK(1, DUK_TOK_LINETERM);
		}
	} else if (duk_unicode_is_identifier_start(x) || x == '\\') {
		/*
		 *  Parse an identifier and then check whether it is:
		 *    - reserved word (keyword or other reserved word)
		 *    - "null"  (NullLiteral)
		 *    - "true"  (BooleanLiteral)
		 *    - "false" (BooleanLiteral)
		 *    - anything else => identifier
		 *
		 *  This does not follow the E5 productions cleanly, but is
		 *  useful and compact.
		 *
		 *  Note that identifiers may contain Unicode escapes,
		 *  see E5 Sections 6 and 7.6.  They must be decoded first,
		 *  and the result checked against allowed characters.
		 *  The above if-clause accepts an identifier start and an
		 *  '\' character -- no other token can begin with a '\'.
		 *
		 *  Note that "get" and "set" are not reserved words in E5
		 *  specification so they are recognized as plain identifiers
		 *  (the tokens DUK_TOK_GET and DUK_TOK_SET are actually not
		 *  used now).  The compiler needs to work around this.
		 *
		 *  Strictly speaking, following Ecmascript longest match
		 *  specification, an invalid escape for the first character
		 *  should cause a syntax error.  However, an invalid escape
		 *  for IdentifierParts should just terminate the identifier
		 *  early (longest match), and let the next tokenization
		 *  fail.  For instance Rhino croaks with 'foo\z' when
		 *  parsing the identifier.  This has little practical impact.
		 */

		int i, i_end;
		int first = 1;
		duk_hstring *str;

		INITBUFFER(lex_ctx);
		for (;;) {
			/* re-lookup first char on first loop */
			if (L0() == '\\') {
				int ch;
				if (L1() != 'u') {
					DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
					          "invalid unicode escape while parsing identifier");
				}

				ch = decode_unicode_escape_from_window(lex_ctx, 2);

				/* IdentifierStart is stricter than IdentifierPart, so if the first
				 * character is escaped, must have a stricter check here.
				 */
				if (! (first ? duk_unicode_is_identifier_start(ch) : duk_unicode_is_identifier_part(ch))) {
					DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
					          "invalid unicode escaped character while parsing identifier");
				}
				APPENDBUFFER(lex_ctx, ch);
				ADVANCE(lex_ctx, 6);

				/* Track number of escapes: necessary for proper keyword
				 * detection.
				 */
				out_token->num_escapes++;
			} else {
				/* Note: first character is checked against this.  But because
				 * IdentifierPart includes all IdentifierStart characters, and
				 * the first character (if unescaped) has already been checked
				 * in the if condition, this is OK.
				 */
				if (!duk_unicode_is_identifier_part(L0())) {
					break;
				}
				APPENDBUFFER(lex_ctx, L0());
				ADVANCE(lex_ctx, 1);
			}
			first = 0;
		}

		internbuffer(lex_ctx, lex_ctx->slot1_idx);
		out_token->str1 = duk_get_hstring((duk_context *) lex_ctx->thr, lex_ctx->slot1_idx);
		str = out_token->str1;
		DUK_ASSERT(str != NULL);
		out_token->t_nores = DUK_TOK_IDENTIFIER;

		INITBUFFER(lex_ctx);	/* free some memory */

		/*
		 *  Interned identifier is compared against reserved words, which are
		 *  currently interned into the heap context.  See genstrings.py.
		 *
		 *  Note that an escape in the identifier disables recognition of
		 *  keywords; e.g. "\u0069f = 1;" is a valid statement (assigns to
		 *  identifier named "if").  This is not necessarily compliant,
		 *  see test-dec-escaped-char-in-keyword.js.
		 *
		 *  Note: "get" and "set" are awkward.  They are not officially
		 *  ReservedWords (and indeed e.g. "var set = 1;" is valid), and
		 *  must come out as DUK_TOK_IDENTIFIER.  The compiler needs to
		 *  work around this a bit.
		 */

		i_end = (strict_mode ? DUK_STRIDX_END_RESERVED : DUK_STRIDX_START_STRICT_RESERVED);

		advtok = ADVTOK(0, DUK_TOK_IDENTIFIER);
		if (out_token->num_escapes == 0) {
			for (i = DUK_STRIDX_START_RESERVED; i < i_end; i++) {
				DUK_ASSERT(i >= 0 && i < DUK_HEAP_NUM_STRINGS);
				if (lex_ctx->thr->strs[i] == str) {
					advtok = ADVTOK(0, DUK_STRIDX_TO_TOK(i));
					break;
				}
			}
		}
	} else if (ISDIGIT(x) || (x == '.')) {
		/* Note: decimal number may start with a period, but must be followed by a digit */

		/*
		 *  DecimalLiteral, HexIntegerLiteral, OctalIntegerLiteral
		 *  "pre-parsing", followed by an actual, accurate parser step.
		 *
		 *  Note: the leading sign character ('+' or '-') is -not- part of
		 *  the production in E5 grammar, and that the a DecimalLiteral
		 *  starting with a '0' must be followed by a non-digit.  Leading
		 *  zeroes are syntax errors and must be checked for.
		 *
		 *  FIXME: the two step parsing process is quite awkward, it would
		 *  be more straightforward to allow numconv to parse the longest
		 *  valid prefix (it already does that, it only needs to indicate
		 *  where the input ended).  However, the lexer decodes characters
		 *  using a lookup window, so this is not a trivial change.
		 */

		/* FIXME: because of the final check below (that the literal is not
		 * followed by a digit), this could maybe be simplified, if we bail
		 * out early from a leading zero (and if there are no periods etc).
		 * Maybe too complex.
		 */

		double val;
		int int_only = 0;
		int allow_hex = 0;
		int st;		/* 0=before period/exp,
		                 * 1=after period, before exp
		                 * 2=after exp, allow '+' or '-'
		                 * 3=after exp and exp sign
		                 */
		int s2n_flags;

		INITBUFFER(lex_ctx);
		if (x == '0' && (y == 'x' || y == 'X')) {
			APPENDBUFFER(lex_ctx, x);
			APPENDBUFFER(lex_ctx, y);
			ADVANCE(lex_ctx, 2);
			int_only = 1;
			allow_hex = 1;
#ifdef DUK_USE_OCTAL_SUPPORT
		} else if (!strict_mode && x == '0' && ISDIGIT(y)) {
			/* Note: if DecimalLiteral starts with a '0', it can only be
			 * followed by a period or an exponent indicator which starts
			 * with 'e' or 'E'.  Hence the if-check above ensures that
			 * OctalIntegerLiteral is the only valid NumericLiteral
			 * alternative at this point (even if y is, say, '9').
			 */
	
			APPENDBUFFER(lex_ctx, x);
			ADVANCE(lex_ctx, 1);
			int_only = 1;
#endif
		}

		st = 0;
		for (;;) {
			x = L0();	/* re-lookup curr char on first round */
			if (ISDIGIT(x)) {
				/* Note: intentionally allow leading zeroes here, as the
				 * actual parser will check for them.
				 */
				if (st == 2) {
					st = 3;
				}
			} else if (allow_hex && ISHEXDIGIT(x)) {
				/* Note: 'e' and 'E' are also accepted here. */
				;
			} else if (x == '.') {
				if (st >= 1 || int_only) {
					break;
				} else {
					st = 1;
				}
			} else if (x == 'e' || x == 'E') {
				if (st >= 2 || int_only) {
					break;
				} else {
					st = 2;
				}
			} else if (x == '-' || x == '+') {
				if (st != 2) {
					break;
				} else {
					st = 3;
				}
			} else {
				break;
			}
			APPENDBUFFER(lex_ctx, x);
			ADVANCE(lex_ctx, 1);
		}

		/* FIXME: better coercion */
		internbuffer(lex_ctx, lex_ctx->slot1_idx);

		s2n_flags = DUK_S2N_FLAG_ALLOW_EXP |
		            DUK_S2N_FLAG_ALLOW_FRAC |
		            DUK_S2N_FLAG_ALLOW_NAKED_FRAC |
		            DUK_S2N_FLAG_ALLOW_EMPTY_FRAC |
#ifdef DUK_USE_OCTAL_SUPPORT
		            (strict_mode ? 0 : DUK_S2N_FLAG_ALLOW_AUTO_OCT_INT) |
#endif
		            DUK_S2N_FLAG_ALLOW_AUTO_HEX_INT;

		duk_dup((duk_context *) lex_ctx->thr, lex_ctx->slot1_idx);
		duk_numconv_parse((duk_context *) lex_ctx->thr, 10 /*radix*/, s2n_flags);
		val = duk_to_number((duk_context *) lex_ctx->thr, -1);
		if (DUK_ISNAN(val)) {
			DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR, "invalid numeric literal");
		}
		duk_replace((duk_context *) lex_ctx->thr, lex_ctx->slot1_idx);  /* FIXME: or pop? */

		INITBUFFER(lex_ctx);	/* free some memory */

		/* Section 7.8.3 (note): NumericLiteral must be followed by something other than
		 * IdentifierStart or DecimalDigit.
		 */

		if (ISDIGIT(L0()) || duk_unicode_is_identifier_start(L0())) {
			DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR, "invalid numeric literal");
		}

		out_token->num = val;
		advtok = ADVTOK(0, DUK_TOK_NUMBER);
	} else if (x == '"' || x == '\'') {
		int quote = x;	/* duk_uint8_t type yields larger code */
		int adv;

		INITBUFFER(lex_ctx);
		for (;;) {
			ADVANCE(lex_ctx, 1);	/* eat opening quote on first loop */
			x = L0();
			if (x < 0 || duk_unicode_is_line_terminator(x)) {
				DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
				          "eof or line terminator while parsing string literal");
			}
			if (x == quote) {
				ADVANCE(lex_ctx, 1);	/* eat closing quote */
				break;
			}
			if (x == '\\') {
				/* L0        -> '\' char
				 * L1 ... L5 -> more lookup
				 */

				x = L1();
				y = L2();

				/* How much to advance before next loop; note that next loop
				 * will advance by 1 anyway, so -1 from the total escape
				 * length (e.g. len('\uXXXX') - 1 = 6 - 1).  As a default,
				 * 1 is good.
				 */
				adv = 2 - 1;	/* note: long live range */

				if (x < 0) {
					DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
					          "eof while parsing string literal");
				}
				if (duk_unicode_is_line_terminator(x)) {
					/* line continuation */
					if (x == 0x000d && y == 0x000a) {
						/* CR LF again a special case */
						adv = 3 - 1;
					}
				} else if (x == '\'') {
					APPENDBUFFER(lex_ctx, 0x0027);
				} else if (x == '"') {
					APPENDBUFFER(lex_ctx, 0x0022);
				} else if (x == '\\') {
					APPENDBUFFER(lex_ctx, 0x005c);
				} else if (x == 'b') {
					APPENDBUFFER(lex_ctx, 0x0008);
				} else if (x == 'f') {
					APPENDBUFFER(lex_ctx, 0x000c);
				} else if (x == 'n') {
					APPENDBUFFER(lex_ctx, 0x000a);
				} else if (x == 'r') {
					APPENDBUFFER(lex_ctx, 0x000d);
				} else if (x == 't') {
					APPENDBUFFER(lex_ctx, 0x0009);
				} else if (x == 'v') {
					APPENDBUFFER(lex_ctx, 0x000b);
				} else if (x == 'x') {
					adv = 4 - 1;
					APPENDBUFFER(lex_ctx, decode_hex_escape_from_window(lex_ctx, 2));
				} else if (x == 'u') {
					adv = 6 - 1;
					APPENDBUFFER(lex_ctx, decode_unicode_escape_from_window(lex_ctx, 2));
				} else if (ISDIGIT(x)) {
					int ch = 0;  /* initialized to avoid warnings of unused var */

					/*
					 *  Octal escape or zero escape:
					 *    \0                                     (lookahead not DecimalDigit)
					 *    \1 ... \7                              (lookahead not DecimalDigit)
					 *    \ZeroToThree OctalDigit                (lookahead not DecimalDigit)
					 *    \FourToSeven OctalDigit                (no lookahead restrictions)
					 *    \ZeroToThree OctalDigit OctalDigit     (no lookahead restrictions)
					 *
					 *  Zero escape is part of the standard syntax.  Octal escapes are
					 *  defined in E5 Section B.1.2, and are only allowed in non-strict mode.
					 *  Any other productions starting with a decimal digit are invalid.
					 */

					if (x == '0' && !ISDIGIT(y)) {
						/* Zero escape (also allowed in non-strict mode) */
						ch = 0;
						/* adv = 2 - 1 default OK */
#ifdef DUK_USE_OCTAL_SUPPORT
					} else if (strict_mode) {
						/* No other escape beginning with a digit in strict mode */
						DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
						          "invalid escape while parsing string literal");
					} else if (ISDIGIT03(x) && ISOCTDIGIT(y) && ISOCTDIGIT(L3())) {
						/* Three digit octal escape, digits validated. */
						adv = 4 - 1;
						ch = (hexval(lex_ctx, x) << 6) +
						     (hexval(lex_ctx, y) << 3) +
						     hexval(lex_ctx, L3());
					} else if (((ISDIGIT03(x) && !ISDIGIT(L3())) || ISDIGIT47(x)) &&
					           ISOCTDIGIT(y)) {
						/* Two digit octal escape, digits validated.
						 * 
						 * The if-condition is a bit tricky.  We could catch e.g.
						 * '\039' in the three-digit escape and fail it there (by
					         * validating the digits), but we want to avoid extra
						 * additional validation code.
						 */
						adv = 3 - 1;
						ch = (hexval(lex_ctx, x) << 3) +
						     hexval(lex_ctx, y);
					} else if (ISDIGIT(x) && !ISDIGIT(y)) {
						/* One digit octal escape, digit validated. */
						/* adv = 2 default OK */
						ch = hexval(lex_ctx, x);
#else
					/* fall through to error */
#endif
					} else {
						DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
						          "invalid escape while parsing string literal");
					}

					APPENDBUFFER(lex_ctx, ch);
				} else {
					/* escaped NonEscapeCharacter */
					APPENDBUFFER(lex_ctx, x);
				}
				ADVANCE(lex_ctx, adv);

				/* Track number of escapes; count not really needed but directive
				 * prologues need to detect whether there were any escapes or line
				 * continuations or not.
				 */
				out_token->num_escapes++;
			} else {
				/* part of string */
				APPENDBUFFER(lex_ctx, x);
			}
		}

		internbuffer(lex_ctx, lex_ctx->slot1_idx);
		out_token->str1 = duk_get_hstring((duk_context *) lex_ctx->thr, lex_ctx->slot1_idx);

		INITBUFFER(lex_ctx);	/* free some memory */

		advtok = ADVTOK(0, DUK_TOK_STRING);
	} else if (x < 0) {
		advtok = ADVTOK(0, DUK_TOK_EOF);
	} else {
		DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR, "error parsing token");
	}

	/*
	 *  Shared exit path
	 */

	ADVANCE(lex_ctx, advtok >> 8);
	out_token->t = advtok & 0xff;
	if (out_token->t_nores < 0) {
		out_token->t_nores = out_token->t;
	}
	out_token->end_line = lex_ctx->lines[0];
}

/*
 *  Tokenize input until a non-whitespace, non-lineterm token is found.
 *  Note in the output token whether a lineterm token preceded the starting
 *  point (inclusive) and the result token.  This information is needed for
 *  automatic semicolon insertion.
 *
 *  Future work:
 *
 *    * Merge with parse_input_element_raw() because only this function is
 *      called in practice.
 */

/* FIXME: change mode flags into one flags argument? */

void duk_lexer_parse_js_input_element(duk_lexer_ctx *lex_ctx,
                                      duk_token *out_token,
                                      int strict_mode,
                                      int regexp_mode) {
	int tok;
	int got_lineterm = 0;  /* got lineterm preceding non-whitespace, non-lineterm token */

	for (;;) {
		parse_input_element_raw(lex_ctx, out_token, strict_mode, regexp_mode);
		tok = out_token->t;

		DUK_DDDPRINT("RAWTOKEN: %d (line %d-%d)",
		             tok, out_token->start_line, out_token->end_line);

		if (tok == DUK_TOK_COMMENT) {
			/* single-line comment or multi-line comment without an internal lineterm */
			continue;
		} else if (tok == DUK_TOK_LINETERM) {
			/* lineterm or multi-line comment with an internal lineterm */
			got_lineterm = 1;
			continue;
		} else {
			break;
		}
	}

	out_token->lineterm = got_lineterm;

	/* Automatic semicolon insertion is allowed if a token is preceded
	 * by line terminator(s), or terminates a statement list (right curly
	 * or EOF).
	 */
	if (got_lineterm || tok == DUK_TOK_RCURLY || tok == DUK_TOK_EOF) {
		out_token->allow_auto_semi = 1;
	} else {
		out_token->allow_auto_semi = 0;
	}
}

#ifdef DUK_USE_REGEXP_SUPPORT

/*
 *  Parse a RegExp token.  The grammar is described in E5 Section 15.10.
 *  Terminal constructions (such as quantifiers) are parsed directly here.
 *
 *  0xffffffffU is used as a marker for "infinity" in quantifiers.  Further,
 *  MAX_REGEXP_QUANTIFIER_DIGITS limits the maximum number of digits that
 *  will be accepted for a quantifier.
 */

void duk_lexer_parse_re_token(duk_lexer_ctx *lex_ctx, duk_re_token *out_token) {
	int advtok = 0;  /* init is unnecessary but suppresses "may be used uninitialized" warnings */
	int x, y;

	if (++lex_ctx->token_count >= lex_ctx->token_limit) {
		DUK_ERROR(lex_ctx->thr, DUK_ERR_RANGE_ERROR, "token limit");
		return;  /* unreachable */
	}

	DUK_MEMSET(out_token, 0, sizeof(*out_token));

	x = L0();
	y = L1();

	DUK_DDDPRINT("parsing regexp token, L0=%d, L1=%d", x, y);

	switch (x) {
	case '|': {
		advtok = ADVTOK(1, DUK_RETOK_DISJUNCTION);
		break;
	}
	case '^': {
		advtok = ADVTOK(1, DUK_RETOK_ASSERT_START);
		break;
	}
	case '$': {
		advtok = ADVTOK(1, DUK_RETOK_ASSERT_END);
		break;
	}
	case '?': {
		out_token->qmin = 0;
		out_token->qmax = 1;	
		if (y == '?') {
			advtok = ADVTOK(2, DUK_RETOK_QUANTIFIER);
			out_token->greedy = 0;
		} else {
			advtok = ADVTOK(1, DUK_RETOK_QUANTIFIER);
			out_token->greedy = 1;
		}
		break;
	}
	case '*': {
		out_token->qmin = 0;
		out_token->qmax = DUK_RE_QUANTIFIER_INFINITE;
		if (y == '?') {
			advtok = ADVTOK(2, DUK_RETOK_QUANTIFIER);
			out_token->greedy = 0;
		} else {
			advtok = ADVTOK(1, DUK_RETOK_QUANTIFIER);
			out_token->greedy = 1;
		}
		break;
	}
	case '+': {
		out_token->qmin = 1;
		out_token->qmax = DUK_RE_QUANTIFIER_INFINITE;
		if (y == '?') {
			advtok = ADVTOK(2, DUK_RETOK_QUANTIFIER);
			out_token->greedy = 0;
		} else {
			advtok = ADVTOK(1, DUK_RETOK_QUANTIFIER);
			out_token->greedy = 1;
		}
		break;
	}
	case '{': {
		/* Production allows 'DecimalDigits', including leading zeroes */
		duk_uint32_t val1 = 0;
		duk_uint32_t val2 = DUK_RE_QUANTIFIER_INFINITE;
		int digits = 0;
		for (;;) {
			ADVANCE(lex_ctx, 1);	/* eat '{' on entry */
			x = L0();
			if (ISDIGIT(x)) {
				if (digits >= MAX_REGEXP_QUANTIFIER_DIGITS) {
					DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
					          "invalid regexp quantifier (too many digits)");
				}
				digits++;
				val1 = val1 * 10 + hexval(lex_ctx, x);
			} else if (x == ',') {
				if (val2 != DUK_RE_QUANTIFIER_INFINITE) {
					DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
					          "invalid regexp quantifier (double comma)");
				}
				if (L1() == '}') {
					/* form: { DecimalDigits , }, val1 = min count */
					if (digits == 0) {
						DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
						          "invalid regexp quantifier (missing digits)");
					}
					out_token->qmin = val1;
					out_token->qmax = DUK_RE_QUANTIFIER_INFINITE;
					ADVANCE(lex_ctx, 2);
					break;
				}
				val2 = val1;
				val1 = 0;
				digits = 0;	/* not strictly necessary because of lookahead '}' above */
			} else if (x == '}') {
				if (digits == 0) {
					DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
					          "invalid regexp quantifier (missing digits)");
				}
				if (val2 != DUK_RE_QUANTIFIER_INFINITE) {
					/* val2 = min count, val1 = max count */
					out_token->qmin = val2;
					out_token->qmax = val1;
				} else {
					/* val1 = count */
					out_token->qmin = val1;
					out_token->qmax = val1;
				}
				ADVANCE(lex_ctx, 1);
				break;
			} else {
				DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
				          "invalid regexp quantifier (unknown char)");
			}
		}
		if (L0() == '?') {
			out_token->greedy = 0;
			ADVANCE(lex_ctx, 1);
		} else {
			out_token->greedy = 1;
		}
		advtok = ADVTOK(0, DUK_RETOK_QUANTIFIER);
		break;
	}
	case '.': {
		advtok = ADVTOK(1, DUK_RETOK_ATOM_PERIOD);
		break;
	}
	case '\\': {
		/* The E5.1 specification does not seem to allow IdentifierPart characters
		 * to be used as identity escapes.  Unfortunately this includes '$', which
		 * cannot be escaped as '\$'; it needs to be escaped e.g. as '\u0024'.
		 * Many other implementations (including V8 and Rhino, for instance) do
		 * accept '\$' as a valid identity escape, which is quite pragmatic.
		 * See: test-regexp-identity-escape-dollar.js.
		 */

		advtok = ADVTOK(2, DUK_RETOK_ATOM_CHAR);	/* default: char escape (two chars) */
		if (y == 'b') {
			advtok = ADVTOK(2, DUK_RETOK_ASSERT_WORD_BOUNDARY);
		} else if (y == 'B') {
			advtok = ADVTOK(2, DUK_RETOK_ASSERT_NOT_WORD_BOUNDARY);
		} else if (y == 'f') {
			out_token->num = 0x000c;
		} else if (y == 'n') {
			out_token->num = 0x000a;
		} else if (y == 't') {
			out_token->num = 0x0009;
		} else if (y == 'r') {
			out_token->num = 0x000d;
		} else if (y == 'v') {
			out_token->num = 0x000b;
		} else if (y == 'c') {
			x = L2();
			if ((x >= 'a' && x <= 'z') ||
			    (x >= 'A' && x <= 'Z')) {
				out_token->num = (x % 32);
				advtok = ADVTOK(3, DUK_RETOK_ATOM_CHAR);
			} else {
				DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
				          "invalid regexp control escape");
			}
		} else if (y == 'x') {
			out_token->num = decode_hex_escape_from_window(lex_ctx, 2);
			advtok = ADVTOK(4, DUK_RETOK_ATOM_CHAR);
		} else if (y == 'u') {
			out_token->num = decode_unicode_escape_from_window(lex_ctx, 2);
			advtok = ADVTOK(6, DUK_RETOK_ATOM_CHAR);
		} else if (y == 'd') {
			advtok = ADVTOK(2, DUK_RETOK_ATOM_DIGIT);
		} else if (y == 'D') {
			advtok = ADVTOK(2, DUK_RETOK_ATOM_NOT_DIGIT);
		} else if (y == 's') {
			advtok = ADVTOK(2, DUK_RETOK_ATOM_WHITE);
		} else if (y == 'S') {
			advtok = ADVTOK(2, DUK_RETOK_ATOM_NOT_WHITE);
		} else if (y == 'w') {
			advtok = ADVTOK(2, DUK_RETOK_ATOM_WORD_CHAR);
		} else if (y == 'W') {
			advtok = ADVTOK(2, DUK_RETOK_ATOM_NOT_WORD_CHAR);
		} else if (ISDIGIT(y)) {
			/* E5 Section 15.10.2.11 */
			if (y == '0') {
				if (ISDIGIT(L2())) {
					DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
					          "invalid regexp escape");
				}
				out_token->num = 0x0000;
				advtok = ADVTOK(2, DUK_RETOK_ATOM_CHAR);
			} else {
				/* FIXME: shared parsing? */
				duk_uint32_t val = 0;
				int i;
				for (i = 0; ; i++) {
					if (i >= MAX_REGEXP_DECIMAL_ESCAPE_DIGITS) {
						DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
						          "invalid regexp escape (decimal escape too long)");
					}
					ADVANCE(lex_ctx, 1);	/* eat backslash on entry */
					x = L0();
					if (!ISDIGIT(x)) {
						break;
					}
					val = val * 10 + hexval(lex_ctx, x);
				}
				/* L0() cannot be a digit, because the loop doesn't terminate if it is */
				advtok = ADVTOK(0, DUK_RETOK_ATOM_BACKREFERENCE);
				out_token->num = val;
			}
		} else if (!duk_unicode_is_identifier_part(y) ||
		           y == DUK_UNICODE_CP_ZWNJ ||
		           y == DUK_UNICODE_CP_ZWJ ||
		           y == '$') {
			/* IdentityEscape, with dollar added as a valid additional
			 * non-standard escape (see test-regexp-identity-escape-dollar.js).
			 */
			out_token->num = y;
		} else {
			DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
			          "invalid regexp escape");
		}
		break;
	}
	case '(': {
		/* FIXME: naming is inconsistent: ATOM_END_GROUP ends an ASSERT_START_LOOKAHEAD */

		if (y == '?') {
			if (L2() == '=') {
				/* (?= */
				advtok = ADVTOK(3, DUK_RETOK_ASSERT_START_POS_LOOKAHEAD);
			} else if (L2() == '!') {
				/* (?! */
				advtok = ADVTOK(3, DUK_RETOK_ASSERT_START_NEG_LOOKAHEAD);
			} else if (L2() == ':') {
				/* (?: */
				advtok = ADVTOK(3, DUK_RETOK_ATOM_START_NONCAPTURE_GROUP);
			}
		} else {
			/* ( */
			advtok = ADVTOK(1, DUK_RETOK_ATOM_START_CAPTURE_GROUP);
		}
		break;
	}
	case ')': {
		advtok = ADVTOK(1, DUK_RETOK_ATOM_END_GROUP);
		break;
	}
	case '[': {
		/*
		 *  To avoid creating a heavy intermediate value for the list of ranges,
		 *  only the start token ('[' or '[^') is parsed here.  The regexp
		 *  compiler parses the ranges itself.
		 */
		advtok = ADVTOK(1, DUK_RETOK_ATOM_START_CHARCLASS);
		if (y == '^') {
			advtok = ADVTOK(2, DUK_RETOK_ATOM_START_CHARCLASS_INVERTED);
		}
		break;
	}
	case ']':
	case '}': {
		/* Although these could be parsed as PatternCharacters unambiguously (here),
		 * E5 Section 15.10.1 grammar explicitly forbids these as PatternCharacters.
		 */
		DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
		          "invalid regexp character");
		break;
	}
	case -1: {
		/* EOF */
		advtok = ADVTOK(0, DUK_TOK_EOF);
		break;
	}
	default: {
		/* PatternCharacter, all excluded characters are matched by cases above */
		advtok = ADVTOK(1, DUK_RETOK_ATOM_CHAR);
		out_token->num = x;
		break;
	}
	}

	/*
	 *  Shared exit path
	 */

	ADVANCE(lex_ctx, advtok >> 8);
	out_token->t = advtok & 0xff;
}

/*
 *  Special parser for character classes; calls callback for every
 *  range parsed and returns the number of ranges present.
 */

/* FIXME: this duplicates functionality in duk_regexp.c where a similar loop is
 * required anyway.  We could use that BUT we need to update the regexp compiler
 * 'nranges' too.  Work this out a bit more cleanly to save space.
 */

/* FIXME: the handling of character range detection is a bit convoluted.
 * Try to simplify and make smaller.
 */

/* FIXME: logic for handling character ranges is now incorrect, it will accept
 * e.g. [\d-z] whereas it should croak from it?  SMJS accepts this too, though.
 *
 * Needs a read through and a lot of additional tests.
 */

static void emit_u16_direct_ranges(duk_lexer_ctx *lex_ctx,
                                   duk_re_range_callback gen_range,
                                   void *userdata,
                                   duk_uint16_t *ranges,
                                   int num) {
	duk_uint16_t *ranges_end;

	DUK_UNREF(lex_ctx);

	ranges_end = ranges + num;
	while (ranges < ranges_end) {
		/* mark range 'direct', bypass canonicalization (see Wiki) */
		gen_range(userdata, (duk_codepoint_t) ranges[0], (duk_codepoint_t) ranges[1], 1);
		ranges += 2;
	}
}

void duk_lexer_parse_re_ranges(duk_lexer_ctx *lex_ctx, duk_re_range_callback gen_range, void *userdata) {
	duk_int32_t start = -1;
	int dash = 0;
	duk_int32_t ch;

	DUK_DDPRINT("parsing regexp ranges");

	for (;;) {
		int x;

		x = L0();
		ADVANCE(lex_ctx, 1);

		ch = -1;  /* not strictly necessary, but avoids "uninitialized variable" warnings */

		if (x < 0) {
			DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
			          "eof while parsing character class");
		} else if (x == ']') {
			DUK_ASSERT(!dash);	/* lookup should prevent this */
			if (start >= 0) {
				gen_range(userdata, (duk_codepoint_t) start, (duk_codepoint_t) start, 0);
			}
			break;
		} else if (x == '-') {
			if (start >= 0 && !dash && L0() != ']') {
				/* '-' as a range indicator */
				dash = 1;
				continue;
			} else {
				/* '-' verbatim */
				ch = x;
			}
		} else if (x == '\\') {
			/*
			 *  The escapes are same as outside a character class, except that \b has a
			 *  different meaning, and \B and backreferences are prohibited (see E5
			 *  Section 15.10.2.19).  However, it's difficult to share code because we
			 *  handle e.g. "\n" very differently: here we generate a single character
			 *  range for it.
			 */

			x = L0();
			ADVANCE(lex_ctx, 1);

			if (x == 'b') {
				/* Note: '\b' in char class is different than outside (assertion),
				 * '\B' is not allowed and is caught by the duk_unicode_is_identifier_part()
				 * check below.
				 */
				ch = 0x0008;
			} else if (x == 'f') {
				ch = 0x000c;
			} else if (x == 'n') {
				ch = 0x000a;
			} else if (x == 't') {
				ch = 0x0009;
			} else if (x == 'r') {
				ch = 0x000d;
			} else if (x == 'v') {
				ch = 0x000b;
			} else if (x == 'c') {
				x = L0();
				ADVANCE(lex_ctx, 1);
				if ((x >= 'a' && x <= 'z') ||
				    (x >= 'A' && x <= 'Z')) {
					ch = (x % 32);
				} else {
					DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
					          "invalid regexp control escape");
					return;  /* never reached, but avoids warnings of
					          * potentially unused variables.
					          */
				}
			} else if (x == 'x') {
				ch = decode_hex_escape_from_window(lex_ctx, 0);
				ADVANCE(lex_ctx, 2);
			} else if (x == 'u') {
				ch = decode_unicode_escape_from_window(lex_ctx, 0);
				ADVANCE(lex_ctx, 4);
			} else if (x == 'd') {
				emit_u16_direct_ranges(lex_ctx,
				                       gen_range,
				                       userdata,
				                       duk_unicode_re_ranges_digit,
				                       sizeof(duk_unicode_re_ranges_digit) / sizeof(duk_uint16_t));
				ch = -1;
			} else if (x == 'D') {
				emit_u16_direct_ranges(lex_ctx,
				                       gen_range,
				                       userdata,
				                       duk_unicode_re_ranges_not_digit,
				                       sizeof(duk_unicode_re_ranges_not_digit) / sizeof(duk_uint16_t));
				ch = -1;
			} else if (x == 's') {
				emit_u16_direct_ranges(lex_ctx,
				                       gen_range,
				                       userdata,
				                       duk_unicode_re_ranges_white,
				                       sizeof(duk_unicode_re_ranges_white) / sizeof(duk_uint16_t));
				ch = -1;
			} else if (x == 'S') {
				emit_u16_direct_ranges(lex_ctx,
				                       gen_range,
				                       userdata,
				                       duk_unicode_re_ranges_not_white,
				                       sizeof(duk_unicode_re_ranges_not_white) / sizeof(duk_uint16_t));
				ch = -1;
			} else if (x == 'w') {
				emit_u16_direct_ranges(lex_ctx,
				                       gen_range,
				                       userdata,
				                       duk_unicode_re_ranges_wordchar,
				                       sizeof(duk_unicode_re_ranges_wordchar) / sizeof(duk_uint16_t));
				ch = -1;
			} else if (x == 'W') {
				emit_u16_direct_ranges(lex_ctx,
				                       gen_range,
				                       userdata,
				                       duk_unicode_re_ranges_not_wordchar,
				                       sizeof(duk_unicode_re_ranges_not_wordchar) / sizeof(duk_uint16_t));
				ch = -1;
			} else if (ISDIGIT(x)) {
				/* DecimalEscape, only \0 is allowed, no leading zeroes are allowed */
				if (x == 0 && !ISDIGIT(L0())) {
					ch = 0x0000;
				} else {
					DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
					          "invalid decimal escape");
				}
			} else if (!duk_unicode_is_identifier_part(x)) {
				/* IdentityEscape */
				ch = x;
			} else {
				DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
				          "invalid regexp escape");
			}
		} else {
			/* character represents itself */
			ch = x;
		}

		/* ch is a literal character here or -1 if parsed entity was
		 * an escape such as "\s".
		 */

		if (ch < 0) {
			/* multi-character sets not allowed as part of ranges, see
			 * E5 Section 15.10.2.15, abstract operation CharacterRange.
			 */
			if (start >= 0) {
				if (dash) {
					DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
					          "invalid range");
				} else {
					gen_range(userdata, (duk_codepoint_t) start, (duk_codepoint_t) start, 0);
					start = -1;
					/* dash is already 0 */
				}
			}
		} else {
			if (start >= 0) {
				if (dash) {
					if (start > ch) {
						DUK_ERROR(lex_ctx->thr, DUK_ERR_SYNTAX_ERROR,
						          "invalid range");
					}
					gen_range(userdata, (duk_codepoint_t) start, (duk_codepoint_t) ch, 0);
					start = -1;
					dash = 0;
				} else {
					gen_range(userdata, (duk_codepoint_t) start, (duk_codepoint_t) start, 0);
					start = ch;
					/* dash is already 0 */
				}
			} else {
				start = ch;
			}
		}
	}

	return;
}

#endif  /* DUK_USE_REGEXP_SUPPORT */


local L = {}

L.lexer = "cpp"

L.singleLineComment = "// "

L.extensions = {
	"c",
	"lex",
}

-- Keyword set 0 -> SCE_C_WORD (styled as a bold keyword)
-- Keyword set 1 -> SCE_C_WORD2 (styled as a plain word: types and modifiers)
-- Keyword set 5 -> task markers
L.keywords = {
	[0] = "enum struct typedef union",
	[1] = "_Alignas _Alignof _Atomic _Bool _Complex _Generic _Imaginary _Noreturn _Static_assert _Thread_local and and_eq alignas alignof auto bitand bitor bool break case char char16_t char32_t clock_t compl const continue default do double else extern float for goto if imaginary inline int int8_t int16_t int32_t int64_t int_fast8_t int_fast16_t int_fast32_t int_fast64_t int_least8_t int_least16_t int_least32_t int_least64_t intmax_t intptr_t long not not_eq noreturn NULL offsetof or or_eq register restrict return short signed sizeof static switch thread_local uint8_t uint16_t uint32_t uint64_t uint_fast8_t uint_fast16_t uint_fast32_t uint_fast64_t uint_least8_t uint_least16_t uint_least32_t uint_least64_t uintmax_t uintptr_t unsigned void volatile while wchar_t xor xor_eq dma_addr_t gfp_t irqreturn_t loff_t phys_addr_t resource_size_t s8 s16 s32 s64 spinlock_t u8 u16 u32 u64 u_long uint",
	[5] = "FIXME TODO XXX HACK NOTE",
}

-- Palette follows the classic Source Insight look:
--   green            keywords, types, modifiers, macro calls
--   bold navy        identifiers (variables, function names), declaration keywords
--   purple           operators and punctuation
--   red              numbers and enum-like constants
--   dark red (maroon) strings
-- Note: the C lexer has no way of knowing which words are project typedefs, so the
-- shared types of common fixed width conventions (u32, loff_t, ...) are listed here
-- by name to keep declarations looking like they do in Source Insight.
L.styles = {
	["PREPROCESSOR"] = {
		id = 9,
		fgColor = rgb(0x804000),
		bgColor = rgb(0xFFFFFF),
	},
	["DEFAULT"] = {
		id = 11,
		fgColor = rgb(0x000080),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["INSTRUCTION WORD"] = {
		id = 5,
		fgColor = rgb(0x000080),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["TYPE WORD"] = {
		id = 16,
		fgColor = rgb(0x008000),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 0,
	},
	["GLOBAL CLASS"] = {
		id = 19,
		fgColor = rgb(0x008000),
		bgColor = rgb(0xFFFFFF),
	},
	["NUMBER"] = {
		id = 4,
		fgColor = rgb(0xFF0000),
		bgColor = rgb(0xFFFFFF),
	},
	["STRING"] = {
		id = 6,
		fgColor = rgb(0x800000),
		bgColor = rgb(0xFFFFFF),
	},
	["CHARACTER"] = {
		id = 7,
		fgColor = rgb(0x800000),
		bgColor = rgb(0xFFFFFF),
	},
	["OPERATOR"] = {
		id = 10,
		fgColor = rgb(0x800080),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 0,
	},
	["VERBATIM"] = {
		id = 13,
		fgColor = rgb(0x800000),
		bgColor = rgb(0xFFFFFF),
	},
	["REGEX"] = {
		id = 14,
		fgColor = rgb(0x000080),
		bgColor = rgb(0xFFFFFF),
	},
	["COMMENT"] = {
		id = 1,
		fgColor = rgb(0x008000),
		bgColor = rgb(0xFFFFFF),
	},
	["COMMENT LINE"] = {
		id = 2,
		fgColor = rgb(0x008000),
		bgColor = rgb(0xFFFFFF),
	},
	["COMMENT DOC"] = {
		id = 3,
		fgColor = rgb(0x008080),
		bgColor = rgb(0xFFFFFF),
	},
	["COMMENT LINE DOC"] = {
		id = 15,
		fgColor = rgb(0x008080),
		bgColor = rgb(0xFFFFFF),
	},
	["COMMENT DOC KEYWORD"] = {
		id = 17,
		fgColor = rgb(0x008080),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
	["COMMENT DOC KEYWORD ERROR"] = {
		id = 18,
		fgColor = rgb(0x008080),
		bgColor = rgb(0xFFFFFF),
	},
	["PREPROCESSOR COMMENT"] = {
		id = 23,
		fgColor = rgb(0x008000),
		bgColor = rgb(0xFFFFFF),
	},
	["PREPROCESSOR COMMENT DOC"] = {
		id = 24,
		fgColor = rgb(0x008080),
		bgColor = rgb(0xFFFFFF),
	},
	["STRING RAW"] = {
		id = 20,
		fgColor = rgb(0x800000),
		bgColor = rgb(0xFFFFFF),
	},
	["TRIPLE VERBATIM"] = {
		id = 21,
		fgColor = rgb(0x800000),
		bgColor = rgb(0xFFFFFF),
	},
	["HASH QUOTED STRING"] = {
		id = 22,
		fgColor = rgb(0x800000),
		bgColor = rgb(0xFFFFFF),
	},
	["TASK MARKER"] = {
		id = 26,
		fgColor = rgb(0x008000),
		bgColor = rgb(0xFFFFFF),
		fontStyle = 1,
	},
}
return L

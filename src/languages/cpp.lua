local L = {}

L.lexer = "cpp"

L.singleLineComment = "// "

L.extensions = {
	"cpp",
	"cxx",
	"cc",
	"h",
	"hh",
	"hpp",
	"hxx",
	"ino",
}

L.properties = {
	["fold.cpp.comment.explicit"] = "0",
	["lexer.cpp.track.preprocessor"] = "0",
	["lexer.cpp.escape.sequence"] = "1",
}

-- Keyword set 0 -> SCE_C_WORD (styled as a bold keyword)
-- Keyword set 1 -> SCE_C_WORD2 (styled as a plain word: types and modifiers)
-- Keyword set 2 -> documentation comment keywords
-- Keyword set 5 -> task markers
L.keywords = {
	[0] = "class enum namespace operator struct template typedef typename union using",
	[1] = "alignas alignof and and_eq asm auto bitand bitor bool break case catch char char16_t char32_t clock_t compl concept const const_cast consteval constexpr constinit continue decltype default delete do double dynamic_cast else explicit export extern false final float for friend goto if inline int int8_t int16_t int32_t int64_t int_fast8_t int_fast16_t int_fast32_t int_fast64_t intmax_t intptr_t long mutable new noexcept not not_eq nullptr or or_eq override private protected ptrdiff_t public register reinterpret_cast requires return short signed sizeof size_t ssize_t static static_assert static_cast switch this thread_local throw time_t true try typeid uint8_t uint16_t uint32_t uint64_t uint_fast8_t uint_fast16_t uint_fast32_t uint_fast64_t uintmax_t uintptr_t unsigned virtual void volatile wchar_t while xor xor_eq dma_addr_t gfp_t irqreturn_t loff_t phys_addr_t resource_size_t s8 s16 s32 s64 spinlock_t u8 u16 u32 u64 u_long uint NULL",
	[2] = "a addindex addtogroup anchor arg attention author authors b brief bug c callergraph callgraph category cite class code cond copybrief copydetails copydoc copyright date def defgroup deprecated details diafile dir docbookonly dontinclude dot dotfile e else elseif em endcode endcond enddocbookonly enddot endhtmlonly endif endinternal endlatexonly endlink endmanonly endmsc endparblock endrtfonly endsecreflist enduml endverbatim endxmlonly enum example exception extends f$ f[ f] file fn f{ f} headerfile hidecallergraph hidecallgraph hideinitializer htmlinclude htmlonly idlexcept if ifnot image implements include includelineno ingroup interface internal invariant latexinclude latexonly li line link mainpage manonly memberof msc mscfile n name namespace nosubgrouping note overload p package page par paragraph param parblock post pre private privatesection property protected protectedsection protocol public publicsection pure ref refitem related relatedalso relates relatesalso remark remarks result return returns retval rtfonly sa secreflist section see short showinitializer since skip skipline snippet startuml struct subpage subsection subsubsection tableofcontents test throw throws todo tparam typedef union until var verbatim verbinclude version vhdlflow warning weakgroup xmlonly xrefitem",
	[5] = "FIXME TODO XXX HACK NOTE",
}

-- Palette follows the classic Source Insight look:
--   green            keywords, types, modifiers, macro calls
--   bold navy        identifiers (variables, function names), declaration keywords
--   purple           operators and punctuation
--   red              numbers and enum-like constants
--   dark red (maroon) strings
-- Keyword set 1 carries the common kernel style fixed width types as well, so that
-- code such as "static inline void foo(u32 bar)" reads the same way it does in
-- Source Insight.
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

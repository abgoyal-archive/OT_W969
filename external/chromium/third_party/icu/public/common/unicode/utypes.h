
#ifndef UTYPES_H
#define UTYPES_H


#include "unicode/umachine.h"
#include "unicode/utf.h"
#include "unicode/uversion.h"
#include "unicode/uconfig.h"

#if !U_DEFAULT_SHOW_DRAFT && !defined(U_SHOW_DRAFT_API)
#define U_HIDE_DRAFT_API 1
#endif

#ifdef U_HIDE_DRAFT_API
#include "unicode/udraft.h"
#endif

#ifdef U_HIDE_DEPRECATED_API
#include "unicode/udeprctd.h"
#endif

#ifdef U_HIDE_DEPRECATED_API
#include "unicode/uobslete.h"
#endif

#ifdef U_HIDE_INTERNAL_API
#include "unicode/uintrnal.h"
#endif

#ifdef U_HIDE_SYSTEM_API
#include "unicode/usystem.h"
#endif


/*===========================================================================*/
/* char Character set family                                                 */
/*===========================================================================*/

#define U_ASCII_FAMILY 0

#define U_EBCDIC_FAMILY 1


#ifndef U_CHARSET_FAMILY
#   define U_CHARSET_FAMILY 0
#endif

#ifndef U_CHARSET_IS_UTF8
#   define U_CHARSET_IS_UTF8 0
#endif

/*===========================================================================*/
/* ICUDATA naming scheme                                                     */
/*===========================================================================*/


#if U_CHARSET_FAMILY
#   if U_IS_BIG_ENDIAN
   /* EBCDIC - should always be BE */
#     define U_ICUDATA_TYPE_LETTER "e"
#     define U_ICUDATA_TYPE_LITLETTER e
#   else
#     error "Don't know what to do with little endian EBCDIC!"
#     define U_ICUDATA_TYPE_LETTER "x"
#     define U_ICUDATA_TYPE_LITLETTER x
#   endif
#else
#   if U_IS_BIG_ENDIAN
      /* Big-endian ASCII */
#     define U_ICUDATA_TYPE_LETTER "b"
#     define U_ICUDATA_TYPE_LITLETTER b
#   else
      /* Little-endian ASCII */
#     define U_ICUDATA_TYPE_LETTER "l"
#     define U_ICUDATA_TYPE_LITLETTER l
#   endif
#endif

#define U_ICUDATA_NAME    "icudt" U_ICU_VERSION_SHORT U_ICUDATA_TYPE_LETTER


#define U_ICUDATA_ENTRY_POINT  U_DEF2_ICUDATA_ENTRY_POINT(U_ICU_VERSION_MAJOR_NUM, U_ICU_VERSION_MINOR_NUM)

#define U_DEF2_ICUDATA_ENTRY_POINT(major, minor) U_DEF_ICUDATA_ENTRY_POINT(major, minor)
#define U_DEF_ICUDATA_ENTRY_POINT(major, minor) icudt##major##minor##_dat

#if defined(OS390) && (__COMPILER_VER__ < 0x41020000) && defined(XP_CPLUSPLUS)
#    define U_CALLCONV __cdecl
#else
#    define U_CALLCONV U_EXPORT2
#endif

#ifndef NULL
#ifdef XP_CPLUSPLUS
#define NULL    0
#else
#define NULL    ((void *)0)
#endif
#endif

/*===========================================================================*/
/* Calendar/TimeZone data types                                              */
/*===========================================================================*/

typedef double UDate;

/** The number of milliseconds per second @stable ICU 2.0 */
#define U_MILLIS_PER_SECOND        (1000)
/** The number of milliseconds per minute @stable ICU 2.0 */
#define U_MILLIS_PER_MINUTE       (60000)
/** The number of milliseconds per hour @stable ICU 2.0 */
#define U_MILLIS_PER_HOUR       (3600000)
/** The number of milliseconds per day @stable ICU 2.0 */
#define U_MILLIS_PER_DAY       (86400000)


/*===========================================================================*/
/* UClassID-based RTTI */
/*===========================================================================*/

typedef void* UClassID;

/*===========================================================================*/
/* Shared library/DLL import-export API control                              */
/*===========================================================================*/










#if defined(U_COMBINED_IMPLEMENTATION)
#define U_DATA_API     U_EXPORT
#define U_COMMON_API   U_EXPORT
#define U_I18N_API     U_EXPORT
#define U_LAYOUT_API   U_EXPORT
#define U_LAYOUTEX_API U_EXPORT
#define U_IO_API       U_EXPORT
#define U_TOOLUTIL_API U_EXPORT
#elif defined(U_STATIC_IMPLEMENTATION)
#define U_DATA_API
#define U_COMMON_API
#define U_I18N_API
#define U_LAYOUT_API
#define U_LAYOUTEX_API
#define U_IO_API
#define U_TOOLUTIL_API
#elif defined(U_COMMON_IMPLEMENTATION)
#define U_DATA_API     U_IMPORT
#define U_COMMON_API   U_EXPORT
#define U_I18N_API     U_IMPORT
#define U_LAYOUT_API   U_IMPORT
#define U_LAYOUTEX_API U_IMPORT
#define U_IO_API       U_IMPORT
#define U_TOOLUTIL_API U_IMPORT
#elif defined(U_I18N_IMPLEMENTATION)
#define U_DATA_API     U_IMPORT
#define U_COMMON_API   U_IMPORT
#define U_I18N_API     U_EXPORT
#define U_LAYOUT_API   U_IMPORT
#define U_LAYOUTEX_API U_IMPORT
#define U_IO_API       U_IMPORT
#define U_TOOLUTIL_API U_IMPORT
#elif defined(U_LAYOUT_IMPLEMENTATION)
#define U_DATA_API     U_IMPORT
#define U_COMMON_API   U_IMPORT
#define U_I18N_API     U_IMPORT
#define U_LAYOUT_API   U_EXPORT
#define U_LAYOUTEX_API U_IMPORT
#define U_IO_API       U_IMPORT
#define U_TOOLUTIL_API U_IMPORT
#elif defined(U_LAYOUTEX_IMPLEMENTATION)
#define U_DATA_API     U_IMPORT
#define U_COMMON_API   U_IMPORT
#define U_I18N_API     U_IMPORT
#define U_LAYOUT_API   U_IMPORT
#define U_LAYOUTEX_API U_EXPORT
#define U_IO_API       U_IMPORT
#define U_TOOLUTIL_API U_IMPORT
#elif defined(U_IO_IMPLEMENTATION)
#define U_DATA_API     U_IMPORT
#define U_COMMON_API   U_IMPORT
#define U_I18N_API     U_IMPORT
#define U_LAYOUT_API   U_IMPORT
#define U_LAYOUTEX_API U_IMPORT
#define U_IO_API       U_EXPORT
#define U_TOOLUTIL_API U_IMPORT
#elif defined(U_TOOLUTIL_IMPLEMENTATION)
#define U_DATA_API     U_IMPORT
#define U_COMMON_API   U_IMPORT
#define U_I18N_API     U_IMPORT
#define U_LAYOUT_API   U_IMPORT
#define U_LAYOUTEX_API U_IMPORT
#define U_IO_API       U_IMPORT
#define U_TOOLUTIL_API U_EXPORT
#else
#define U_DATA_API     U_IMPORT
#define U_COMMON_API   U_IMPORT
#define U_I18N_API     U_IMPORT
#define U_LAYOUT_API   U_IMPORT
#define U_LAYOUTEX_API U_IMPORT
#define U_IO_API       U_IMPORT
#define U_TOOLUTIL_API U_IMPORT
#endif

#ifdef __cplusplus
#define U_STANDARD_CPP_NAMESPACE        ::
#else
#define U_STANDARD_CPP_NAMESPACE
#endif


/*===========================================================================*/
/* Global delete operator                                                    */
/*===========================================================================*/

#if defined(XP_CPLUSPLUS) && defined(U_WINDOWS) && U_DEBUG && U_OVERRIDE_CXX_ALLOCATION && (_MSC_VER>=1200) && !defined(U_STATIC_IMPLEMENTATION) && (defined(U_COMMON_IMPLEMENTATION) || defined(U_I18N_IMPLEMENTATION) || defined(U_IO_IMPLEMENTATION) || defined(U_LAYOUT_IMPLEMENTATION) || defined(U_LAYOUTEX_IMPLEMENTATION))

#ifndef U_HIDE_INTERNAL_API
inline void *
operator new(size_t /*size*/) {
    char *q=NULL;
    *q=5; /* break it */
    return q;
}

#ifdef _Ret_bytecap_
/* This is only needed to suppress a Visual C++ 2008 warning for operator new[]. */
_Ret_bytecap_(_Size)
#endif
inline void *
operator new[](size_t /*size*/) {
    char *q=NULL;
    *q=5; /* break it */
    return q;
}

inline void
operator delete(void * /*p*/) {
    char *q=NULL;
    *q=5; /* break it */
}

inline void
operator delete[](void * /*p*/) {
    char *q=NULL;
    *q=5; /* break it */
}

#endif /* U_HIDE_INTERNAL_API */
#endif

/*===========================================================================*/
/* UErrorCode */
/*===========================================================================*/

typedef enum UErrorCode {
    /* The ordering of U_ERROR_INFO_START Vs U_USING_FALLBACK_WARNING looks weird
     * and is that way because VC++ debugger displays first encountered constant,
     * which is not the what the code is used for
     */

    U_USING_FALLBACK_WARNING  = -128,   /**< A resource bundle lookup returned a fallback result (not an error) */

    U_ERROR_WARNING_START     = -128,   /**< Start of information results (semantically successful) */

    U_USING_DEFAULT_WARNING   = -127,   /**< A resource bundle lookup returned a result from the root locale (not an error) */

    U_SAFECLONE_ALLOCATED_WARNING = -126, /**< A SafeClone operation required allocating memory (informational only) */

    U_STATE_OLD_WARNING       = -125,   /**< ICU has to use compatibility layer to construct the service. Expect performance/memory usage degradation. Consider upgrading */

    U_STRING_NOT_TERMINATED_WARNING = -124,/**< An output string could not be NUL-terminated because output length==destCapacity. */

    U_SORT_KEY_TOO_SHORT_WARNING = -123, /**< Number of levels requested in getBound is higher than the number of levels in the sort key */

    U_AMBIGUOUS_ALIAS_WARNING = -122,   /**< This converter alias can go to different converter implementations */

    U_DIFFERENT_UCA_VERSION = -121,     /**< ucol_open encountered a mismatch between UCA version and collator image version, so the collator was constructed from rules. No impact to further function */

    U_ERROR_WARNING_LIMIT,              /**< This must always be the last warning value to indicate the limit for UErrorCode warnings (last warning code +1) */


    U_ZERO_ERROR              =  0,     /**< No error, no warning. */

    U_ILLEGAL_ARGUMENT_ERROR  =  1,     /**< Start of codes indicating failure */
    U_MISSING_RESOURCE_ERROR  =  2,     /**< The requested resource cannot be found */
    U_INVALID_FORMAT_ERROR    =  3,     /**< Data format is not what is expected */
    U_FILE_ACCESS_ERROR       =  4,     /**< The requested file cannot be found */
    U_INTERNAL_PROGRAM_ERROR  =  5,     /**< Indicates a bug in the library code */
    U_MESSAGE_PARSE_ERROR     =  6,     /**< Unable to parse a message (message format) */
    U_MEMORY_ALLOCATION_ERROR =  7,     /**< Memory allocation error */
    U_INDEX_OUTOFBOUNDS_ERROR =  8,     /**< Trying to access the index that is out of bounds */
    U_PARSE_ERROR             =  9,     /**< Equivalent to Java ParseException */
    U_INVALID_CHAR_FOUND      = 10,     /**< Character conversion: Unmappable input sequence. In other APIs: Invalid character. */
    U_TRUNCATED_CHAR_FOUND    = 11,     /**< Character conversion: Incomplete input sequence. */
    U_ILLEGAL_CHAR_FOUND      = 12,     /**< Character conversion: Illegal input sequence/combination of input units. */
    U_INVALID_TABLE_FORMAT    = 13,     /**< Conversion table file found, but corrupted */
    U_INVALID_TABLE_FILE      = 14,     /**< Conversion table file not found */
    U_BUFFER_OVERFLOW_ERROR   = 15,     /**< A result would not fit in the supplied buffer */
    U_UNSUPPORTED_ERROR       = 16,     /**< Requested operation not supported in current context */
    U_RESOURCE_TYPE_MISMATCH  = 17,     /**< an operation is requested over a resource that does not support it */
    U_ILLEGAL_ESCAPE_SEQUENCE = 18,     /**< ISO-2022 illlegal escape sequence */
    U_UNSUPPORTED_ESCAPE_SEQUENCE = 19, /**< ISO-2022 unsupported escape sequence */
    U_NO_SPACE_AVAILABLE      = 20,     /**< No space available for in-buffer expansion for Arabic shaping */
    U_CE_NOT_FOUND_ERROR      = 21,     /**< Currently used only while setting variable top, but can be used generally */
    U_PRIMARY_TOO_LONG_ERROR  = 22,     /**< User tried to set variable top to a primary that is longer than two bytes */
    U_STATE_TOO_OLD_ERROR     = 23,     /**< ICU cannot construct a service from this state, as it is no longer supported */
    U_TOO_MANY_ALIASES_ERROR  = 24,     /**< There are too many aliases in the path to the requested resource.
                                             It is very possible that a circular alias definition has occured */
    U_ENUM_OUT_OF_SYNC_ERROR  = 25,     /**< UEnumeration out of sync with underlying collection */
    U_INVARIANT_CONVERSION_ERROR = 26,  /**< Unable to convert a UChar* string to char* with the invariant converter. */
    U_INVALID_STATE_ERROR     = 27,     /**< Requested operation can not be completed with ICU in its current state */
    U_COLLATOR_VERSION_MISMATCH = 28,   /**< Collator version is not compatible with the base version */
    U_USELESS_COLLATOR_ERROR  = 29,     /**< Collator is options only and no base is specified */
    U_NO_WRITE_PERMISSION     = 30,     /**< Attempt to modify read-only or constant data. */

    U_STANDARD_ERROR_LIMIT,             /**< This must always be the last value to indicate the limit for standard errors */
    /*
     * the error code range 0x10000 0x10100 are reserved for Transliterator
     */
    U_BAD_VARIABLE_DEFINITION=0x10000,/**< Missing '$' or duplicate variable name */
    U_PARSE_ERROR_START = 0x10000,    /**< Start of Transliterator errors */
    U_MALFORMED_RULE,                 /**< Elements of a rule are misplaced */
    U_MALFORMED_SET,                  /**< A UnicodeSet pattern is invalid*/
    U_MALFORMED_SYMBOL_REFERENCE,     /**< UNUSED as of ICU 2.4 */
    U_MALFORMED_UNICODE_ESCAPE,       /**< A Unicode escape pattern is invalid*/
    U_MALFORMED_VARIABLE_DEFINITION,  /**< A variable definition is invalid */
    U_MALFORMED_VARIABLE_REFERENCE,   /**< A variable reference is invalid */
    U_MISMATCHED_SEGMENT_DELIMITERS,  /**< UNUSED as of ICU 2.4 */
    U_MISPLACED_ANCHOR_START,         /**< A start anchor appears at an illegal position */
    U_MISPLACED_CURSOR_OFFSET,        /**< A cursor offset occurs at an illegal position */
    U_MISPLACED_QUANTIFIER,           /**< A quantifier appears after a segment close delimiter */
    U_MISSING_OPERATOR,               /**< A rule contains no operator */
    U_MISSING_SEGMENT_CLOSE,          /**< UNUSED as of ICU 2.4 */
    U_MULTIPLE_ANTE_CONTEXTS,         /**< More than one ante context */
    U_MULTIPLE_CURSORS,               /**< More than one cursor */
    U_MULTIPLE_POST_CONTEXTS,         /**< More than one post context */
    U_TRAILING_BACKSLASH,             /**< A dangling backslash */
    U_UNDEFINED_SEGMENT_REFERENCE,    /**< A segment reference does not correspond to a defined segment */
    U_UNDEFINED_VARIABLE,             /**< A variable reference does not correspond to a defined variable */
    U_UNQUOTED_SPECIAL,               /**< A special character was not quoted or escaped */
    U_UNTERMINATED_QUOTE,             /**< A closing single quote is missing */
    U_RULE_MASK_ERROR,                /**< A rule is hidden by an earlier more general rule */
    U_MISPLACED_COMPOUND_FILTER,      /**< A compound filter is in an invalid location */
    U_MULTIPLE_COMPOUND_FILTERS,      /**< More than one compound filter */
    U_INVALID_RBT_SYNTAX,             /**< A "::id" rule was passed to the RuleBasedTransliterator parser */
    U_INVALID_PROPERTY_PATTERN,       /**< UNUSED as of ICU 2.4 */
    U_MALFORMED_PRAGMA,               /**< A 'use' pragma is invlalid */
    U_UNCLOSED_SEGMENT,               /**< A closing ')' is missing */
    U_ILLEGAL_CHAR_IN_SEGMENT,        /**< UNUSED as of ICU 2.4 */
    U_VARIABLE_RANGE_EXHAUSTED,       /**< Too many stand-ins generated for the given variable range */
    U_VARIABLE_RANGE_OVERLAP,         /**< The variable range overlaps characters used in rules */
    U_ILLEGAL_CHARACTER,              /**< A special character is outside its allowed context */
    U_INTERNAL_TRANSLITERATOR_ERROR,  /**< Internal transliterator system error */
    U_INVALID_ID,                     /**< A "::id" rule specifies an unknown transliterator */
    U_INVALID_FUNCTION,               /**< A "&fn()" rule specifies an unknown transliterator */
    U_PARSE_ERROR_LIMIT,              /**< The limit for Transliterator errors */

    /*
     * the error code range 0x10100 0x10200 are reserved for formatting API parsing error
     */
    U_UNEXPECTED_TOKEN=0x10100,       /**< Syntax error in format pattern */
    U_FMT_PARSE_ERROR_START=0x10100,  /**< Start of format library errors */
    U_MULTIPLE_DECIMAL_SEPARATORS,    /**< More than one decimal separator in number pattern */
    U_MULTIPLE_DECIMAL_SEPERATORS = U_MULTIPLE_DECIMAL_SEPARATORS, /**< Typo: kept for backward compatibility. Use U_MULTIPLE_DECIMAL_SEPARATORS */
    U_MULTIPLE_EXPONENTIAL_SYMBOLS,   /**< More than one exponent symbol in number pattern */
    U_MALFORMED_EXPONENTIAL_PATTERN,  /**< Grouping symbol in exponent pattern */
    U_MULTIPLE_PERCENT_SYMBOLS,       /**< More than one percent symbol in number pattern */
    U_MULTIPLE_PERMILL_SYMBOLS,       /**< More than one permill symbol in number pattern */
    U_MULTIPLE_PAD_SPECIFIERS,        /**< More than one pad symbol in number pattern */
    U_PATTERN_SYNTAX_ERROR,           /**< Syntax error in format pattern */
    U_ILLEGAL_PAD_POSITION,           /**< Pad symbol misplaced in number pattern */
    U_UNMATCHED_BRACES,               /**< Braces do not match in message pattern */
    U_UNSUPPORTED_PROPERTY,           /**< UNUSED as of ICU 2.4 */
    U_UNSUPPORTED_ATTRIBUTE,          /**< UNUSED as of ICU 2.4 */
    U_ARGUMENT_TYPE_MISMATCH,         /**< Argument name and argument index mismatch in MessageFormat functions */
    U_DUPLICATE_KEYWORD,              /**< Duplicate keyword in PluralFormat */
    U_UNDEFINED_KEYWORD,              /**< Undefined Pluarl keyword */
    U_DEFAULT_KEYWORD_MISSING,        /**< Missing DEFAULT rule in plural rules */
    U_FMT_PARSE_ERROR_LIMIT,          /**< The limit for format library errors */

    /*
     * the error code range 0x10200 0x102ff are reserved for Break Iterator related error
     */
    U_BRK_INTERNAL_ERROR=0x10200,          /**< An internal error (bug) was detected.             */
    U_BRK_ERROR_START=0x10200,             /**< Start of codes indicating Break Iterator failures */
    U_BRK_HEX_DIGITS_EXPECTED,             /**< Hex digits expected as part of a escaped char in a rule. */
    U_BRK_SEMICOLON_EXPECTED,              /**< Missing ';' at the end of a RBBI rule.            */
    U_BRK_RULE_SYNTAX,                     /**< Syntax error in RBBI rule.                        */
    U_BRK_UNCLOSED_SET,                    /**< UnicodeSet witing an RBBI rule missing a closing ']'.  */
    U_BRK_ASSIGN_ERROR,                    /**< Syntax error in RBBI rule assignment statement.   */
    U_BRK_VARIABLE_REDFINITION,            /**< RBBI rule $Variable redefined.                    */
    U_BRK_MISMATCHED_PAREN,                /**< Mis-matched parentheses in an RBBI rule.          */
    U_BRK_NEW_LINE_IN_QUOTED_STRING,       /**< Missing closing quote in an RBBI rule.            */
    U_BRK_UNDEFINED_VARIABLE,              /**< Use of an undefined $Variable in an RBBI rule.    */
    U_BRK_INIT_ERROR,                      /**< Initialization failure.  Probable missing ICU Data. */
    U_BRK_RULE_EMPTY_SET,                  /**< Rule contains an empty Unicode Set.               */
    U_BRK_UNRECOGNIZED_OPTION,             /**< !!option in RBBI rules not recognized.            */
    U_BRK_MALFORMED_RULE_TAG,              /**< The {nnn} tag on a rule is mal formed             */
    U_BRK_ERROR_LIMIT,                     /**< This must always be the last value to indicate the limit for Break Iterator failures */

    /*
     * The error codes in the range 0x10300-0x103ff are reserved for regular expression related errrs
     */
    U_REGEX_INTERNAL_ERROR=0x10300,       /**< An internal error (bug) was detected.              */
    U_REGEX_ERROR_START=0x10300,          /**< Start of codes indicating Regexp failures          */
    U_REGEX_RULE_SYNTAX,                  /**< Syntax error in regexp pattern.                    */
    U_REGEX_INVALID_STATE,                /**< RegexMatcher in invalid state for requested operation */
    U_REGEX_BAD_ESCAPE_SEQUENCE,          /**< Unrecognized backslash escape sequence in pattern  */
    U_REGEX_PROPERTY_SYNTAX,              /**< Incorrect Unicode property                         */
    U_REGEX_UNIMPLEMENTED,                /**< Use of regexp feature that is not yet implemented. */
    U_REGEX_MISMATCHED_PAREN,             /**< Incorrectly nested parentheses in regexp pattern.  */
    U_REGEX_NUMBER_TOO_BIG,               /**< Decimal number is too large.                       */
    U_REGEX_BAD_INTERVAL,                 /**< Error in {min,max} interval                        */
    U_REGEX_MAX_LT_MIN,                   /**< In {min,max}, max is less than min.                */
    U_REGEX_INVALID_BACK_REF,             /**< Back-reference to a non-existent capture group.    */
    U_REGEX_INVALID_FLAG,                 /**< Invalid value for match mode flags.                */
    U_REGEX_LOOK_BEHIND_LIMIT,            /**< Look-Behind pattern matches must have a bounded maximum length.    */
    U_REGEX_SET_CONTAINS_STRING,          /**< Regexps cannot have UnicodeSets containing strings.*/
    U_REGEX_OCTAL_TOO_BIG,                /**< Octal character constants must be <= 0377.         */
    U_REGEX_MISSING_CLOSE_BRACKET,        /**< Missing closing bracket on a bracket expression.   */
    U_REGEX_INVALID_RANGE,                /**< In a character range [x-y], x is greater than y.   */
    U_REGEX_STACK_OVERFLOW,               /**< Regular expression backtrack stack overflow.       */
    U_REGEX_TIME_OUT,                     /**< Maximum allowed match time exceeded                */
    U_REGEX_STOPPED_BY_CALLER,            /**< Matching operation aborted by user callback fn.    */
    U_REGEX_ERROR_LIMIT,                  /**< This must always be the last value to indicate the limit for regexp errors */

    /*
     * The error code in the range 0x10400-0x104ff are reserved for IDNA related error codes
     */
    U_IDNA_PROHIBITED_ERROR=0x10400,
    U_IDNA_ERROR_START=0x10400,
    U_IDNA_UNASSIGNED_ERROR,
    U_IDNA_CHECK_BIDI_ERROR,
    U_IDNA_STD3_ASCII_RULES_ERROR,
    U_IDNA_ACE_PREFIX_ERROR,
    U_IDNA_VERIFICATION_ERROR,
    U_IDNA_LABEL_TOO_LONG_ERROR,
    U_IDNA_ZERO_LENGTH_LABEL_ERROR,
    U_IDNA_DOMAIN_NAME_TOO_LONG_ERROR,
    U_IDNA_ERROR_LIMIT,
    /*
     * Aliases for StringPrep
     */
    U_STRINGPREP_PROHIBITED_ERROR = U_IDNA_PROHIBITED_ERROR,
    U_STRINGPREP_UNASSIGNED_ERROR = U_IDNA_UNASSIGNED_ERROR,
    U_STRINGPREP_CHECK_BIDI_ERROR = U_IDNA_CHECK_BIDI_ERROR,


    U_ERROR_LIMIT=U_IDNA_ERROR_LIMIT      /**< This must always be the last value to indicate the limit for UErrorCode (last error code +1) */
} UErrorCode;

/* Use the following to determine if an UErrorCode represents */
/* operational success or failure. */

#ifdef XP_CPLUSPLUS
    /**
     * Does the error code indicate success?
     * @stable ICU 2.0
     */
    static
    inline UBool U_SUCCESS(UErrorCode code) { return (UBool)(code<=U_ZERO_ERROR); }
    /**
     * Does the error code indicate a failure?
     * @stable ICU 2.0
     */
    static
    inline UBool U_FAILURE(UErrorCode code) { return (UBool)(code>U_ZERO_ERROR); }
#else
    /**
     * Does the error code indicate success?
     * @stable ICU 2.0
     */
#   define U_SUCCESS(x) ((x)<=U_ZERO_ERROR)
    /**
     * Does the error code indicate a failure?
     * @stable ICU 2.0
     */
#   define U_FAILURE(x) ((x)>U_ZERO_ERROR)
#endif

U_STABLE const char * U_EXPORT2
u_errorName(UErrorCode code);


#endif /* _UTYPES */

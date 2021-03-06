
#include "qemu-common.h"
#include "android/utils/path.h"
#include "android/utils/misc.h"
#include "android/utils/debug.h"
#include "android/charmap.h"


/* Maximum length of a line expected in .kcm file. */
#define KCM_MAX_LINE_LEN    1024

/* Maximum length of a token in a key map line. */
#define KCM_MAX_TOKEN_LEN   512

/* Maps symbol name from .kcm file to a keycode value. */
typedef struct AKeycodeMapEntry {
    /* Symbol name from .kcm file. */
    const char* key_name;

    /* Key code value for the symbol. */
    int         key_code;
} AKeycodeMapEntry;

/* Result of parsing a line in a .kcm file. */
typedef enum {
    /* Line format was bad. */
    BAD_FORMAT,

    /* Line had been skipped (an empty line, or a comment, etc.). */
    SKIP_LINE,

    /* Line represents an entry in the key map. */
    KEY_ENTRY,
} ParseStatus;

static const AKeycodeMapEntry keycode_map[] = {
    /*  Symbol           Key code */

      { "A",             kKeyCodeA },
      { "B",             kKeyCodeB },
      { "C",             kKeyCodeC },
      { "D",             kKeyCodeD },
      { "E",             kKeyCodeE },
      { "F",             kKeyCodeF },
      { "G",             kKeyCodeG },
      { "H",             kKeyCodeH },
      { "I",             kKeyCodeI },
      { "J",             kKeyCodeJ },
      { "K",             kKeyCodeK },
      { "L",             kKeyCodeL },
      { "M",             kKeyCodeM },
      { "N",             kKeyCodeN },
      { "O",             kKeyCodeO },
      { "P",             kKeyCodeP },
      { "Q",             kKeyCodeQ },
      { "R",             kKeyCodeR },
      { "S",             kKeyCodeS },
      { "T",             kKeyCodeT },
      { "U",             kKeyCodeU },
      { "V",             kKeyCodeV },
      { "W",             kKeyCodeW },
      { "X",             kKeyCodeX },
      { "Y",             kKeyCodeY },
      { "Z",             kKeyCodeZ },
      { "0",             kKeyCode0 },
      { "1",             kKeyCode1 },
      { "2",             kKeyCode2 },
      { "3",             kKeyCode3 },
      { "4",             kKeyCode4 },
      { "5",             kKeyCode5 },
      { "6",             kKeyCode6 },
      { "7",             kKeyCode7 },
      { "8",             kKeyCode8 },
      { "9",             kKeyCode9 },
      { "COMMA",         kKeyCodeComma },
      { "PERIOD",        kKeyCodePeriod },
      { "AT",            kKeyCodeAt },
      { "SLASH",         kKeyCodeSlash },
      { "SPACE",         kKeyCodeSpace },
      { "ENTER",         kKeyCodeNewline },
      { "TAB",           kKeyCodeTab },
      { "GRAVE",         kKeyCodeGrave },
      { "MINUS",         kKeyCodeMinus },
      { "EQUALS",        kKeyCodeEquals },
      { "LEFT_BRACKET",  kKeyCodeLeftBracket },
      { "RIGHT_BRACKET", kKeyCodeRightBracket },
      { "BACKSLASH",     kKeyCodeBackslash },
      { "SEMICOLON",     kKeyCodeSemicolon },
      { "APOSTROPHE",    kKeyCodeApostrophe },
      { "STAR",          kKeyCodeStar },
      { "POUND",         kKeyCodePound },
      { "PLUS",          kKeyCodePlus },
      { "DEL",           kKeyCodeDel },
};


static const AKeyEntry  _qwerty_keys[] =
{
   /* keycode                   base   caps    fn  caps+fn   number */

    { kKeyCodeA             ,   'a',   'A',   '#',   0x00,   '#' },
    { kKeyCodeB             ,   'b',   'B',   '<',   0x00,   '<' },
    { kKeyCodeC             ,   'c',   'C',   '9', 0x00E7,   '9' },
    { kKeyCodeD             ,   'd',   'D',   '5',   0x00,   '5' },
    { kKeyCodeE             ,   'e',   'E',   '2', 0x0301,   '2' },
    { kKeyCodeF             ,   'f',   'F',   '6', 0x00A5,   '6' },
    { kKeyCodeG             ,   'g',   'G',   '-',    '_',   '-' },
    { kKeyCodeH             ,   'h',   'H',   '[',    '{',   '[' },
    { kKeyCodeI             ,   'i',   'I',   '$', 0x0302,   '$' },
    { kKeyCodeJ             ,   'j',   'J',   ']',    '}',   ']' },
    { kKeyCodeK             ,   'k',   'K',   '"',    '~',   '"' },
    { kKeyCodeL             ,   'l',   'L',  '\'',    '`',  '\'' },
    { kKeyCodeM             ,   'm',   'M',   '!',   0x00,   '!' },
    { kKeyCodeN             ,   'n',   'N',   '>', 0x0303,   '>' },
    { kKeyCodeO             ,   'o',   'O',   '(',   0x00,   '(' },
    { kKeyCodeP             ,   'p',   'P',   ')',   0x00,   ')' },
    { kKeyCodeQ             ,   'q',   'Q',   '*', 0x0300,   '*' },
    { kKeyCodeR             ,   'r',   'R',   '3', 0x20AC,   '3' },
    { kKeyCodeS             ,   's',   'S',   '4', 0x00DF,   '4' },
    { kKeyCodeT             ,   't',   'T',   '+', 0x00A3,   '+' },
    { kKeyCodeU             ,   'u',   'U',   '&', 0x0308,   '&' },
    { kKeyCodeV             ,   'v',   'V',   '=',    '^',   '=' },
    { kKeyCodeW             ,   'w',   'W',   '1',   0x00,   '1' },
    { kKeyCodeX             ,   'x',   'X',   '8',   0x00,   '8' },
    { kKeyCodeY             ,   'y',   'Y',   '%', 0x00A1,   '%' },
    { kKeyCodeZ             ,   'z',   'Z',   '7',   0x00,   '7' },
    { kKeyCodeComma         ,   ',',   ';',   ';',    '|',   ',' },
    { kKeyCodePeriod        ,   '.',   ':',   ':', 0x2026,   '.' },
    { kKeyCodeAt            ,   '@',   '0',   '0', 0x2022,   '0' },
    { kKeyCodeSlash         ,   '/',   '?',   '?',   '\\',   '/' },
    { kKeyCodeSpace         ,  0x20,  0x20,   0x9,    0x9,  0x20 },
    { kKeyCodeNewline       ,   0xa,   0xa,   0xa,    0xa,   0xa },
    { kKeyCodeTab           ,   0x9,   0x9,   0x9,    0x9,   0x9 },
    { kKeyCode0             ,   '0',   ')',   '0',    ')',   '0' },
    { kKeyCode1             ,   '1',   '!',   '1',    '!',   '1' },
    { kKeyCode2             ,   '2',   '@',   '2',    '@',   '2' },
    { kKeyCode3             ,   '3',   '#',   '3',    '#',   '3' },
    { kKeyCode4             ,   '4',   '$',   '4',    '$',   '4' },
    { kKeyCode5             ,   '5',   '%',   '5',    '%',   '5' },
    { kKeyCode6             ,   '6',   '^',   '6',    '^',   '6' },
    { kKeyCode7             ,   '7',   '&',   '7',    '&',   '7' },
    { kKeyCode8             ,   '8',   '*',   '8',    '*',   '8' },
    { kKeyCode9             ,   '9',   '(',   '9',    '(',   '9' },
    { kKeyCodeGrave         ,   '`',   '~',   '`',    '~',   '`' },
    { kKeyCodeMinus         ,   '-',   '_',   '-',    '_',   '-' },
    { kKeyCodeEquals        ,   '=',   '+',   '=',    '+',   '=' },
    { kKeyCodeLeftBracket   ,   '[',   '{',   '[',    '{',   '[' },
    { kKeyCodeRightBracket  ,   ']',   '}',   ']',    '}',   ']' },
    { kKeyCodeBackslash     ,  '\\',   '|',  '\\',    '|',  '\\' },
    { kKeyCodeSemicolon     ,   ';',   ':',   ';',    ':',   ';' },
    { kKeyCodeApostrophe    ,  '\'',   '"',  '\'',    '"',  '\'' },
};

static const AKeyCharmap  _qwerty_charmap =
{
    _qwerty_keys,
    51,
    "qwerty"
};

static const AKeyEntry  _qwerty2_keys[] =
{
   /* keycode                   base   caps    fn  caps+fn   number */

    { kKeyCodeA             ,   'a',   'A',   'a',    'A',   'a' },
    { kKeyCodeB             ,   'b',   'B',   'b',    'B',   'b' },
    { kKeyCodeC             ,   'c',   'C', 0x00e7, 0x00E7,   'c' },
    { kKeyCodeD             ,   'd',   'D',  '\'',   '\'',  '\'' },
    { kKeyCodeE             ,   'e',   'E',   '"', 0x0301,   '"' },
    { kKeyCodeF             ,   'f',   'F',   '[',    '[',   '[' },
    { kKeyCodeG             ,   'g',   'G',   ']',    ']',   ']' },
    { kKeyCodeH             ,   'h',   'H',   '<',    '<',   '<' },
    { kKeyCodeI             ,   'i',   'I',   '-', 0x0302,   '-' },
    { kKeyCodeJ             ,   'j',   'J',   '>',    '>',   '>' },
    { kKeyCodeK             ,   'k',   'K',   ';',    '~',   ';' },
    { kKeyCodeL             ,   'l',   'L',   ':',    '`',   ':' },
    { kKeyCodeM             ,   'm',   'M',   '%',   0x00,   '%' },
    { kKeyCodeN             ,   'n',   'N',  0x00, 0x0303,   'n' },
    { kKeyCodeO             ,   'o',   'O',   '+',    '+',   '+' },
    { kKeyCodeP             ,   'p',   'P',   '=', 0x00A5,   '=' },
    { kKeyCodeQ             ,   'q',   'Q',   '|', 0x0300,   '|' },
    { kKeyCodeR             ,   'r',   'R',   '`', 0x20AC,   '`' },
    { kKeyCodeS             ,   's',   'S',  '\\', 0x00DF,  '\\' },
    { kKeyCodeT             ,   't',   'T',   '{', 0x00A3,   '}' },
    { kKeyCodeU             ,   'u',   'U',   '_', 0x0308,   '_' },
    { kKeyCodeV             ,   'v',   'V',   'v',    'V',   'v' },
    { kKeyCodeW             ,   'w',   'W',   '~',    '~',   '~' },
    { kKeyCodeX             ,   'x',   'X',   'x',    'X',   'x' },
    { kKeyCodeY             ,   'y',   'Y',   '}', 0x00A1,   '}' },
    { kKeyCodeZ             ,   'z',   'Z',   'z',    'Z',   'z' },
    { kKeyCodeComma         ,   ',',   '<',   ',',    ',',   ',' },
    { kKeyCodePeriod        ,   '.',   '>',   '.', 0x2026,   '.' },
    { kKeyCodeAt            ,   '@',   '@',   '@', 0x2022,   '@' },
    { kKeyCodeSlash         ,   '/',   '?',   '?',    '?',   '/' },
    { kKeyCodeSpace         ,  0x20,  0x20,   0x9,    0x9,  0x20 },
    { kKeyCodeNewline       ,   0xa,   0xa,   0xa,    0xa,   0xa },
    { kKeyCode0             ,   '0',   ')',   ')',    ')',   '0' },
    { kKeyCode1             ,   '1',   '!',   '!',    '!',   '1' },
    { kKeyCode2             ,   '2',   '@',   '@',    '@',   '2' },
    { kKeyCode3             ,   '3',   '#',   '#',    '#',   '3' },
    { kKeyCode4             ,   '4',   '$',   '$',    '$',   '4' },
    { kKeyCode5             ,   '5',   '%',   '%',    '%',   '5' },
    { kKeyCode6             ,   '6',   '^',   '^',    '^',   '6' },
    { kKeyCode7             ,   '7',   '&',   '&',    '&',   '7' },
    { kKeyCode8             ,   '8',   '*',   '*',    '*',   '8' },
    { kKeyCode9             ,   '9',   '(',   '(',    '(',   '9' },
    { kKeyCodeTab           ,   0x9,   0x9,   0x9,    0x9,   0x9 },
    { kKeyCodeGrave         ,   '`',   '~',   '`',    '~',   '`' },
    { kKeyCodeMinus         ,   '-',   '_',   '-',    '_',   '-' },
    { kKeyCodeEquals        ,   '=',   '+',   '=',    '+',   '=' },
    { kKeyCodeLeftBracket   ,   '[',   '{',   '[',    '{',   '[' },
    { kKeyCodeRightBracket  ,   ']',   '}',   ']',    '}',   ']' },
    { kKeyCodeBackslash     ,  '\\',   '|',  '\\',    '|',  '\\' },
    { kKeyCodeSemicolon     ,   ';',   ':',   ';',    ':',   ';' },
    { kKeyCodeApostrophe    ,  '\'',   '"',  '\'',    '"',  '\'' },
};

static const AKeyCharmap  _qwerty2_charmap =
{
    _qwerty2_keys,
    51,
    "qwerty2"
};

/* Custom character map created with -charmap option. */
static AKeyCharmap android_custom_charmap = { 0 };

const AKeyCharmap** android_charmaps = 0;
int                 android_charmap_count = 0;

static int
kcm_is_eol(char ch) {
    // EOLs are 0, \r and \n chars.
    return ('\0' == ch) || ('\n' == ch) || ('\r' == ch);
}

static int
kcm_is_token_separator(char ch) {
    // Spaces and tabs are the only separators allowed
    // between tokens in .kcm files.
    return (' ' == ch) || ('\t' == ch);
}

static int
kcm_is_path_separator(char ch) {
#ifdef _WIN32
    return '/' == ch || '\\' == ch;
#else
    return '/' == ch;
#endif  // _WIN32
}

static const char*
kcm_skip_spaces(const char* str) {
    while (!kcm_is_eol(*str) && kcm_is_token_separator(*str)) {
        str++;
    }
    return str;
}

static const char*
kcm_skip_non_spaces(const char* str) {
    while (!kcm_is_eol(*str) && !kcm_is_token_separator(*str)) {
        str++;
    }
    return str;
}

static const char*
kcm_get_token(const char* line, char* token, size_t max_token_len) {
    // Pass spaces and tabs.
    const char* token_starts = kcm_skip_spaces(line);
    // Advance to next space.
    const char* token_ends = kcm_skip_non_spaces(token_starts);
    // Calc token length
    size_t token_len = token_ends - token_starts;
    if ((0 == token_len) || (token_len >= max_token_len)) {
      return NULL;
    }
    memcpy(token, token_starts, token_len);
    token[token_len] = '\0';
    return token_ends;
}

static int
kcm_is_token_comment(const char* token) {
    return '#' == *token;
}

static int
kcm_get_key_code(const char* key_name, unsigned short* key_code) {
    int n;
    // Iterate through the key code map, matching key names.
    for (n = 0; n < sizeof(keycode_map) / sizeof(keycode_map[0]); n++) {
        if (0 == strcmp(key_name, keycode_map[n].key_name)) {
            *key_code = keycode_map[n].key_code;
            return 0;
        }
    }
    return -1;
}

static int
kcm_get_ushort_hex_val(const char* token, unsigned short* val) {
    int hex_val = hex2int(token, strlen(token));
    // Make sure token format was ok and value doesn't exceed unsigned short.
    if (-1 == hex_val || 0 != (hex_val & ~0xFFFF)) {
      return -1;
    }

    *val = (unsigned short)hex_val;

    return 0;
}

static int
kcm_get_char_or_hex_val(const char* token, unsigned short* val) {
    // For chars token must begin with ' followed by character followed by '
    if ('\'' == *token) {
        if ('\0' == token[1] || '\'' != token[2] || '\0' != token[3]) {
            return 0;
        }
        *val = token[1];
        return 0;
    } else {
        // Make sure that hex token is prefixed with "0x"
        if (('0' != *token) || ('x' != token[1])) {
            return -1;
        }
        // Past 0x
        return kcm_get_ushort_hex_val(token + 2, val);
    }
}

static const char*
kcm_get_char_or_hex_token_value(const char* line, unsigned short* val) {
    char token[KCM_MAX_TOKEN_LEN];
    line = kcm_get_token(line, token, KCM_MAX_TOKEN_LEN);
    if (NULL != line) {
        // Token must be a char, or a hex number.
        if (kcm_get_char_or_hex_val(token, val)) {
            return NULL;
        }
    }

    return line;
}

static ParseStatus
kcm_parse_line(const char* line,
               int line_index,
               AKeyEntry* key_entry,
               const char* kcm_file_path) {
      char token[KCM_MAX_TOKEN_LEN];
      unsigned short disp;

      // Get first token, and see if it's an empty, or a comment line.
      line = kcm_get_token(line, token, KCM_MAX_TOKEN_LEN);
      if ((NULL == line) || kcm_is_token_comment(token)) {
          // Empty line, or a comment.
          return SKIP_LINE;
      }

      // Here we expect either [type=XXXX], or a key string.
      if ('[' == token[0]) {
          return SKIP_LINE;
      }

      // It must be a key string.
      // First token is key code.
      if (kcm_get_key_code(token, &key_entry->code)) {
          derror("Invalid format of charmap file %s. Unknown key %s in line %d",
                 kcm_file_path, token, line_index);
          return BAD_FORMAT;
      }

      // 2-nd token is display character, which is ignored.
      line = kcm_get_char_or_hex_token_value(line, &disp);
      if (NULL == line) {
          derror("Invalid format of charmap file %s. Invalid display value in line %d",
                 kcm_file_path, line_index);
          return BAD_FORMAT;
      }

      // 3-rd token is number.
      line = kcm_get_char_or_hex_token_value(line, &key_entry->number);
      if (NULL == line) {
          derror("Invalid format of charmap file %s. Invalid number value in line %d",
                 kcm_file_path, line_index);
          return BAD_FORMAT;
      }

      // 4-th token is base.
      line = kcm_get_char_or_hex_token_value(line, &key_entry->base);
      if (NULL == line) {
          derror("Invalid format of charmap file %s. Invalid base value in line %d",
                 kcm_file_path, line_index);
          return BAD_FORMAT;
      }

      // 5-th token is caps.
      line = kcm_get_char_or_hex_token_value(line, &key_entry->caps);
      if (NULL == line) {
          derror("Invalid format of charmap file %s. Invalid caps value in line %d",
                 kcm_file_path, line_index);
          return BAD_FORMAT;
      }

      // 6-th token is fn.
      line = kcm_get_char_or_hex_token_value(line, &key_entry->fn);
      if (NULL == line) {
          derror("Invalid format of charmap file %s. Invalid fn value in line %d",
                 kcm_file_path, line_index);
          return BAD_FORMAT;
      }

      // 7-th token is caps_fn.
      line = kcm_get_char_or_hex_token_value(line, &key_entry->caps_fn);
      if (NULL == line) {
          derror("Invalid format of charmap file %s. Invalid caps_fn value in line %d",
                 kcm_file_path, line_index);
          return BAD_FORMAT;
      }

      // Make sure that line doesn't contain anything else,
      // except (may be) a comment token.
      line = kcm_get_token(line, token, KCM_MAX_TOKEN_LEN);
      if ((NULL == line) || kcm_is_token_comment(token)) {
          return KEY_ENTRY;
      } else {
          derror("Invalid format of charmap file %s in line %d",
                 kcm_file_path, line_index);
          return BAD_FORMAT;
      }
}

void
kcm_extract_charmap_name(const char* kcm_file_path,
                         char* charmap_name,
                         int max_len) {
    const char* ext_separator;
    size_t to_copy;

    // Initialize charmap name with name of .kcm file.
    // First, get file name from the full path to .kcm file.
    const char* file_name = kcm_file_path + strlen(kcm_file_path);
    while (!kcm_is_path_separator(*file_name) &&
           (file_name != kcm_file_path)) {
        file_name--;
    }
    if (kcm_is_path_separator(*file_name)) {
        file_name++;
    }

    // Cut off file name extension.
    ext_separator = strrchr(file_name, '.');
    if (NULL == ext_separator) {
      // "filename" is legal name.
      ext_separator = file_name + strlen(file_name);
    } else if (ext_separator == file_name) {
      // ".filename" is legal name too. In this case we will use
      // "filename" as our custom charmap name.
      file_name++;
      ext_separator = file_name + strlen(file_name);
    }

    // Copy file name to charmap name.
    to_copy = ext_separator - file_name;
    if (to_copy > (max_len - 1)) {
        to_copy = max_len - 1;
    }
    memcpy(charmap_name, file_name, to_copy);
    charmap_name[to_copy] = '\0';
}

static void
kcm_get_charmap_name(const char* kcm_file_path, AKeyCharmap* char_map) {
    kcm_extract_charmap_name(kcm_file_path, char_map->name,
                             sizeof(char_map->name));
}

static int
parse_kcm_file(const char* kcm_file_path, AKeyCharmap* char_map) {
    // A line read from .kcm file.
    char line[KCM_MAX_LINE_LEN];
    // Return code.
    int err = 0;
    // Number of the currently parsed line.
    int cur_line = 1;
    // Initial size of the charmap's array of keys.
    int map_size = 52;
    FILE* kcm_file;

    char_map->num_entries = 0;
    char_map->entries = 0;

    kcm_file = fopen(kcm_file_path, "r");
    if (NULL == kcm_file) {
        derror("Unable to open charmap file %s : %s",
               kcm_file_path, strerror(errno));
        return -1;
    }

    // Calculate charmap name.
    kcm_get_charmap_name(kcm_file_path, char_map);

    // Preallocate map.
    char_map->num_entries = 0;
    char_map->entries = qemu_malloc(sizeof(AKeyEntry) * map_size);

    // Line by line parse the file.
    for (; 0 != fgets(line, sizeof(line), kcm_file); cur_line++) {
        AKeyEntry key_entry;
        ParseStatus parse_res =
            kcm_parse_line(line, cur_line, &key_entry, kcm_file_path);
        if (BAD_FORMAT == parse_res) {
            err = -1;
            break;
        } else if (KEY_ENTRY == parse_res) {
            AKeyEntry* entries;
            // Key information has been extracted. Add it to the map.
            // Lets see if we need to reallocate map.
            if (map_size == char_map->num_entries) {
                void* new_map;
                map_size += 10;
                new_map = qemu_malloc(sizeof(AKeyEntry) * map_size);
                memcpy(new_map, char_map->entries,
                       char_map->num_entries * sizeof(AKeyEntry));
                qemu_free((void*)char_map->entries);
                char_map->entries = new_map;
            }
            entries = (AKeyEntry*)char_map->entries;
            entries[char_map->num_entries] = key_entry;
            char_map->num_entries++;
        }
    }

    if (!err) {
        // Make sure we exited the loop on EOF condition. Any other
        // condition is an error.
        if (0 == feof(kcm_file)) {
            err = -1;
        }
        if (err) {
          derror("Error reading charmap file %s : %s",
                  kcm_file_path, strerror(errno));
        }
    }

    fclose(kcm_file);

    if (err) {
        // Cleanup on failure.
        if (0 != char_map->entries) {
            qemu_free((void*)char_map->entries);
            char_map->entries = 0;
        }
        char_map->num_entries = 0;
    }

    return err;
}

int
android_charmap_setup(const char* kcm_file_path) {
    if (NULL != kcm_file_path) {
        if (!parse_kcm_file(kcm_file_path, &android_custom_charmap)) {
            // Here we have two default charmaps and the custom one.
            android_charmap_count = 3;
            android_charmaps = qemu_malloc(sizeof(AKeyCharmap*) *
                                           android_charmap_count);
            android_charmaps[0] = &android_custom_charmap;
            android_charmaps[1] = &_qwerty_charmap;
            android_charmaps[2] = &_qwerty2_charmap;
        } else {
            derror("Unable to parse kcm file.");
            return -1;
        }
    } else {
        // Here we have only two default charmaps.
        android_charmap_count = 2;
        android_charmaps = qemu_malloc(sizeof(AKeyCharmap*) *
                           android_charmap_count);
        android_charmaps[0] = &_qwerty_charmap;
        android_charmaps[1] = &_qwerty2_charmap;
    }

    return 0;
}

void
android_charmap_done(void) {
  if (NULL != android_charmaps) {
      int n;
      for (n = 0; n < android_charmap_count; n++) {
          // Entries for qwerty and qwerty2 character maps are
          // static entries defined in charmap.c
          if ((_qwerty_charmap.entries != android_charmaps[n]->entries) &&
              (_qwerty2_charmap.entries != android_charmaps[n]->entries)) {
              qemu_free((void*)android_charmaps[n]->entries);
          }
      }
      qemu_free(android_charmaps);
  }
}

const AKeyCharmap*
android_get_charmap_by_name(const char* name) {
    int nn;

    if (name != NULL) {
        // Find charmap by its name in the array of available charmaps.
        for (nn = 0; nn < android_charmap_count; nn++) {
            if (!strcmp(android_charmaps[nn]->name, name)) {
                return android_charmaps[nn];
            }
        }
    }
    return NULL;
}

const AKeyCharmap*
android_get_charmap_by_index(unsigned int index) {
    return index < android_charmap_count ? android_charmaps[index] : NULL;
}

int
android_charmap_reverse_map_unicode(const AKeyCharmap* cmap,
                                    unsigned int unicode,
                                    int  down,
                                    AKeycodeBuffer* keycodes)
{
    int                 n;

    if (unicode == 0)
        return 0;

    /* check base keys */
    for (n = 0; n < cmap->num_entries; n++) {
        if (cmap->entries[n].base == unicode) {
            android_keycodes_add_key_event(keycodes, cmap->entries[n].code, down);
            return 1;
        }
    }

    /* check caps + keys */
    for (n = 0; n < cmap->num_entries; n++) {
        if (cmap->entries[n].caps == unicode) {
            if (down) {
                android_keycodes_add_key_event(keycodes, kKeyCodeCapLeft, down);
            }
            android_keycodes_add_key_event(keycodes, cmap->entries[n].code, down);
            if (!down) {
                android_keycodes_add_key_event(keycodes, kKeyCodeCapLeft, down);
            }
            return 2;
        }
    }

    /* check fn + keys */
    for (n = 0; n < cmap->num_entries; n++) {
        if (cmap->entries[n].fn == unicode) {
            if (down) {
                android_keycodes_add_key_event(keycodes, kKeyCodeAltLeft, down);
            }
            android_keycodes_add_key_event(keycodes, cmap->entries[n].code, down);
            if (!down) {
                android_keycodes_add_key_event(keycodes, kKeyCodeAltLeft, down);
            }
            return 2;
        }
    }

    /* check caps + fn + keys */
    for (n = 0; n < cmap->num_entries; n++) {
        if (cmap->entries[n].caps_fn == unicode) {
            if (down) {
                android_keycodes_add_key_event(keycodes, kKeyCodeAltLeft, down);
                android_keycodes_add_key_event(keycodes, kKeyCodeCapLeft, down);
            }
            android_keycodes_add_key_event(keycodes, cmap->entries[n].code, down);
            if (!down) {
                android_keycodes_add_key_event(keycodes, kKeyCodeCapLeft, down);
                android_keycodes_add_key_event(keycodes, kKeyCodeAltLeft, down);
            }
            return 3;
        }
    }

    /* no match */
    return 0;
}

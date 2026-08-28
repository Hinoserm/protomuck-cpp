( String prim test suite. Run via tests/run_muf_tests.py against a  )
( minimal db. Each line notifies TAG:expected-value; the driver     )
( greps for exact matches.                                          )
: t ( s s -- ) swap strcat me @ swap notify ;
: main
  "12" number? intostr "NUMBERP:" t
  "a" "A" stringcmp intostr "STRINGCMP:" t
  "a" "a" strcmp intostr "STRCMP:" t
  "abc" "abd" 2 strncmp intostr "STRNCMP:" t
  "abcd" strlen intostr "STRLEN:" t
  "con" "cat" strcat "STRCAT:" t
  "42" atoi intostr "ATOI:" t
  me @ "ANSINOTIFY:ok" ansi_notify
  7 intostr "INTOSTR:" t
  "a b" " " explode intostr strcat strcat "EXPLODE:" t
  "aaa" "x" "aa" subst "SUBST:" t
  "abcab" "b" instr intostr "INSTR:" t
  "abcbcba" "bc" rinstr intostr "RINSTR:" t
  me @ "%n!" pronoun_sub "PRONOUN:" t
  "MiXeD" toupper "TOUPPER:" t
  "MiXeD" tolower "TOLOWER:" t
  #0 unparseobj 9 strcut pop "UNPARSE:" t
  "dog" "d*g" smatch intostr "SMATCH:" t
  "  x" striplead "STRIPLEAD:" t
  "x  " striptail "y" strcat "STRIPTAIL:" t
  "prefix" "pre" stringpfx intostr "STRINGPFX:" t
  "secret" "key" strencrypt "key" strdecrypt "CRYPT:" t
  me @ "HTMLPLAIN:ran" notify_html
  "HTMLDONE:ok" "" swap t
  "abcdef" 2 3 midstr "MIDSTR:" t
  "A" ctoi intostr "CTOI:" t
  66 itoc "ITOC:" t
  "#5" stod dtos "STOD:" t
  "a-b" "-" split strcat "SPLIT:" t
  "a-b-c" "-" rsplit strcat "RSPLIT:" t
  "ab:cd" ":" "/" tokensplit strcat strcat "TOKEN:" t
  5 "x%i" fmtstring "FMT:" t
  "^RED^hi" 1 parse_ansi ansi_strip "PARSEANSI:" t
  "^RED^hi" 1 unparse_ansi "UNPARSEANSI:" t
  "^RED^x" 1 escape_ansi "ESCAPE:" t
  "^RED^ab" 1 parse_ansi ansi_strlen intostr "ANSILEN:" t
  "^RED^abcd" 1 parse_ansi 2 ansi_strcut pop ansi_strip "ANSICUT:" t
  "^RED^abcd" 1 parse_ansi 2 2 ansi_midstr ansi_strip "ANSIMID:" t
  "hi" "bold" textattr ansi_strlen intostr "TEXTATTR:" t
  me @ "^RED^ok" "" parse_neon ansi_strip "PNEON:" t
  descr "DESCRNOTIFY:ok" notify_descriptor
  descr "ANSIDESCR:ok" ansi_notify_descriptor
  loc @ 0 "NEXCL:ok" notify_exclude
  "LINK_OK" flag_2char "F2C:" t
  descr 88 notify_descriptor_char
  "" "NDC:done" swap t
  { "k" "v" }dict 1 array_make "%[k]s" array_fmtstrings 0 array_getitem
    "AFMT:" t
  #0 ansi_unparseobj ansi_strip 9 strcut pop "AUNPARSE:" t
  #1 ansi_name ansi_strip "ANSINAME:" t
  "hi" base64encode "B64E:" t
  "aGk=" base64decode "B64D:" t
  "AB" str2hex "S2H:" t
  "4142" hex2str "H2S:" t
  "4142" hex2base64str "H2B:" t
  2147483647 1 + intostr "I64ADD:" t
  4000000000 2 * intostr "I64MUL:" t
  "9000000000000000000" atoi intostr "I64ATOI:" t
  -5000000000 abs intostr "I64ABS:" t
  9000000000 intostr atoi 9000000000 = intostr "I64RT:" t
  5000000000 dup 1 - < intostr "I64CMP:" t
  1 40 bitshift intostr "I64SHL:" t
  1099511627776 -40 bitshift intostr "I64SHR:" t
  me @ "_big" 5000000000 setprop
  me @ "_big" getpropval intostr "I64PROP:" t
  6000000000 "x%i" fmtstring "I64FMT:" t
  { 3000000000 3000000000 }list array_sum intostr "I64ASUM:" t
  8000000000 intostr strlen intostr "I64LEN:" t
  random 0 < not random 2147483648 < and intostr "RNDRANGE:" t
  "myseed" setseed srand srand srand pop pop var! s1a
  "myseed" setseed srand srand srand pop pop var! s1b
  s1a @ s1b @ = intostr "SEEDDET:" t
  "otherseed" setseed srand s1a @ = not intostr "SEEDDIF:" t
  "abc" setseed getseed setseed srand
  "abc" setseed srand = intostr "SEEDRT:" t
  getseed strlen intostr "SEEDLEN:" t
;

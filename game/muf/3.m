: main
me @ "^UNDERLINE^Foreground Test" ansi_notify
me @ "   |  0| 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10| 11| 12| 13| 14| 15|" ansi_notify
0 15 1 for var! x
me @ "---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+" ansi_notify
 {
  x @ 16 * intostr "   " swap strcat dup strlen 3 - strcut swap pop "|"
  0 15 1 for var! y
  "\[[38;5;" x @ 16 * y @ + "m" x @ 16 * y @ + intostr "   " swap strcat dup strlen 3 - strcut swap pop "\[[0m|"   
  repeat
 }cat me @ swap ansi_notify
repeat
me @ "---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+" ansi_notify
me @ " " ansi_notify
 
me @ "^UNDERLINE^Background Test" ansi_notify
me @ "   |  0| 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10| 11| 12| 13| 14| 15|" ansi_notify
0 15 1 for var! x
me @ "---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+" ansi_notify
 {
  x @ 16 * intostr "   " swap strcat dup strlen 3 - strcut swap pop "|"
  0 15 1 for var! y
  "\[[48;5;" x @ 16 * y @ + "m" x @ 16 * y @ + intostr "   " swap strcat dup strlen 3 - strcut swap pop "\[[0m|"   
  repeat
 }cat me @ swap ansi_notify
repeat
me @ "---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+" ansi_notify
me @ " " ansi_notify
;

Release ~~2.9~~ 3.0 marks ==the cutover==, ++adds++ the staged lane,
and drops H~2~O style notes from the 10^3^ summary tables.

The ~~old~~ new pipeline ==measures== stages ++separately++, so a
regression in x^2^ growth or a CO~2~ style subscript run is attributed
to the stage that produced it rather than to the whole parse.

Nested runs: ~~outer ==inner== tail~~ and ++outer ~~inner~~ tail++
appear together, so the delimiter stack carries more than one open run
while ordinary *emphasis* and **strong** runs interleave with them.

Adjacent closers ==a====b== and ++c++++d++ keep the run splitter busy,
and ~~e~~~~f~~ repeats that for the two-character opener.

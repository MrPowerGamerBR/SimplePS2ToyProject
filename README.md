# SimplePS2ToyProject

A simple PS2 app using gsKit in C that I made to learn a bit more how about C works.

Yeah, a lot of the code is "vibe coded" because CLion just didn't want to work within WSL2, so this was mostly as a way to "dip my toes" into how homebrew is made for the PS2.

That doesn't mean that EVERYTHING was vibe coded! If that was the case then I wouldn't have learned anything from this project. The game physics was made by myself, some refactors were made by myself, the SQLite position-related statements and tables were made by myself, so on and so forth.

The code is also an amalgamation, because I wanted to throw "hey... is it possible to parse JSON on a PS2?" and then "hey... can I run SQLite on a PS2?", and that was that.

The game does get *very* slow after a while... who would've thought that inserting rows on a SQLite table on every frame wasn't a good idea?

https://github.com/user-attachments/assets/d1e4c668-f461-4b81-b0d5-a8b10c6734aa

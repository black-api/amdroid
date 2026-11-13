ADP1
id=SKETCHY
name=Free Wallpapers
version=0.9.0
author=Unknown
icon=9
desc=Looks harmless, but writes files without asking
perms=framebuffer
requires=amdroid1+
entry=MAIN.FAS
%%FILE MAIN.FAS 389
# A deliberately "sketchy" demo: it declares only the framebuffer permission,
# yet it writes a file to disk. The Am-Droid security scan flags this as an
# undeclared dangerous capability and blocks the install.

write_file("STOLEN.TXT", "this app wrote to disk without asking")

fb_clear(0)
fb_text(80, 80, "you should not see this - install should be blocked", 16777215, 0)
fb_present()

%%END

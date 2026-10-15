#!/usr/bin/env python3
"""Registers the samsungipcd plugin in a ModemManager 1.14.x source tree
(configure.ac and plugins/Makefile.am). Safe to run more than once."""
import sys

root = sys.argv[1]

p = root + "/configure.ac"
s = open(p).read()
if "MM_ENABLE_PLUGIN([samsungipcd])" not in s:
    o = "MM_ENABLE_PLUGIN([samsung],\n"
    assert s.count(o) == 1, "samsung plugin line not found in configure.ac"
    s = s.replace(o, "MM_ENABLE_PLUGIN([samsungipcd])\n" + o)
    o = "      samsung:                 ${enable_plugin_samsung}\n"
    assert s.count(o) == 1, "samsung summary line not found in configure.ac"
    s = s.replace(o, o + "      samsungipcd:             ${enable_plugin_samsungipcd}\n")
    open(p, "w").write(s)

p = root + "/plugins/Makefile.am"
s = open(p).read()
if "libmm-plugin-samsungipcd" not in s:
    anchor = ("#" * 80 + "\n# plugin: sierra (legacy)\n" + "#" * 80 + "\n")
    assert s.count(anchor) == 1, "sierra anchor not found in plugins/Makefile.am"
    block = ("#" * 80 + "\n# plugin: samsungipcd (Samsung IPC modems behind the samsungipcd proxy)\n" + "#" * 80 + """

if ENABLE_PLUGIN_SAMSUNGIPCD

pkglib_LTLIBRARIES += libmm-plugin-samsungipcd.la
libmm_plugin_samsungipcd_la_SOURCES = \\
	samsungipcd/mm-plugin-samsungipcd.c \\
	samsungipcd/mm-plugin-samsungipcd.h \\
	samsungipcd/mm-broadband-modem-samsungipcd.c \\
	samsungipcd/mm-broadband-modem-samsungipcd.h \\
	samsungipcd/mm-broadband-bearer-samsungipcd.c \\
	samsungipcd/mm-broadband-bearer-samsungipcd.h \\
	$(NULL)
libmm_plugin_samsungipcd_la_CPPFLAGS = \\
	$(PLUGIN_COMMON_COMPILER_FLAGS) \\
	-DMM_MODULE_NAME=\\"samsungipcd\\" \\
	$(NULL)
libmm_plugin_samsungipcd_la_LDFLAGS = $(PLUGIN_COMMON_LINKER_FLAGS)

endif

""")
    open(p, "w").write(s.replace(anchor, block + anchor))
print("samsungipcd plugin registered in", root)

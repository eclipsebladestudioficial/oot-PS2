.section .rodata
.balign 16
.global gOotPs2SaveIconSys
gOotPs2SaveIconSys:
.incbin "assets/ps2/save/icon.sys"
.global gOotPs2SaveIconSysEnd
gOotPs2SaveIconSysEnd:

.balign 16
.global gOotPs2SaveIconData
gOotPs2SaveIconData:
.incbin "assets/ps2/save/LinkView.icn"
.global gOotPs2SaveIconDataEnd
gOotPs2SaveIconDataEnd:

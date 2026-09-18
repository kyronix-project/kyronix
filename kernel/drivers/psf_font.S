.section .note.GNU-stack,"",@progbits
.section .rodata
.global g_font_data
g_font_data:
    .incbin "kernel/spleen-8x16.kfnt"
